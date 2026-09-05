// The app's side of the broker protocol (architecture sections 10.2 and 12).
//
// Section 12.3 keeps one escape hatch open: if the C++/WinRT XAML tooling
// becomes a schedule risk, Morph.App.exe is rewritten in C# over a projection of
// the broker protocol and everything else stays as designed. That only remains
// possible while the UI reaches the engine through a CONNECTION AND FOUR
// MESSAGES rather than through anything shaped like the engine. So nothing in
// this file knows what a conversion is. It knows what the wire says about one.
//
// THREADING, which is the only subtle thing here. The pipe is duplex by
// construction -- pipe.h gives every connection one event per direction for
// exactly this -- and RunJob uses both directions at once on purpose: it blocks
// reading the broker's progress while Cancel writes from whichever thread the
// user clicked Stop on. Everything else is request/response and belongs to one
// thread at a time.
//
// The shell extension talks to the broker too (command.cpp) and deliberately
// does not use this: it submits forty files and exits without ever reading a
// Progress frame, which is a different shape and a shorter one.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "morph/core/ipc/pipe.h"
#include "morph/core/ipc/protocol.h"
#include "morph/core/result.h"

namespace morph::app {

// Everything the broker says while a job runs, delivered as it arrives.
//
// Called on whichever thread is inside RunJob, which is not the UI thread. The
// model on the other side of these is single-threaded by contract (model.h), so
// the shell marshals each one onto its dispatcher rather than mutating from
// here. Any of them may be empty.
struct JobEvents {
    std::function<void(const ipc::JobAccepted&)> accepted;
    std::function<void(const ipc::Progress&)> progress;
    std::function<void(const ipc::TaskDone&)> taskDone;
    std::function<void(const ipc::JobDone&)> jobDone;
    // A broker-level failure, as opposed to one task failing. The job is over
    // when this arrives.
    std::function<void(const ipc::Error&)> failed;
};

class BrokerClient {
public:
    BrokerClient() = default;
    ~BrokerClient() = default;

    BrokerClient(const BrokerClient&) = delete;
    BrokerClient& operator=(const BrokerClient&) = delete;

    // Connects to the broker, starting one when there is none.
    //
    // Section 10.3 makes that the CLIENT's decision rather than the transport's,
    // because only the caller knows whether it may spawn a process. The app may.
    // A second client racing us to it gets kResourceLimit from StartBroker,
    // which is not a failure -- it means somebody else won and the right move is
    // to connect to the winner, which is what the second attempt does.
    [[nodiscard]] Result Connect(std::uint32_t timeoutMs = 5000);

    // For tests: the same handshake against a pipe that is already listening,
    // with no process ever started. The name is the only difference, and it is
    // the difference between a unit test and an integration test.
    [[nodiscard]] Result ConnectTo(const std::wstring& pipeName, std::uint32_t timeoutMs = 5000);

    [[nodiscard]] bool Connected() const noexcept { return connection_.Open(); }
    void Disconnect() noexcept { connection_.Close(); }

    // What the broker said about itself. Meaningful only after Connect.
    [[nodiscard]] const ipc::HelloAck& greeting() const noexcept { return greeting_; }

    // "list of source profiles -> available target profiles with fidelity
    //  ratings" (section 10.2). The app asks this; the shell extension never
    //  does, because it has a memory-mapped table and 8 ms.
    [[nodiscard]] Result QueryTargets(const std::vector<std::string>& sourceProfiles,
                                      std::uint64_t largestInputBytes, ipc::Targets& into);

    // Submits one job and pumps its events until the broker says it is over.
    // Blocks; the caller owns the thread.
    //
    // Returns ok when a JobDone arrived, whatever the individual tasks did --
    // three failed conversions are a job that finished, and the per-task truth
    // is in the TaskDone events. An error here is the connection or the broker,
    // not the files.
    [[nodiscard]] Result RunJob(const ipc::SubmitJob& job, const JobEvents& events);

    // Cancels a whole job, or one task of it. Safe to call from another thread
    // while RunJob is blocked; that is the entire reason the write lock exists.
    [[nodiscard]] Result Cancel(std::uint64_t jobId, std::int64_t taskIndex = -1);

    // The job RunJob is currently pumping, or 0. Lets the UI wire a Stop button
    // before the JobAccepted has arrived without inventing an id.
    [[nodiscard]] std::uint64_t currentJob() const noexcept { return currentJob_.load(); }

private:
    [[nodiscard]] Result Handshake();

    ipc::Connection connection_;
    ipc::HelloAck greeting_;

    // Held across every Send. Two threads writing frames into one pipe
    // interleave them, and a half-written frame is not a recoverable state.
    std::mutex write_;

    std::atomic<std::uint64_t> currentJob_{0};
};

}  // namespace morph::app
