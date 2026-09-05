#include "serve.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

#include <windows.h>

#include "morph/core/ipc/pipe.h"
#include "morph/core/ipc/worker_protocol.h"
#include "morph/core/planner.h"
#include "morph/core/stream.h"
#include "morph/core/temp_store.h"

namespace morph::worker {
namespace {

using namespace morph::ipc;

// Section 11.4: "the broker coalesces per job to 10 Hz before fan-out". The
// worker throttles at the same rate on the way out, because a provider free to
// report as often as it likes would otherwise put a frame on the pipe per
// decoded row for the broker to throw away.
constexpr auto kProgressInterval = std::chrono::milliseconds(100);

// UTF-8 from the wire to a filesystem path. std::filesystem::u8path would say
// this in one line and is deprecated in C++20 with no replacement that does not
// involve char8_t, so the conversion is written out.
std::filesystem::path PathFrom(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                             static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return std::filesystem::path(utf8);
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                          needed);
    return std::filesystem::path(wide);
}

PathHint HintFor(const std::filesystem::path& path) {
    PathHint hint;
    hint.filename = path.filename().string();
    hint.extension = path.extension().string();
    for (char& c : hint.extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return hint;
}

// Sends Progress frames as the conversion runs.
//
// This is the only thing that writes to the pipe while a task is in flight, and
// it does so from the thread running the conversion. The reader thread never
// writes, so the pipe has exactly one writer at any moment -- which is what
// makes a duplex pipe safe to use from two threads at once.
class PipeProgress final : public IProgressSink {
public:
    PipeProgress(Connection& connection, std::uint64_t jobId, std::uint32_t taskIndex)
        : connection_(&connection), jobId_(jobId), taskIndex_(taskIndex) {}

    void Report(double fraction, std::uint64_t bytesWritten) override {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_ < kProgressInterval) return;
        last_ = now;

        Progress message;
        message.jobId = jobId_;
        message.taskIndex = taskIndex_;
        message.fraction = fraction;
        message.bytesWritten = bytesWritten;
        // A failed send is not worth reporting: the broker has gone away, and
        // the conversion will find that out when it tries to send TaskDone.
        (void)connection_->Send(message);
    }

private:
    Connection* connection_;
    std::uint64_t jobId_;
    std::uint32_t taskIndex_;
    std::chrono::steady_clock::time_point last_{};
};

// --- The requests, and the thread that reads them ----------------------------

// One frame the reader thread handed to the main thread. Cancel never gets here:
// it is applied on the reader thread, which is the whole reason there are two.
struct Request {
    MessageType type = MessageType::kInvalid;
    std::vector<std::byte> payload;
};

class Inbox {
public:
    void Push(Request request) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(request));
        }
        ready_.notify_one();
    }

    // Marks the channel finished. A main thread waiting in Pop wakes and is told
    // there is nothing more coming.
    void Finish() {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            finished_ = true;
        }
        ready_.notify_all();
    }

    [[nodiscard]] bool Pop(Request& into) {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return finished_ || !queue_.empty(); });
        // Drain before quitting: a request that arrived in the same breath as the
        // pipe closing is still worth answering if the answer can be written.
        if (queue_.empty()) return false;
        into = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Request> queue_;
    bool finished_ = false;
};

// The stop_source of the task currently running, if any. Section 11.3's
// cooperative level: the reader thread requests the stop and the provider
// notices within 100 ms.
class CurrentTask {
public:
    void Begin(std::uint64_t jobId, std::uint32_t taskIndex, std::stop_source source) {
        const std::lock_guard<std::mutex> lock(mutex_);
        jobId_ = jobId;
        taskIndex_ = taskIndex;
        source_ = std::move(source);
        running_ = true;
    }

    void End() {
        const std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        source_ = std::stop_source();
    }

    void Cancel(const ipc::Cancel& message) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        if (message.jobId != 0 && message.jobId != jobId_) return;
        if (message.taskIndex >= 0 &&
            static_cast<std::uint32_t>(message.taskIndex) != taskIndex_) {
            return;
        }
        source_.request_stop();
    }

    // Used when the pipe dies: whatever is running has nowhere to report to.
    void CancelAll() {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (running_) source_.request_stop();
    }

private:
    std::mutex mutex_;
    std::stop_source source_;
    std::uint64_t jobId_ = 0;
    std::uint32_t taskIndex_ = 0;
    bool running_ = false;
};

// --- Handlers ----------------------------------------------------------------

GraphDescription DescribeGraphOf(const Providers& providers) {
    const FormatGraph& graph = providers.graph();

    GraphDescription description;
    description.capabilities = providers.capabilities().All();

    for (std::size_t index = 0; index < graph.NodeCount(); ++index) {
        const NodeId from = static_cast<NodeId>(index);
        for (const EdgeId edge : graph.Outgoing(from)) {
            const EdgeCost& cost = graph.Cost(edge);
            EdgeWire wire;
            wire.from = graph.Node(from).Id();
            wire.to = graph.Node(graph.To(edge)).Id();
            wire.provider = cost.provider;
            wire.fidelity = static_cast<std::uint8_t>(cost.fidelity);
            wire.generational = cost.generational;
            wire.cpuPerMiB = cost.cpuPerMiB;
            wire.memPeakFactor = cost.memPeakFactor;
            wire.requiresDisk = cost.requiresDisk;
            description.edges.push_back(std::move(wire));
        }
    }
    return description;
}

ProbeResult ProbeAll(Providers& providers, const ProbeFiles& request) {
    ProbeResult result;
    result.profiles.reserve(request.paths.size());

    for (const std::string& path : request.paths) {
        // One entry per request entry, always, so the broker can match answers
        // to questions by position. An unreadable or unidentifiable file is an
        // empty string, not a missing element.
        const std::filesystem::path file = PathFrom(path);
        try {
            FileSource source(file);
            const std::optional<MediaProfile> profile =
                providers.executor().Probe(source, HintFor(file));
            result.profiles.push_back(profile ? profile->Id() : std::string());
        } catch (const std::exception&) {
            // The comment above was a promise this code did not keep. FileSource
            // THROWS when a path cannot be opened -- deleted between the drop and
            // the probe, locked, on a share that went away -- and nothing caught
            // it, so one missing file in a batch unwound the dispatch loop past a
            // joinable reader thread and terminated the worker. An unopenable
            // file is exactly the "unreadable" case the promise covers.
            result.profiles.emplace_back();
        }
    }
    return result;
}

// A provider threw. The broker is waiting for a reply of a particular shape and
// silence would hang the job, so every request type that has an answer gets one
// -- with the same "one entry per request entry" invariant the happy path keeps.
[[nodiscard]] Result ReportThrow(Connection& connection, const Request& request,
                                 std::string_view what) {
    std::fprintf(stderr, "morph.worker: %s threw: %.*s\n",
                 std::string(Name(request.type)).c_str(), static_cast<int>(what.size()),
                 what.data());

    switch (request.type) {
        case MessageType::kProbeFiles: {
            ProbeFiles probe;
            ProbeResult result;
            if (Decode(request.payload, probe)) result.profiles.resize(probe.paths.size());
            return connection.Send(result);
        }
        case MessageType::kRunTask: {
            RunTask task;
            if (!Decode(request.payload, task)) return Result::Ok();
            TaskDone done;
            done.jobId = task.jobId;
            done.taskIndex = task.taskIndex;
            done.status = static_cast<std::uint8_t>(Status::kInternal);
            done.inputPath = task.inputPath;
            done.error = "Morph hit an internal error converting this file: " + std::string(what);
            return connection.Send(done);
        }
        default:
            // DescribeGraph has no failure shape, and a broker that gets no
            // graph reports that itself.
            return Result::Ok();
    }
}

TaskDone Fail(const RunTask& task, Status status, std::string message) {
    TaskDone done;
    done.jobId = task.jobId;
    done.taskIndex = task.taskIndex;
    done.status = static_cast<std::uint8_t>(status);
    done.inputPath = task.inputPath;
    done.error = std::move(message);
    return done;
}

// morph::OptionValue (a variant, in options.h) and morph::ipc::OptionValue (four
// flat fields, on the wire) are two spellings of the same idea, and this is the
// one place they meet. The wire shape is flat because section 12.3 requires the
// protocol to be readable from a language without variants.
OptionBag BagFrom(const std::vector<ipc::OptionValue>& options) {
    OptionBag bag;
    for (const ipc::OptionValue& option : options) {
        switch (option.kind) {
            case 0: bag.Set(option.key, option.boolValue); break;
            case 1: bag.Set(option.key, option.intValue); break;
            case 2: bag.Set(option.key, option.doubleValue); break;
            default: bag.Set(option.key, option.stringValue); break;
        }
    }
    return bag;
}

TaskDone RunOne(Providers& providers, Connection& connection, CurrentTask& current,
                const RunTask& task) {
    const std::filesystem::path input = PathFrom(task.inputPath);
    const std::filesystem::path output = PathFrom(task.outputPath);

    FileSource source(input);
    const std::optional<MediaProfile> from = providers.executor().Probe(source, HintFor(input));
    if (!from) {
        return Fail(task, Status::kUnsupportedInput,
                    "Morph could not identify " + input.filename().string() + ".");
    }

    PlanRequest request;
    request.source = from->Id();
    request.target = task.targetProfile;
    if (const auto size = source.Size()) {
        request.inputSizeMiB = static_cast<double>(*size) / (1024.0 * 1024.0);
    }

    const PlanResult planned = FindPlan(providers.graph(), request);
    if (!planned) {
        // The broker planned this route against its own copy of the graph before
        // sending the task, so a failure here means the two copies disagree --
        // worth saying plainly rather than reporting as a conversion failure.
        return Fail(task, Status::kUnsupportedInput,
                    "There is no way to convert " + from->Id() + " to " + task.targetProfile +
                        ".");
    }

    std::error_code ec;
    if (output.has_parent_path()) std::filesystem::create_directories(output.parent_path(), ec);

    OptionBag bag = BagFrom(task.options);
    PipeProgress progress(connection, task.jobId, task.taskIndex);
    ScopedTempStore temp("task");
    std::stop_source stop;
    current.Begin(task.jobId, task.taskIndex, stop);

    ConvertContext context{bag, progress, stop.get_token(), temp, providers.capabilities()};
    FileSink sink(output);
    Result result =
        providers.executor().Execute(providers.graph(), *planned.plan, context, source, sink);
    current.End();

    if (!result) {
        // Section 11.3: a cancelled or failed task never leaves a half-written
        // file where the user expects a real one.
        sink.Abandon();
        return Fail(task, result.status(),
                    result.message().empty() ? "The conversion failed." : result.message());
    }

    TaskDone done;
    done.jobId = task.jobId;
    done.taskIndex = task.taskIndex;
    done.status = static_cast<std::uint8_t>(Status::kOk);
    done.inputPath = task.inputPath;
    done.outputPath = task.outputPath;
    done.fidelity = static_cast<std::uint8_t>(result.incurred());
    for (const FidelityNote& note : result.notes()) {
        done.notes.push_back(NoteWire{note.code, note.detail, note.location});
    }
    return done;
}

// One request, answered. Extracted from the loop so that the loop can be a
// try/catch without the switch drifting two levels to the right.
[[nodiscard]] Result ServeOne(Providers& providers, Connection& connection,
                              CurrentTask& current, const Request& request) {
    switch (request.type) {
        case MessageType::kDescribeGraph:
            return connection.Send(DescribeGraphOf(providers));

        case MessageType::kProbeFiles: {
            ProbeFiles probe;
            if (!Decode(request.payload, probe)) return Result::Ok();
            return connection.Send(ProbeAll(providers, probe));
        }
        case MessageType::kRunTask: {
            RunTask task;
            if (!Decode(request.payload, task)) return Result::Ok();
            return connection.Send(RunOne(providers, connection, current, task));
        }
        default:
            // A message the worker does not serve is a broker bug. Say so on
            // stderr, which the broker captures, and keep going: dropping the
            // connection would turn a mistake into a lost job batch.
            std::fprintf(stderr, "morph.worker: unexpected %s from the broker\n",
                         std::string(Name(request.type)).c_str());
            return Result::Ok();
    }
}

}  // namespace

int Serve(Providers& providers, const std::wstring& pipeName) {
    Connection connection;
    if (const Result connected = ConnectToBroker(connection, pipeName); !connected) {
        return 6;
    }

    WorkerHello hello;
    hello.workerVersion = MORPH_VERSION;
    hello.processId = ::GetCurrentProcessId();
    if (const Result sent = connection.Send(hello); !sent) return 6;

    Inbox inbox;
    CurrentTask current;

    // The reader owns Receive for the life of the process. It exists so that a
    // Cancel arriving mid-task is acted on immediately: the main thread is busy
    // converting and cannot be reading, and cancellation that waits for the
    // conversion to finish is not cancellation.
    std::thread reader([&connection, &inbox, &current] {
        while (true) {
            Result failed = Result::Ok();
            std::optional<FrameReader::Frame> frame = connection.Receive(failed);
            if (!frame) break;

            if (frame->type == MessageType::kCancel) {
                ipc::Cancel cancel;
                if (Decode(frame->payload, cancel)) current.Cancel(cancel);
                continue;
            }
            inbox.Push(Request{frame->type, std::move(frame->payload)});
        }
        current.CancelAll();
        inbox.Finish();
    });

    // Joined however this function leaves, not only on the way out of the loop
    // below. std::thread's destructor calls std::terminate on a JOINABLE thread,
    // so before this existed any exception escaping the dispatch loop killed the
    // process instead of being reported -- which is what an abort() dialog from
    // a debug worker turned out to be.
    struct Joiner {
        std::thread& thread;
        ~Joiner() { if (thread.joinable()) thread.join(); }
    } joiner{reader};

    Request request;
    while (inbox.Pop(request)) {
        // A provider that throws fails ITS REQUEST and nothing else. Before this
        // catch existed the exception unwound the dispatch loop, and unwinding
        // past a joinable std::thread calls std::terminate -- so one unopenable
        // file killed the worker in the middle of a batch.
        Result sent = Result::Ok();
        try {
            sent = ServeOne(providers, connection, current, request);
        } catch (const std::exception& error) {
            sent = ReportThrow(connection, request, error.what());
        } catch (...) {
            sent = ReportThrow(connection, request, "an unrecognised failure");
        }
        if (!sent) break;  // the broker went away mid-reply
    }

    // Join before closing: the reader thread is blocked in ReadFile on this very
    // handle, and closing a handle out from under a blocked read is a race we do
    // not need. Whatever ended the loop -- a dead pipe or a finished batch --
    // ends the read too.
    //
    // The Joiner above is the safety net, not the mechanism: it checks joinable,
    // so this join makes it a no-op on the normal path and it only does anything
    // when the function is left some other way.
    reader.join();
    connection.Close();
    return 0;
}

}  // namespace morph::worker
