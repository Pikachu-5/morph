// One worker process, its job object, and the pipe between us (sections 11.2,
// 11.3).
//
// Everything that makes a worker containable is here rather than spread across
// the scheduler, because the containment is only as good as its weakest step and
// the steps have to happen in one order:
//
//   create the job object -> create the process SUSPENDED -> assign it to the
//   job -> resume it
//
// Spawning first and assigning afterwards leaves a window in which the process
// is alive and unconstrained, and a process that spawns a child in that window
// has escaped the limit permanently.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "morph/core/ipc/pipe.h"
#include "morph/core/ipc/worker_protocol.h"
#include "morph/core/result.h"

namespace morph::broker {

// Section 11.2's limits, per worker.
struct WorkerLimits {
    // JobMemoryLimit. Zero means "do not cap", which is only used for the
    // utility worker that answers DescribeGraph and never opens a user file.
    std::uint64_t memoryBytes = 0;

    // "rc.Weight = userIsInteractive ? 3 : 9". A background batch that makes the
    // desktop unusable is a bug report even when it finishes faster.
    bool interactive = true;
};

class WorkerChannel {
public:
    WorkerChannel() = default;
    ~WorkerChannel();

    WorkerChannel(const WorkerChannel&) = delete;
    WorkerChannel& operator=(const WorkerChannel&) = delete;

    // Spawns morph.worker.exe --serve on a pipe named with `suffix`, and waits
    // for its WorkerHello. Fails if the worker speaks a different protocol
    // version -- section 10.2's rule, applied to our own processes because a
    // half-upgraded install is exactly where this happens.
    [[nodiscard]] Result Start(std::wstring_view suffix, const WorkerLimits& limits);

    [[nodiscard]] Result Describe(ipc::GraphDescription& into);
    [[nodiscard]] Result Probe(const std::vector<std::string>& paths, ipc::ProbeResult& into);

    // Runs one task to completion, calling `onProgress` for each Progress frame
    // that arrives on the way. Blocks the calling thread for the length of the
    // conversion; cancellation comes from another thread through Cancel().
    [[nodiscard]] Result Run(const ipc::RunTask& task,
                             const std::function<void(const ipc::Progress&)>& onProgress,
                             ipc::TaskDone& into);

    // Section 11.3, level one: cooperative. Safe to call from any thread while
    // Run is in flight -- it is the only thing that is.
    void Cancel(std::uint64_t jobId, std::int64_t taskIndex);

    // Level three: TerminateJobObject. Takes the worker and anything it spawned.
    void Terminate() noexcept;

    void Close() noexcept;

    [[nodiscard]] bool Running() const noexcept;
    [[nodiscard]] std::uint32_t ProcessId() const noexcept { return processId_; }

private:
    // Not CreateJobObject: <windows.h> defines that as a macro for
    // CreateJobObjectW, and a member function of that name is silently renamed
    // by the preprocessor into one that does not exist.
    [[nodiscard]] Result CreateContainer(const WorkerLimits& limits);
    [[nodiscard]] Result Spawn(const std::wstring& pipeName);

    ipc::PipeServer server_;
    ipc::Connection connection_;
    void* job_ = nullptr;      // HANDLE
    void* process_ = nullptr;  // HANDLE
    std::uint32_t processId_ = 0;
};

// The full path to morph.worker.exe, which sits beside the broker. Exposed
// because a broker that cannot find its worker should say so at startup rather
// than on the first conversion.
[[nodiscard]] std::wstring WorkerExecutablePath();

}  // namespace morph::broker
