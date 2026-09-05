#include "app/client.h"

#include "morph/core/ipc/codec.h"

namespace morph::app {

Result BrokerClient::Connect(std::uint32_t timeoutMs) {
    const std::wstring name = ipc::PipeName();

    // A short first attempt on purpose. The common case is a broker already
    // running, which answers immediately; the long timeout belongs after we have
    // started one and are waiting for it to create its pipe, spawn a worker and
    // ask it for the graph.
    if (!ipc::ConnectToBroker(connection_, name, 1000)) {
        (void)ipc::StartBroker(timeoutMs);
        if (const Result connected = ipc::ConnectToBroker(connection_, name, timeoutMs);
            !connected) {
            return connected;
        }
    }
    return Handshake();
}

Result BrokerClient::ConnectTo(const std::wstring& pipeName, std::uint32_t timeoutMs) {
    if (const Result connected = ipc::ConnectToBroker(connection_, pipeName, timeoutMs);
        !connected) {
        return connected;
    }
    return Handshake();
}

Result BrokerClient::Handshake() {
    ipc::Hello hello;
    hello.kind = ipc::ClientKind::kApp;
    hello.clientVersion = MORPH_VERSION;

    {
        const std::lock_guard lock(write_);
        if (const Result sent = connection_.Send(hello); !sent) return sent;
    }
    if (const Result received = connection_.ReceiveAs(greeting_); !received) return received;

    // Section 10.2: "Version mismatch closes the pipe rather than negotiating."
    // Both halves ship in the same package, so a mismatch means a half-finished
    // install or a stale broker from a previous build -- neither of which is
    // improved by carrying on.
    if (greeting_.protocolVersion != ipc::kProtocolVersion) {
        connection_.Close();
        return Result::Error(Status::kInternal,
                             "This build speaks version " +
                                 std::to_string(ipc::kProtocolVersion) +
                                 " of the Morph protocol and the running broker speaks version " +
                                 std::to_string(greeting_.protocolVersion) +
                                 ". Close Morph and try again, or reinstall it.");
    }
    return Result::Ok();
}

Result BrokerClient::IdentifyFiles(const std::vector<std::string>& paths,
                                   ipc::Identified& into) {
    if (!connection_.Open()) {
        return Result::Error(Status::kIoError, "Not connected to the Morph broker.");
    }

    ipc::IdentifyFiles request;
    request.paths = paths;

    {
        const std::lock_guard lock(write_);
        if (const Result sent = connection_.Send(request); !sent) return sent;
    }
    return connection_.ReceiveAs(into);
}

Result BrokerClient::QueryTargets(const std::vector<std::string>& sourceProfiles,
                                  std::uint64_t largestInputBytes, ipc::Targets& into) {
    if (!connection_.Open()) {
        return Result::Error(Status::kIoError, "Not connected to the Morph broker.");
    }

    ipc::QueryTargets query;
    query.sourceProfiles = sourceProfiles;
    query.largestInputBytes = largestInputBytes;

    {
        const std::lock_guard lock(write_);
        if (const Result sent = connection_.Send(query); !sent) return sent;
    }
    return connection_.ReceiveAs(into);
}

Result BrokerClient::Cancel(std::uint64_t jobId, std::int64_t taskIndex) {
    if (!connection_.Open()) {
        return Result::Error(Status::kIoError, "Not connected to the Morph broker.");
    }

    ipc::Cancel cancel;
    cancel.jobId = jobId;
    cancel.taskIndex = taskIndex;

    const std::lock_guard lock(write_);
    return connection_.Send(cancel);
}

Result BrokerClient::RunJob(const ipc::SubmitJob& job, const JobEvents& events) {
    if (!connection_.Open()) {
        return Result::Error(Status::kIoError, "Not connected to the Morph broker.");
    }

    {
        const std::lock_guard lock(write_);
        if (const Result sent = connection_.Send(job); !sent) return sent;
    }

    // Cleared on every exit, including the error ones: a Stop button wired to
    // currentJob() must not cancel a job number that has already been reused.
    struct ClearOnExit {
        std::atomic<std::uint64_t>* slot;
        ~ClearOnExit() { slot->store(0); }
    } clear{&currentJob_};

    bool acceptedSeen = false;

    while (true) {
        Result failed = Result::Ok();
        const std::optional<ipc::FrameReader::Frame> frame = connection_.Receive(failed);
        if (!frame) {
            if (!failed.ok()) return failed;
            return Result::Error(Status::kIoError,
                                 "The Morph broker closed the connection before the job "
                                 "finished.");
        }

        switch (frame->type) {
            case ipc::MessageType::kJobAccepted: {
                ipc::JobAccepted accepted;
                if (!Decode(frame->payload, accepted)) {
                    return Result::Error(Status::kMalformedInput,
                                         "The broker's job acknowledgement could not be read.");
                }
                acceptedSeen = true;
                currentJob_.store(accepted.jobId);
                if (events.accepted) events.accepted(accepted);
                break;
            }

            case ipc::MessageType::kProgress: {
                ipc::Progress progress;
                if (!Decode(frame->payload, progress)) break;  // a lost tick is not a failure
                if (events.progress) events.progress(progress);
                break;
            }

            case ipc::MessageType::kTaskDone: {
                ipc::TaskDone done;
                if (!Decode(frame->payload, done)) {
                    return Result::Error(Status::kMalformedInput,
                                         "The broker's report on a finished file could not be "
                                         "read.");
                }
                if (events.taskDone) events.taskDone(done);
                break;
            }

            case ipc::MessageType::kJobDone: {
                ipc::JobDone done;
                if (!Decode(frame->payload, done)) {
                    return Result::Error(Status::kMalformedInput,
                                         "The broker's report on the finished job could not be "
                                         "read.");
                }
                if (events.jobDone) events.jobDone(done);
                // A JobDone with no JobAccepted before it is a broker that lost
                // track of its own conversation. Saying so beats a Done screen
                // reporting zero of zero files.
                if (!acceptedSeen) {
                    return Result::Error(Status::kInternal,
                                         "The broker reported a finished job it never accepted.");
                }
                return Result::Ok();
            }

            case ipc::MessageType::kError: {
                ipc::Error error;
                if (!Decode(frame->payload, error)) {
                    return Result::Error(Status::kInternal,
                                         "The broker reported a failure that could not be read.");
                }
                if (events.failed) events.failed(error);
                // Section 12.2: "Failures listed with a real reason, never
                // 'conversion failed'." The broker's own words are the real
                // reason; inventing one here would throw it away.
                //
                // A kOk on an Error frame is a broker bug, and taking it at face
                // value would return a Result that says the job succeeded. It
                // becomes kInternal, which is what "our bug" is called.
                const auto status = static_cast<Status>(error.status);
                return Result::Error(status == Status::kOk ? Status::kInternal : status,
                                     error.message.empty()
                                         ? "The Morph broker stopped the job without saying why."
                                         : error.message);
            }

            default:
                return Result::Error(Status::kInternal,
                                     "The broker sent an unexpected " +
                                         std::string(Name(frame->type)) + " message.");
        }
    }
}

}  // namespace morph::app
