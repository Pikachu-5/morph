// The app's side of the broker protocol (architecture sections 10.2 and 12.3).
//
// The broker here is a pipe server in this process, which is the honest way to
// test a client: the real broker has an end-to-end test of its own next door,
// and what these cases are about is what the CLIENT does with what it is told --
// including the things a working broker never says.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <functional>
#include <windows.h>
#include <string>
#include <thread>
#include <vector>

#include "app/client.h"
#include "morph/core/ipc/pipe.h"

using namespace morph;
using namespace morph::app;
using namespace morph::ipc;

namespace {

std::wstring UniqueName() {
    static std::atomic<int> counter{0};
    return PipeNameFor(L"appclient." + std::to_wstring(::GetCurrentProcessId()) + L"." +
                       std::to_wstring(counter.fetch_add(1)));
}

// A broker that runs one scripted conversation and then hangs up. `script` is
// handed the accepted connection and says what this case is about.
class FakeBroker {
public:
    explicit FakeBroker(std::function<void(Connection&)> script) : name_(UniqueName()) {
        REQUIRE(server_.Create(name_).ok());
        thread_ = std::thread([this, script = std::move(script)] {
            Connection connection;
            if (!server_.Accept(connection, 5000).ok()) return;
            script(connection);
        });
    }

    ~FakeBroker() {
        server_.Close();
        if (thread_.joinable()) thread_.join();
    }

    FakeBroker(const FakeBroker&) = delete;
    FakeBroker& operator=(const FakeBroker&) = delete;

    [[nodiscard]] const std::wstring& name() const noexcept { return name_; }

private:
    std::wstring name_;
    PipeServer server_;
    std::thread thread_;
};

// The handshake every script starts with. Returns false when the client said
// something unexpected, which fails the case by leaving the rest unsent.
bool Greet(Connection& connection, std::uint32_t version = kProtocolVersion) {
    Hello hello;
    if (!connection.ReceiveAs(hello).ok()) return false;
    if (hello.kind != ClientKind::kApp) return false;

    HelloAck ack;
    ack.protocolVersion = version;
    ack.brokerVersion = "0.1.0-fake";
    ack.capabilityGeneration = 11;
    return connection.Send(ack).ok();
}

}  // namespace

TEST_CASE("the client introduces itself as the app", "[app][client]") {
    FakeBroker broker([](Connection& connection) { (void)Greet(connection); });

    BrokerClient client;
    REQUIRE(client.ConnectTo(broker.name()).ok());
    CHECK(client.Connected());
    CHECK(client.greeting().brokerVersion == "0.1.0-fake");
    CHECK(client.greeting().capabilityGeneration == 11);
}

TEST_CASE("a broker speaking another protocol version is refused, not negotiated with",
          "[app][client]") {
    FakeBroker broker([](Connection& connection) { (void)Greet(connection, kProtocolVersion + 1); });

    BrokerClient client;
    const Result connected = client.ConnectTo(broker.name());
    // Section 10.2: "Version mismatch closes the pipe rather than negotiating."
    CHECK_FALSE(connected.ok());
    CHECK_FALSE(client.Connected());
    // And it says which two versions, because the person reading this is
    // deciding whether to reinstall.
    CHECK(connected.message().find(std::to_string(kProtocolVersion)) != std::string::npos);
}

TEST_CASE("targets come back for the profiles asked about", "[app][client]") {
    FakeBroker broker([](Connection& connection) {
        if (!Greet(connection)) return;

        QueryTargets query;
        if (!connection.ReceiveAs(query).ok()) return;
        if (query.sourceProfiles.size() != 2 || query.largestInputBytes != 4096) return;

        Targets targets;
        TargetOption option;
        option.profile = "application/pdf";
        option.fidelity = static_cast<std::uint8_t>(Fidelity::Structural);
        option.hopCount = 2;
        targets.targets.push_back(option);
        (void)connection.Send(targets);
    });

    BrokerClient client;
    REQUIRE(client.ConnectTo(broker.name()).ok());

    Targets targets;
    REQUIRE(client.QueryTargets({"text/markdown", "image/png"}, 4096, targets).ok());
    REQUIRE(targets.targets.size() == 1);
    CHECK(targets.targets[0].profile == "application/pdf");
}

TEST_CASE("a job's events arrive in order and the pump ends at JobDone", "[app][client]") {
    FakeBroker broker([](Connection& connection) {
        if (!Greet(connection)) return;

        SubmitJob job;
        if (!connection.ReceiveAs(job).ok()) return;

        JobAccepted accepted;
        accepted.jobId = 42;
        accepted.planProfiles = {"text/markdown", "text/html", "application/pdf"};
        accepted.taskCount = 1;
        if (!connection.Send(accepted).ok()) return;

        if (!connection.Send(Progress{42, 0, 0.5, 1, 0}).ok()) return;

        TaskDone done;
        done.jobId = 42;
        done.taskIndex = 0;
        done.status = static_cast<std::uint8_t>(Status::kOk);
        done.outputPath = R"(C:\out\notes.pdf)";
        if (!connection.Send(done).ok()) return;

        (void)connection.Send(JobDone{42, 1, 0, 0});
    });

    BrokerClient client;
    REQUIRE(client.ConnectTo(broker.name()).ok());

    std::vector<std::string> seen;
    JobEvents events;
    events.accepted = [&](const JobAccepted& a) {
        seen.push_back("accepted:" + std::to_string(a.planProfiles.size()));
    };
    events.progress = [&](const Progress&) { seen.push_back("progress"); };
    events.taskDone = [&](const TaskDone& d) { seen.push_back("task:" + d.outputPath); };
    events.jobDone = [&](const JobDone& d) {
        seen.push_back("done:" + std::to_string(d.succeeded));
    };

    SubmitJob job;
    job.inputPaths = {R"(C:\in\notes.md)"};
    job.targetProfile = "application/pdf";
    REQUIRE(client.RunJob(job, events).ok());

    REQUIRE(seen.size() == 4);
    CHECK(seen[0] == "accepted:3");
    CHECK(seen[1] == "progress");
    CHECK(seen[2] == R"(task:C:\out\notes.pdf)");
    CHECK(seen[3] == "done:1");
    // Cleared on the way out, so a Stop button wired to it cannot cancel a job
    // number that has already been reused.
    CHECK(client.currentJob() == 0);
}

TEST_CASE("a broker error carries the broker's own reason", "[app][client]") {
    FakeBroker broker([](Connection& connection) {
        if (!Greet(connection)) return;

        SubmitJob job;
        if (!connection.ReceiveAs(job).ok()) return;

        JobAccepted accepted;
        accepted.jobId = 7;
        accepted.taskCount = 1;
        if (!connection.Send(accepted).ok()) return;

        Error error;
        error.jobId = 7;
        error.status = static_cast<std::uint8_t>(Status::kMissingCapability);
        error.message = "No encoder on this machine can write HEIF.";
        (void)connection.Send(error);
    });

    BrokerClient client;
    REQUIRE(client.ConnectTo(broker.name()).ok());

    bool sawFailure = false;
    JobEvents events;
    events.failed = [&](const Error&) { sawFailure = true; };

    SubmitJob job;
    job.inputPaths = {R"(C:\in\shot.png)"};
    job.targetProfile = "image/heif";
    const Result result = client.RunJob(job, events);

    CHECK(sawFailure);
    CHECK_FALSE(result.ok());
    CHECK(result.status() == Status::kMissingCapability);
    // Section 12.2: a real reason. Inventing one here would throw the broker's
    // away.
    CHECK(result.message() == "No encoder on this machine can write HEIF.");
}

TEST_CASE("a broker that hangs up mid-job says so rather than waiting", "[app][client]") {
    FakeBroker broker([](Connection& connection) {
        if (!Greet(connection)) return;

        SubmitJob job;
        if (!connection.ReceiveAs(job).ok()) return;

        JobAccepted accepted;
        accepted.jobId = 3;
        accepted.taskCount = 1;
        (void)connection.Send(accepted);
        connection.Close();
    });

    BrokerClient client;
    REQUIRE(client.ConnectTo(broker.name()).ok());

    SubmitJob job;
    job.inputPaths = {R"(C:\in\notes.md)"};
    job.targetProfile = "application/pdf";
    const Result result = client.RunJob(job, JobEvents{});

    CHECK_FALSE(result.ok());
    CHECK(result.message().find("closed the connection") != std::string::npos);
}

TEST_CASE("a JobDone the broker never accepted is a broker bug, and is named as one",
          "[app][client]") {
    FakeBroker broker([](Connection& connection) {
        if (!Greet(connection)) return;

        SubmitJob job;
        if (!connection.ReceiveAs(job).ok()) return;
        // No JobAccepted at all. A done screen reporting zero of zero files is
        // the failure mode this replaces.
        (void)connection.Send(JobDone{9, 0, 0, 0});
    });

    BrokerClient client;
    REQUIRE(client.ConnectTo(broker.name()).ok());

    SubmitJob job;
    job.inputPaths = {R"(C:\in\notes.md)"};
    job.targetProfile = "application/pdf";
    const Result result = client.RunJob(job, JobEvents{});

    CHECK_FALSE(result.ok());
    CHECK(result.status() == Status::kInternal);
}

TEST_CASE("cancel writes while the pump is blocked reading", "[app][client]") {
    // The duplex pipe exists for exactly this: a thread blocked in RunJob's
    // read while the user's click writes a Cancel on another. A synchronous
    // handle would serialise them and deadlock.
    std::atomic<bool> cancelSeen{false};
    FakeBroker broker([&](Connection& connection) {
        if (!Greet(connection)) return;

        SubmitJob job;
        if (!connection.ReceiveAs(job).ok()) return;

        JobAccepted accepted;
        accepted.jobId = 5;
        accepted.taskCount = 1;
        if (!connection.Send(accepted).ok()) return;

        Cancel cancel;
        if (!connection.ReceiveAs(cancel).ok()) return;
        if (cancel.jobId != 5 || cancel.taskIndex != -1) return;
        cancelSeen = true;

        TaskDone done;
        done.jobId = 5;
        done.taskIndex = 0;
        done.status = static_cast<std::uint8_t>(Status::kCancelled);
        if (!connection.Send(done).ok()) return;
        (void)connection.Send(JobDone{5, 0, 0, 1});
    });

    BrokerClient client;
    REQUIRE(client.ConnectTo(broker.name()).ok());

    std::atomic<bool> accepted{false};
    JobEvents events;
    events.accepted = [&](const JobAccepted&) { accepted = true; };

    SubmitJob job;
    job.inputPaths = {R"(C:\in\big.mp4)"};
    job.targetProfile = "video/mkv";

    std::thread stop([&] {
        while (!accepted.load()) std::this_thread::yield();
        (void)client.Cancel(client.currentJob());
    });

    const Result result = client.RunJob(job, events);
    stop.join();

    CHECK(result.ok());
    CHECK(cancelSeen.load());
}
