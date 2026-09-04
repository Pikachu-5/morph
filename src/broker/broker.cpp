#include "broker.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <thread>

#include <windows.h>

#include "capabilities.h"
#include "morph/core/ipc/worker_protocol.h"
#include "morph/core/planner.h"
#include "output.h"

namespace morph::broker {
namespace {

// How long Accept waits before the loop re-checks the idle timer. Short enough
// that the broker exits promptly, long enough that an idle broker is not a
// process spinning in the background.
constexpr int kAcceptTimeoutMs = 250;

// Section 9.3 caps a selection long before it reaches the broker. This is the
// backstop for a client that does not.
constexpr std::size_t kMaxInputsPerJob = 10000;

// Slack over the derived memory budget, for the runtime, the provider's own
// fixed costs, and the fact that memPeakFactor is an estimate.
constexpr std::uint64_t kMemorySlackBytes = 64ull * 1024 * 1024;

std::uint64_t AvailablePhysicalBytes() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!::GlobalMemoryStatusEx(&status)) return 0;
    return status.ullAvailPhys;
}

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

std::string Utf8From(const std::filesystem::path& path) {
    const std::wstring wide = path.wstring();
    if (wide.empty()) return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                             static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                             nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(),
                          needed, nullptr, nullptr);
    return out;
}

// The size of an input, for the estimate and the memory budget. Metadata, not
// content: section 3.1 forbids the broker READING a user file, and the whole
// point of the probe round trip is that it never does.
std::uint64_t SizeOf(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

std::string Explain(PlanError error, const std::string& source, const std::string& target) {
    switch (error) {
        case PlanError::kUnknownSource:
            return "Morph does not handle " + source + " files.";
        case PlanError::kUnknownTarget:
            return "Morph does not produce " + target + " files.";
        case PlanError::kNoRoute:
            break;
    }
    return "There is no way to convert " + source + " to " + target + ".";
}

void SendError(ClientLink& client, std::uint64_t jobId, Status status, std::string message) {
    ipc::Error error;
    error.jobId = jobId;
    error.status = static_cast<std::uint8_t>(status);
    error.message = std::move(message);
    client.Send(error);
}

}  // namespace

Broker::~Broker() {
    pool_.Shutdown();
    if (singleton_ != nullptr) ::CloseHandle(static_cast<HANDLE>(singleton_));
}

Result Broker::Start() {
    // Section 10.3: "Singleton enforced by a named mutex plus
    // FILE_FLAG_FIRST_PIPE_INSTANCE." Both, because they fail differently: the
    // mutex catches a second broker before it does any work, and the pipe flag
    // catches a squatter that would not have taken the mutex at all.
    DWORD session = 0;
    ::ProcessIdToSessionId(::GetCurrentProcessId(), &session);
    const std::wstring mutexName = L"Local\\morph.broker." + std::to_wstring(session);

    singleton_ = ::CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (singleton_ == nullptr) {
        return Result::Error(Status::kInternal, "Could not create the broker's singleton mutex.");
    }
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        return Result::Error(Status::kResourceLimit, "A broker is already running.");
    }

    if (const Result created = server_.Create(ipc::PipeName()); !created) return created;

    // The utility worker is the broker's only source of truth about formats. It
    // is started once and kept: a right-click that has to wait for a process
    // spawn before it can show a submenu is exactly what section 9.2 forbids.
    WorkerLimits limits;
    limits.interactive = true;
    if (const Result started = utility_.Start(L"utility", limits); !started) return started;

    if (const Result graph = AcquireGraph(); !graph) return graph;

    // A broker that cannot publish its table still works -- clients ask
    // QueryTargets and get the same answers over the pipe. Only the Explorer
    // menu degrades, and section 9.2 already says what it degrades to. So this
    // is reported and not fatal.
    if (const Result published = PublishCapabilities(); !published) {
        std::fprintf(stderr, "morph.broker: %s\n", published.message().c_str());
    }
    return Result::Ok();
}

Result Broker::PublishCapabilities() {
    if (!capabilityMap_.Mapped()) {
        if (const Result created = capabilityMap_.Create(); !created) return created;
    }
    return capabilityMap_.Publish(CapabilitiesOf(graph_), ++capabilityGeneration_);
}

Result Broker::AcquireGraph() {
    ipc::GraphDescription description;
    {
        const std::lock_guard<std::mutex> lock(utilityMutex_);
        if (const Result described = utility_.Describe(description); !described) return described;
    }

    for (const std::string& capability : description.capabilities) {
        capabilities_.Add(capability);
    }

    for (const ipc::EdgeWire& wire : description.edges) {
        const std::optional<MediaProfile> from = MediaProfile::Parse(wire.from);
        const std::optional<MediaProfile> to = MediaProfile::Parse(wire.to);
        if (!from || !to) {
            return Result::Error(Status::kInternal,
                                 "The worker described an edge between profiles that do not "
                                 "parse: " + wire.from + " -> " + wire.to + ".");
        }

        EdgeCost cost;
        cost.fidelity = static_cast<Fidelity>(wire.fidelity);
        cost.generational = wire.generational;
        cost.cpuPerMiB = static_cast<float>(wire.cpuPerMiB);
        cost.memPeakFactor = static_cast<float>(wire.memPeakFactor);
        cost.requiresDisk = wire.requiresDisk;
        (void)graph_.AddEdge(*from, *to, cost, wire.provider);
    }

    graph_.Freeze();
    if (graph_.EdgeCount() == 0) {
        return Result::Error(Status::kInternal, "The worker described an empty graph.");
    }
    return Result::Ok();
}

Result Broker::ProbeInputs(const std::vector<std::string>& paths,
                           std::vector<std::string>& profiles) {
    ipc::ProbeResult result;
    {
        const std::lock_guard<std::mutex> lock(utilityMutex_);
        if (!utility_.Running()) {
            // The utility worker died -- killed by its own limits, or crashed on
            // a file. Section 3.1 treats that as expected; restart it and carry
            // on rather than taking the broker down with it.
            utility_.Close();
            WorkerLimits limits;
            limits.interactive = true;
            if (const Result restarted = utility_.Start(L"utility", limits); !restarted) {
                return restarted;
            }
        }
        if (const Result probed = utility_.Probe(paths, result); !probed) return probed;
    }
    profiles = std::move(result.profiles);
    return Result::Ok();
}

void Broker::Run() {
    auto idleSince = std::chrono::steady_clock::now();

    while (!stopping_.load()) {
        ipc::Connection connection;
        const Result accepted = server_.Accept(connection, kAcceptTimeoutMs);

        if (accepted) {
            clients_.fetch_add(1);
            std::thread(&Broker::Serve, this, std::move(connection)).detach();
            continue;
        }
        if (!accepted.cancelled()) break;  // the pipe is broken, not merely idle

        if (clients_.load() > 0 || !pool_.Idle()) {
            idleSince = std::chrono::steady_clock::now();
            continue;
        }
        if (std::chrono::steady_clock::now() - idleSince >= idleTimeout_) break;
    }

    pool_.Shutdown();

    // Session threads are detached and hold a raw `this`. On the idle path there
    // are none by definition; on the Stop() path -- Ctrl+C, or the shutdown
    // notification a packaged install gets -- there can be, and they have to be
    // out of the object before it goes away. Their connections are already dead
    // or dying, so this is a short wait rather than a real one.
    for (int waited = 0; clients_.load() > 0 && waited < 200; ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

std::string Broker::Describe() const {
    std::string text = std::to_string(graph_.NodeCount()) + " formats, " +
                       std::to_string(graph_.EdgeCount()) + " conversions";

    const CapabilityTable table = capabilityMap_.Table();
    text += table.Valid() ? ", menu table generation " + std::to_string(table.Generation()) +
                                " with " + std::to_string(table.Extensions().size()) +
                                " extensions"
                          : ", no menu table";

    if (capabilities_.All().empty()) return text + ", no optional capabilities";

    text += ", capabilities:";
    for (const std::string& capability : capabilities_.All()) text += " " + capability;
    return text;
}

void Broker::Serve(ipc::Connection connection) {
    auto client = std::make_shared<ClientLink>(std::move(connection));

    // Hello first, always. A client that starts with anything else is either a
    // different protocol or a probe, and neither gets further than this.
    {
        Result failed = Result::Ok();
        const std::optional<ipc::FrameReader::Frame> frame = client->Receive(failed);
        if (!frame || frame->type != ipc::MessageType::kHello) {
            client->Detach();
            clients_.fetch_sub(1);
            return;
        }

        ipc::Hello hello;
        if (!Decode(frame->payload, hello) || hello.protocolVersion != ipc::kProtocolVersion) {
            // Section 10.2: "Version mismatch closes the pipe rather than
            // negotiating." The Error is a courtesy so the client can say
            // something useful before it goes.
            SendError(*client, 0, Status::kInternal,
                      "This broker speaks protocol version " +
                          std::to_string(ipc::kProtocolVersion) + ".");
            client->Detach();
            clients_.fetch_sub(1);
            return;
        }

        ipc::HelloAck ack;
        ack.brokerVersion = MORPH_VERSION;
        ack.capabilityGeneration = capabilityGeneration_;
        client->Send(ack);
    }

    while (true) {
        Result failed = Result::Ok();
        const std::optional<ipc::FrameReader::Frame> frame = client->Receive(failed);
        if (!frame) break;

        switch (frame->type) {
            case ipc::MessageType::kQueryTargets: {
                ipc::QueryTargets request;
                if (Decode(frame->payload, request)) HandleQueryTargets(*client, request);
                break;
            }
            case ipc::MessageType::kSubmitJob: {
                ipc::SubmitJob request;
                if (Decode(frame->payload, request)) HandleSubmit(client, request);
                break;
            }
            case ipc::MessageType::kCancel: {
                ipc::Cancel request;
                if (!Decode(frame->payload, request)) break;
                {
                    const std::lock_guard<std::mutex> lock(jobsMutex_);
                    const auto found = jobs_.find(request.jobId);
                    if (found != jobs_.end() && request.taskIndex < 0) {
                        found->second->cancelled.store(true);
                    }
                }
                pool_.Cancel(request.jobId, request.taskIndex);
                break;
            }
            default:
                SendError(*client, 0, Status::kInternal,
                          "A client may not send " + std::string(Name(frame->type)) + ".");
                break;
        }
    }

    // The jobs this client submitted keep running. The user asked for files and
    // the files are being written; a closed window is not a cancellation, and
    // the shell extension closes its connection as a matter of course.
    client->Detach();
    clients_.fetch_sub(1);
}

void Broker::HandleQueryTargets(ClientLink& client, const ipc::QueryTargets& request) {
    ipc::Targets reply;
    if (request.sourceProfiles.empty()) {
        client.Send(reply);
        return;
    }

    const double sizeMiB =
        request.largestInputBytes == 0
            ? 1.0
            : static_cast<double>(request.largestInputBytes) / (1024.0 * 1024.0);

    // A mixed selection can only offer what EVERY file in it can become. The
    // alternative -- the union -- puts entries in the menu that fail for half
    // the selection, which is section 12.2's "never offer what cannot be done".
    bool first = true;
    for (const std::string& source : request.sourceProfiles) {
        PlanRequest planRequest;
        planRequest.source = source;
        planRequest.inputSizeMiB = sizeMiB;

        std::vector<ipc::TargetOption> reachable;
        for (const auto& [node, plan] : FindAllTargets(graph_, planRequest)) {
            ipc::TargetOption option;
            option.profile = graph_.Node(node).Id();
            option.fidelity = static_cast<std::uint8_t>(plan.fidelity());
            option.hopCount = plan.cost.hopCount;
            option.estimatedMs = plan.cost.estimatedMs;
            option.requiresDisk = plan.requiresDisk;
            reachable.push_back(std::move(option));
        }

        if (first) {
            reply.targets = std::move(reachable);
            first = false;
            continue;
        }

        // Intersect, keeping the worst of each pair: the rating shown has to be
        // the one the user will actually get on the worst file in the selection.
        std::vector<ipc::TargetOption> merged;
        for (ipc::TargetOption& kept : reply.targets) {
            const auto match = std::find_if(reachable.begin(), reachable.end(),
                                            [&kept](const ipc::TargetOption& candidate) {
                                                return candidate.profile == kept.profile;
                                            });
            if (match == reachable.end()) continue;
            kept.fidelity = std::max(kept.fidelity, match->fidelity);
            kept.hopCount = std::max(kept.hopCount, match->hopCount);
            kept.estimatedMs += match->estimatedMs;
            kept.requiresDisk = kept.requiresDisk || match->requiresDisk;
            merged.push_back(std::move(kept));
        }
        reply.targets = std::move(merged);
        if (reply.targets.empty()) break;
    }

    client.Send(reply);
}

void Broker::HandleSubmit(const std::shared_ptr<ClientLink>& client,
                          const ipc::SubmitJob& request) {
    if (request.inputPaths.empty()) {
        SendError(*client, 0, Status::kUnsupportedInput, "That job has no files in it.");
        return;
    }
    if (request.inputPaths.size() > kMaxInputsPerJob) {
        SendError(*client, 0, Status::kResourceLimit,
                  "That is " + std::to_string(request.inputPaths.size()) +
                      " files; Morph converts at most " + std::to_string(kMaxInputsPerJob) +
                      " in one job.");
        return;
    }
    if (!graph_.Find(request.targetProfile)) {
        SendError(*client, 0, Status::kUnsupportedInput,
                  "Morph does not produce " + request.targetProfile + " files.");
        return;
    }

    std::vector<std::string> profiles;
    if (const Result probed = ProbeInputs(request.inputPaths, profiles); !probed) {
        SendError(*client, 0, probed.status(), probed.message());
        return;
    }

    auto job = std::make_shared<Job>();
    job->id = nextJobId_.fetch_add(1);
    job->targetProfile = request.targetProfile;
    job->options = request.options;
    job->interactive = request.interactive;
    job->client = client;

    const std::uint64_t memoryCeiling = AvailablePhysicalBytes() / 4;

    std::vector<std::filesystem::path> taken;
    std::vector<std::uint32_t> runnable;
    std::vector<std::uint32_t> decided;
    std::vector<std::string> planProfiles;
    double estimatedMs = 0.0;
    std::uint8_t worstFidelity = 0;
    unsigned laneLimit = 0;

    for (std::size_t i = 0; i < request.inputPaths.size(); ++i) {
        Task task;
        task.index = static_cast<std::uint32_t>(i);
        task.input = request.inputPaths[i];

        const std::filesystem::path input = PathFrom(task.input);

        if (profiles[i].empty()) {
            task.status = Status::kUnsupportedInput;
            task.error = "Morph could not identify " + input.filename().string() +
                         ". It reads formats by content, not by extension, so a renamed file "
                         "is not the problem.";
            job->tasks.push_back(std::move(task));
            decided.push_back(static_cast<std::uint32_t>(i));
            continue;
        }

        const std::uint64_t inputBytes = SizeOf(input);

        PlanRequest planRequest;
        planRequest.source = profiles[i];
        planRequest.target = request.targetProfile;
        planRequest.inputSizeMiB =
            inputBytes == 0 ? 1.0 : static_cast<double>(inputBytes) / (1024.0 * 1024.0);

        const PlanResult planned = FindPlan(graph_, planRequest);
        if (!planned) {
            task.status = Status::kUnsupportedInput;
            task.error = Explain(planned.error, profiles[i], request.targetProfile);
            job->tasks.push_back(std::move(task));
            decided.push_back(static_cast<std::uint32_t>(i));
            continue;
        }

        // Section 11.2: "A task that would exceed it is rejected at plan time
        // with a clear message rather than being allowed to start and then
        // killed halfway."
        const std::uint64_t needed =
            static_cast<std::uint64_t>(planned.plan->memPeakFactor *
                                       static_cast<double>(inputBytes)) +
            kMemorySlackBytes;
        if (memoryCeiling > 0 && needed > memoryCeiling) {
            task.status = Status::kResourceLimit;
            task.error = input.filename().string() + " would need about " +
                         std::to_string(needed / (1024 * 1024)) +
                         " MB to convert, and there is not that much memory free.";
            job->tasks.push_back(std::move(task));
            decided.push_back(static_cast<std::uint32_t>(i));
            continue;
        }

        const OutputPlan output =
            ResolveOutput(input, request.targetProfile, request.output, i, taken);
        if (!output.error.empty()) {
            task.status = Status::kIoError;
            task.error = output.error;
            job->tasks.push_back(std::move(task));
            decided.push_back(static_cast<std::uint32_t>(i));
            continue;
        }
        task.output = Utf8From(output.path);

        if (output.skip) {
            // Not an error and not silence. Section 12.2 wants every file
            // accounted for, and "it was already there" is an answer.
            task.skip = true;
            task.status = Status::kCancelled;
            task.error = output.path.filename().string() + " is already there, so it was left "
                         "alone.";
            job->tasks.push_back(std::move(task));
            decided.push_back(static_cast<std::uint32_t>(i));
            continue;
        }

        job->memoryBudget = std::max(job->memoryBudget, needed);
        estimatedMs += planned.plan->cost.estimatedMs;
        worstFidelity =
            std::max(worstFidelity, static_cast<std::uint8_t>(planned.plan->fidelity()));

        // The job takes the most constrained class any of its tasks needs. A
        // batch of images that includes one PDF render must not run four wide
        // through a WebView2 environment there is only one of.
        const TaskClass cls = ClassOf(graph_, *planned.plan);
        const unsigned limit = pool_.sizes()[cls];
        if (laneLimit == 0 || limit < laneLimit) {
            laneLimit = limit;
            job->taskClass = cls;
        }

        if (planProfiles.empty()) {
            planProfiles.push_back(profiles[i]);
            for (const PlanHop& hop : planned.plan->hops) {
                planProfiles.push_back(graph_.Node(hop.to).Id());
            }
        }

        job->tasks.push_back(std::move(task));
        runnable.push_back(static_cast<std::uint32_t>(i));
    }

    {
        const std::lock_guard<std::mutex> lock(jobsMutex_);
        // Finished jobs are forgotten here rather than on completion: a job is
        // only reachable by id for cancellation, and sweeping on submit keeps
        // that bookkeeping off the scheduler's threads.
        std::erase_if(jobs_, [](const auto& entry) { return entry.second->Complete(); });
        jobs_.emplace(job->id, job);
    }

    ipc::JobAccepted accepted;
    accepted.jobId = job->id;
    accepted.planProfiles = planProfiles;
    accepted.fidelity = worstFidelity;
    accepted.estimatedMs = estimatedMs;
    accepted.taskCount = static_cast<std::uint32_t>(job->tasks.size());
    client->Send(accepted);

    // The decided tasks are reported before the runnable ones are queued, so a
    // job whose every file was rejected still sends its JobDone -- and sends it
    // after the JobAccepted rather than racing it.
    for (const std::uint32_t index : decided) {
        const Task& task = job->tasks[index];
        ipc::TaskDone done;
        done.jobId = job->id;
        done.taskIndex = task.index;
        done.status = static_cast<std::uint8_t>(task.status);
        done.inputPath = task.input;
        done.outputPath = task.skip ? task.output : std::string();
        done.error = task.error;
        ReportTask(job, done);
    }

    pool_.Submit(job, runnable);
}

}  // namespace morph::broker
