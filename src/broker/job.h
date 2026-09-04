// The job model (architecture section 11).
//
// "A job is a user intent: this set of files, to that target, with those
//  options. A task is one file. Tasks are the scheduling unit."
//
// Everything here is shaped by one fact: a job outlives the client that
// submitted it. A shell extension can submit forty files and exit; the broker
// still has to finish them and still has to have somewhere to send the results
// that does not dangle. That is what ClientLink is.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "morph/core/graph.h"
#include "morph/core/ipc/pipe.h"
#include "morph/core/ipc/protocol.h"
#include "morph/core/planner.h"

namespace morph::broker {

// Section 11.1's table. The classes differ in how much parallelism the work
// already has inside it, which is why one number for all of them is wrong in
// both directions at once.
enum class TaskClass : std::uint8_t {
    kImage = 0,
    kDocument,
    kAudioVideo,
    kPdfRender,
    kCount,
};

[[nodiscard]] std::string_view Name(TaskClass value) noexcept;

// The class a route belongs to, from the profiles along it. A route that renders
// to PDF is a PDF render whatever else it does on the way, because the WebView2
// environment it goes through is a singleton and everything else about the
// route's cost is irrelevant next to that.
[[nodiscard]] TaskClass ClassOf(const FormatGraph& graph, const Plan& plan);

// The connection a job reports to, and the only thing that writes to it once the
// session thread has handed it over.
//
// Two rules make this safe. Sends are serialised on a mutex, because progress
// from four tasks of one job arrives on four threads. And Detach() is called
// only by the session thread, only after its own read loop has ended -- so the
// handle is never closed while anything is reading it.
class ClientLink {
public:
    explicit ClientLink(ipc::Connection connection) : connection_(std::move(connection)) {}

    template <typename T>
    void Send(const T& message) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) return;
        // A send that fails means the client is gone. Mark it closed rather than
        // retrying every subsequent message on a dead pipe.
        if (const Result sent = connection_.Send(message); !sent) open_ = false;
    }

    // Progress, coalesced per job to 10 Hz (section 11.4). Returns false when
    // the message was dropped, which is the normal case under load.
    bool SendProgress(const ipc::Progress& message,
                      std::chrono::steady_clock::time_point now);

    // The read side. Called only by the session thread that owns it -- the send
    // mutex is deliberately not taken here, because a read blocks until the
    // client says something and holding the mutex would stop every job in the
    // batch from reporting while it waits.
    [[nodiscard]] std::optional<ipc::FrameReader::Frame> Receive(Result& failed);

    void Detach() noexcept;
    [[nodiscard]] bool Open() const noexcept;

private:
    std::mutex mutex_;
    ipc::Connection connection_;
    bool open_ = true;
    std::chrono::steady_clock::time_point lastProgress_{};
};

// One file.
struct Task {
    std::uint32_t index = 0;
    std::string input;   // UTF-8, as it arrived on the wire
    std::string output;

    // Set when the broker decided the answer before any worker was involved: an
    // unroutable format, an output name that cannot be produced, a budget the
    // machine cannot meet. Reported as a TaskDone with this text, never dropped.
    std::string error;
    Status status = Status::kOk;
    bool skip = false;  // the overwrite rule said so; not an error
};

class Job {
public:
    std::uint64_t id = 0;
    std::string targetProfile;
    std::vector<ipc::OptionValue> options;
    bool interactive = true;
    TaskClass taskClass = TaskClass::kDocument;

    // Section 11.2's derived budget, the largest any task in this job needs.
    std::uint64_t memoryBudget = 0;

    std::vector<Task> tasks;
    std::shared_ptr<ClientLink> client;

    std::atomic<bool> cancelled{false};

    // Records one finished task and returns true when that was the last one.
    bool Finish(Status status);

    [[nodiscard]] bool Complete() const noexcept { return finished_.load() >= tasks.size(); }

    [[nodiscard]] ipc::JobDone Summary() const;

private:
    std::atomic<std::uint32_t> finished_{0};
    std::atomic<std::uint32_t> succeeded_{0};
    std::atomic<std::uint32_t> failed_{0};
    std::atomic<std::uint32_t> cancelled_{0};
};

// Sends one task's result to the job's client and, when that was the last task,
// the JobDone after it. The broker decides some tasks on its own (an unroutable
// file) and the scheduler decides the rest, and both report the same way.
void ReportTask(const std::shared_ptr<Job>& job, const ipc::TaskDone& done);

}  // namespace morph::broker
