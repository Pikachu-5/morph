// morph.broker.exe (architecture sections 3.1, 10.3 and 11).
//
// "Owns the graph, the queue, and worker lifetime. Must never parse a user file
//  -- it is the one process that survives, so it must not be the one that
//  crashes."
//
// The graph comes from a worker (see worker_protocol.h for why), the queue is
// Pool, and worker lifetime is WorkerChannel. What is left here is the part that
// is genuinely the broker's own: being a singleton, speaking section 10.2's
// protocol to clients, turning a SubmitJob into tasks, and knowing when there is
// nothing left to do.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "job.h"
#include "morph/core/caps.h"
#include "morph/core/graph.h"
#include "morph/core/provider.h"
#include "pool.h"
#include "worker_channel.h"

namespace morph::broker {

class Broker {
public:
    Broker() = default;
    ~Broker();

    Broker(const Broker&) = delete;
    Broker& operator=(const Broker&) = delete;

    // Claims the singleton, creates the pipe, and asks a worker to describe the
    // graph. Fails with kResourceLimit when another broker already runs, which
    // is not an error the caller should report: it means the broker the client
    // wanted is there.
    [[nodiscard]] Result Start();

    // Accepts clients until the idle timeout expires. Returns when the broker
    // has had no clients and no work for 60 seconds (section 10.3).
    void Run();

    void Stop() noexcept { stopping_.store(true); }

    // What the broker learned from the worker at startup, in one line. Printed
    // by --verbose, which is the only time this process says anything at all.
    [[nodiscard]] std::string Describe() const;

    // Section 10.3's sixty seconds, overridable by --idle-seconds. The tests set
    // it to a few seconds so that a broker they started does not sit in the
    // session for a minute after they are done with it.
    void SetIdleTimeout(std::chrono::seconds seconds) noexcept { idleTimeout_ = seconds; }

private:
    void Serve(ipc::Connection connection);
    void HandleIdentify(ClientLink& client, const ipc::IdentifyFiles& request);
    void HandleQueryTargets(ClientLink& client, const ipc::QueryTargets& request);
    void HandleSubmit(const std::shared_ptr<ClientLink>& client, const ipc::SubmitJob& request);

    // Identifies inputs by asking the utility worker. The broker may not open a
    // user file itself (section 3.1), so this is the only way it learns what a
    // file is -- and it is serialised, because there is one utility worker.
    [[nodiscard]] Result ProbeInputs(const std::vector<std::string>& paths,
                                     std::vector<std::string>& profiles);

    [[nodiscard]] Result AcquireGraph();

    // Section 9.2's memory-mapped table, republished "at startup and whenever the
    // codec probe result changes". Only the first of those happens today: nothing
    // re-probes yet, and the generation is here so that the day something does,
    // readers find out.
    [[nodiscard]] Result PublishCapabilities();

    FormatGraph graph_;
    Capabilities capabilities_;
    CapabilityMapping capabilityMap_;
    // Bumped on every republish. Clients hold it to know whether their copy of
    // the capability table is current (section 9.2). Starts at zero because zero
    // means "nothing published" in the table's own header.
    std::uint64_t capabilityGeneration_ = 0;

    ipc::PipeServer server_;
    void* singleton_ = nullptr;  // HANDLE to the named mutex

    std::mutex utilityMutex_;
    WorkerChannel utility_;

    Pool pool_;

    std::mutex jobsMutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Job>> jobs_;
    std::atomic<std::uint64_t> nextJobId_{1};

    std::chrono::seconds idleTimeout_{60};
    std::atomic<int> clients_{0};
    std::atomic<bool> stopping_{false};
};

}  // namespace morph::broker
