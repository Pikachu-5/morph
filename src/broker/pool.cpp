#include "pool.h"

#include <algorithm>

#include <windows.h>

namespace morph::broker {
namespace {

// How long a lane thread waits for more work before retiring. Shorter than the
// broker's own 60 second idle exit, so that by the time the broker considers
// quitting its threads have already gone.
constexpr auto kThreadIdleTimeout = std::chrono::seconds(15);

// Section 11.3: "If a provider does not respond within 2 seconds..."
constexpr auto kCancelGrace = std::chrono::seconds(2);
constexpr auto kWatchInterval = std::chrono::milliseconds(250);

ipc::TaskDone Decided(const Job& job, const Task& task, Status status, std::string error) {
    ipc::TaskDone done;
    done.jobId = job.id;
    done.taskIndex = task.index;
    done.status = static_cast<std::uint8_t>(status);
    done.inputPath = task.input;
    done.error = std::move(error);
    return done;
}

}  // namespace

unsigned PhysicalCoreCount() {
    DWORD bytes = 0;
    ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    if (bytes == 0) return 0;

    std::vector<std::byte> buffer(bytes);
    auto* first = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    if (!::GetLogicalProcessorInformationEx(RelationProcessorCore, first, &bytes)) return 0;

    // The records are variable length, so this is a byte walk rather than an
    // array index. Section 11.1: read the topology properly rather than assuming
    // SMT, because "cores / 2" means physical cores, not logical ones halved.
    unsigned cores = 0;
    std::size_t offset = 0;
    while (offset < bytes) {
        const auto* record =
            reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() +
                                                                             offset);
        if (record->Size == 0) break;
        if (record->Relationship == RelationProcessorCore) ++cores;
        offset += record->Size;
    }
    return cores;
}

LaneSizes SizeLanes() {
    const unsigned logical = std::max(1u, std::thread::hardware_concurrency());
    const unsigned reported = PhysicalCoreCount();
    // Halving the logical count is the fallback, not the rule: it is right on an
    // SMT part and wrong on one without it, which is why the topology is read
    // properly first.
    const unsigned physical = std::max(1u, reported != 0 ? reported : logical / 2);

    LaneSizes sizes;
    // "Image: logical cores. WIC codecs are largely single-threaded per frame."
    sizes.limits[static_cast<std::size_t>(TaskClass::kImage)] = logical;
    // "Document: cores / 2. Memory-heavy -- bound by RAM, not CPU."
    sizes.limits[static_cast<std::size_t>(TaskClass::kDocument)] = std::max(1u, physical / 2);
    // "Video / audio: 1-2. FFmpeg and MF already saturate every core internally."
    sizes.limits[static_cast<std::size_t>(TaskClass::kAudioVideo)] = physical >= 4 ? 2u : 1u;
    // "PDF render (WebView2): 1. One WebView2 environment, reused."
    sizes.limits[static_cast<std::size_t>(TaskClass::kPdfRender)] = 1u;
    return sizes;
}

Pool::Pool(LaneSizes sizes) : sizes_(sizes) {
    watchdog_ = std::thread(&Pool::Watch, this);
}

Pool::~Pool() { Shutdown(); }

void Pool::Submit(std::shared_ptr<Job> job, const std::vector<std::uint32_t>& taskIndices) {
    if (taskIndices.empty()) return;

    unsigned toStart = 0;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return;

        Lane& lane = LaneFor(job->taskClass);
        for (const std::uint32_t index : taskIndices) {
            lane.queue.push_back(Queued{job, index});
        }

        // One thread per queued task, up to the lane's limit. Threads that are
        // already serving this lane count toward it.
        const unsigned limit = std::max(1u, sizes_[job->taskClass]);
        const unsigned wanted =
            std::min<unsigned>(limit, static_cast<unsigned>(lane.queue.size()));
        if (wanted > lane.threads) {
            toStart = wanted - lane.threads;
            lane.threads += toStart;
            liveThreads_ += toStart;
        }
    }

    const TaskClass cls = job->taskClass;
    for (unsigned i = 0; i < toStart; ++i) {
        std::thread(&Pool::Serve, this, cls).detach();
    }
    work_.notify_all();
}

void Pool::Serve(TaskClass cls) {
    WorkerChannel worker;
    std::uint64_t workerJobId = 0;

    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        Lane& lane = LaneFor(cls);
        if (shutdown_) break;

        if (lane.queue.empty()) {
            // Retire on a timeout rather than immediately: a batch arriving in
            // several SubmitJob messages should not pay for a new process per
            // message.
            if (!work_.wait_for(lock, kThreadIdleTimeout,
                                [this, cls] {
                                    return shutdown_ || !LaneFor(cls).queue.empty();
                                })) {
                break;
            }
            continue;
        }

        const Queued item = lane.queue.front();
        lane.queue.pop_front();
        ++lane.busy;
        lock.unlock();

        if (item.job->cancelled.load()) {
            Report(item.job, Decided(*item.job, item.job->tasks[item.taskIndex],
                                     Status::kCancelled, {}));
        } else {
            // A worker serves one job's tasks and no other's: its job object
            // carries that job's memory budget and CPU weight.
            if (workerJobId != item.job->id || !worker.Running()) {
                worker.Close();
                WorkerLimits limits;
                limits.memoryBytes = item.job->memoryBudget;
                limits.interactive = item.job->interactive;

                if (const Result started = worker.Start(L"worker", limits); !started) {
                    Report(item.job, Decided(*item.job, item.job->tasks[item.taskIndex],
                                             started.status(), started.message()));
                    lock.lock();
                    --lane.busy;
                    continue;
                }
                workerJobId = item.job->id;
            }
            RunTask(worker, item.job, item.taskIndex);
        }

        lock.lock();
        --lane.busy;
    }

    LaneFor(cls).threads -= 1;
    liveThreads_ -= 1;
    lock.unlock();

    // Outside the lock: closing a worker waits up to two seconds for it to exit,
    // and holding the scheduler's mutex for that would stall every other lane.
    worker.Close();
    retired_.notify_all();
}

void Pool::RunTask(WorkerChannel& worker, const std::shared_ptr<Job>& job,
                   std::uint32_t index) {
    const Task& task = job->tasks[index];

    ipc::RunTask request;
    request.jobId = job->id;
    request.taskIndex = task.index;
    request.inputPath = task.input;
    request.outputPath = task.output;
    request.targetProfile = job->targetProfile;
    request.options = job->options;

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        Running running;
        running.jobId = job->id;
        running.taskIndex = task.index;
        running.worker = &worker;
        running_.push_back(running);
    }

    ipc::TaskDone done;
    const Result ran = worker.Run(request, [this, job](const ipc::Progress& progress) {
        if (job->client) job->client->SendProgress(progress, std::chrono::steady_clock::now());
    }, done);

    bool wasCancelled = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = std::find_if(running_.begin(), running_.end(),
                                        [&](const Running& r) {
                                            return r.jobId == job->id &&
                                                   r.taskIndex == task.index;
                                        });
        if (entry != running_.end()) {
            wasCancelled = entry->cancelRequested;
            running_.erase(entry);
        }
    }

    if (!ran) {
        // A worker that was terminated by the watchdog reports as a dead pipe.
        // The user asked for that, so it is a cancellation, not a failure.
        done = Decided(*job, task, wasCancelled ? Status::kCancelled : ran.status(),
                       wasCancelled ? std::string() : ran.message());
    }
    Report(job, done);
}

void Pool::Report(const std::shared_ptr<Job>& job, const ipc::TaskDone& done) {
    ReportTask(job, done);
}

void Pool::Cancel(std::uint64_t jobId, std::int64_t taskIndex) {
    std::vector<Queued> dropped;
    std::shared_ptr<Job> job;

    {
        const std::lock_guard<std::mutex> lock(mutex_);

        for (Lane& lane : lanes_) {
            for (auto it = lane.queue.begin(); it != lane.queue.end();) {
                const bool matches = it->job->id == jobId &&
                                     (taskIndex < 0 ||
                                      it->taskIndex == static_cast<std::uint32_t>(taskIndex));
                if (!matches) {
                    ++it;
                    continue;
                }
                if (taskIndex < 0) it->job->cancelled.store(true);
                job = it->job;
                dropped.push_back(*it);
                it = lane.queue.erase(it);
            }
        }

        const auto now = std::chrono::steady_clock::now();
        for (Running& running : running_) {
            if (running.jobId != jobId) continue;
            if (taskIndex >= 0 && running.taskIndex != static_cast<std::uint32_t>(taskIndex)) {
                continue;
            }
            if (!running.cancelRequested) {
                running.cancelRequested = true;
                running.cancelledAt = now;
                // Level one: cooperative. Every provider checks its stop token
                // at 100 ms granularity, so this is what ends 99% of cancels.
                running.worker->Cancel(jobId, taskIndex);
            }
        }
    }

    // Reported outside the lock: a client whose pipe has filled up must not
    // block the scheduler.
    for (const Queued& item : dropped) {
        Report(item.job, Decided(*item.job, item.job->tasks[item.taskIndex],
                                 Status::kCancelled, {}));
    }
}

void Pool::Watch() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!shutdown_) {
        work_.wait_for(lock, kWatchInterval);
        if (shutdown_) break;

        const auto now = std::chrono::steady_clock::now();
        for (Running& running : running_) {
            if (!running.cancelRequested || running.terminated) continue;
            if (now - running.cancelledAt < kCancelGrace) continue;

            // Section 11.3 escalates cooperative -> close the worker's pipe ->
            // TerminateJobObject. The middle step exists to unblock a worker
            // sitting in a read; ours cannot be, because it reads on a thread
            // that is never the one converting. So the escalation goes straight
            // to the job object, which takes the worker and anything it spawned.
            running.terminated = true;
            running.worker->Terminate();
        }
    }
}

bool Pool::Idle() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.empty()) return false;
    return std::all_of(lanes_.begin(), lanes_.end(),
                       [](const Lane& lane) { return lane.queue.empty(); });
}

void Pool::Shutdown() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return;
        shutdown_ = true;
        for (Lane& lane : lanes_) lane.queue.clear();
        // Whatever is still running is going away with the process. Ask nicely
        // first so that a worker mid-write abandons its temp file rather than
        // being killed with it half committed.
        for (Running& running : running_) {
            running.worker->Cancel(running.jobId, running.taskIndex);
        }
    }
    work_.notify_all();

    {
        std::unique_lock<std::mutex> lock(mutex_);
        // Bounded: a lane thread blocked on a worker that ignores its cancel
        // must not keep the broker alive indefinitely, and the job objects clean
        // up the workers when the process exits regardless.
        retired_.wait_for(lock, std::chrono::seconds(10),
                          [this] { return liveThreads_ == 0; });
    }

    if (watchdog_.joinable()) watchdog_.join();
}

}  // namespace morph::broker
