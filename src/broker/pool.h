// The scheduler (architecture sections 11.1 to 11.3).
//
// One lane per task class, each with its own concurrency limit and its own
// queue. A lane's threads are created when there is work for them and retire
// when there is not, so an idle broker holds no threads and can honour the 60
// second exit in section 10.3.
//
// A lane thread owns a worker process for as long as it is serving tasks of the
// same job -- section 3.1's "per job batch" lifetime. Moving to a different job
// means a new worker: the job object's limits are derived from the job (memory
// budget, interactive weight), and reusing a process across two jobs would mean
// one of them running under the other's constraints.
#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "job.h"
#include "worker_channel.h"

namespace morph::broker {

// Section 11.1's table, resolved against this machine's topology.
struct LaneSizes {
    std::array<unsigned, static_cast<std::size_t>(TaskClass::kCount)> limits{};

    [[nodiscard]] unsigned operator[](TaskClass cls) const {
        return limits[static_cast<std::size_t>(cls)];
    }
};

// Reads the processor topology properly -- physical cores, not
// hardware_concurrency() -- and applies the table.
[[nodiscard]] LaneSizes SizeLanes();

// The number of physical cores, or 0 when the topology cannot be read.
[[nodiscard]] unsigned PhysicalCoreCount();

class Pool {
public:
    explicit Pool(LaneSizes sizes = SizeLanes());
    ~Pool();

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    // Queues every task of `job` that has not already been decided. Tasks the
    // broker resolved on its own -- an unroutable file, a skipped output -- are
    // reported by the caller before this is called and are not queued.
    void Submit(std::shared_ptr<Job> job, const std::vector<std::uint32_t>& taskIndices);

    // Section 11.3. Queued tasks are dropped and reported cancelled; the running
    // one is asked to stop and escalated to TerminateJobObject if it does not.
    void Cancel(std::uint64_t jobId, std::int64_t taskIndex);

    // Nothing queued and nothing running. The idle timer in section 10.3 asks
    // this before it decides the broker has nothing to live for.
    [[nodiscard]] bool Idle() const;

    void Shutdown();

    [[nodiscard]] const LaneSizes& sizes() const noexcept { return sizes_; }

private:
    struct Queued {
        std::shared_ptr<Job> job;
        std::uint32_t taskIndex = 0;
    };

    struct Lane {
        std::deque<Queued> queue;
        unsigned threads = 0;
        unsigned busy = 0;
    };

    // What a lane thread is doing right now, so that Cancel can reach it.
    struct Running {
        std::uint64_t jobId = 0;
        std::uint32_t taskIndex = 0;
        WorkerChannel* worker = nullptr;
        // When the cooperative cancel was sent, for the escalation in Watch().
        std::chrono::steady_clock::time_point cancelledAt{};
        bool cancelRequested = false;
        bool terminated = false;
    };

    void Serve(TaskClass cls);
    void RunTask(WorkerChannel& worker, const std::shared_ptr<Job>& job, std::uint32_t index);
    void Report(const std::shared_ptr<Job>& job, const ipc::TaskDone& done);
    void Watch();

    [[nodiscard]] Lane& LaneFor(TaskClass cls) {
        return lanes_[static_cast<std::size_t>(cls)];
    }

    LaneSizes sizes_;

    mutable std::mutex mutex_;
    std::condition_variable work_;
    std::array<Lane, static_cast<std::size_t>(TaskClass::kCount)> lanes_;
    std::vector<Running> running_;

    // Lane threads are detached: they retire on their own after an idle spell,
    // and a vector of joinable thread objects for threads that come and go would
    // grow for the life of the process. Shutdown waits on this count instead.
    unsigned liveThreads_ = 0;
    std::condition_variable retired_;
    bool shutdown_ = false;

    std::thread watchdog_;
};

}  // namespace morph::broker
