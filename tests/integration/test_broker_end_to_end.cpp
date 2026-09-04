// The broker, a worker, and a real conversion (architecture sections 3, 10, 11).
//
// This test starts morph.broker.exe, which starts morph.worker.exe, and drives
// the whole thing over the real named pipe with the real protocol. It exists
// because of what the last milestone taught: 297 unit tests passed while the
// assembled path converted nothing at all, and every one of those bugs lived in
// the seam between two components that were each correct.
//
// It is skipped, not failed, when the binaries are not beside the test. A
// developer running the unit suite from a partial build should not see red for
// something they did not break.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "morph/core/caps.h"
#include "morph/core/ipc/pipe.h"
#include "morph/core/ipc/protocol.h"

using namespace morph;

namespace {

std::filesystem::path BinaryDirectory() {
    wchar_t self[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    return std::filesystem::path(self).parent_path();
}

// Starts a broker and keeps its handle, so the test can take it down rather
// than leaving it to the idle timeout.
class BrokerProcess {
public:
    BrokerProcess() {
        const std::filesystem::path exe = BinaryDirectory() / "morph.broker.exe";
        if (!std::filesystem::exists(exe)) return;

        // A short idle timeout, so that a broker this test started does not sit
        // in the session for a minute afterwards if the teardown is skipped.
        std::wstring command = L"\"" + exe.wstring() + L"\" --idle-seconds 5";

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                              nullptr, nullptr, &startup, &info)) {
            return;
        }
        ::CloseHandle(info.hThread);
        process_ = info.hProcess;
        started_ = true;
    }

    ~BrokerProcess() {
        if (process_ != nullptr) {
            // The broker may already have exited -- it does that when another
            // one is running, which is the correct behaviour and not a failure.
            ::TerminateProcess(process_, 0);
            ::CloseHandle(process_);
        }
    }

    BrokerProcess(const BrokerProcess&) = delete;
    BrokerProcess& operator=(const BrokerProcess&) = delete;

    [[nodiscard]] bool started() const noexcept { return started_; }

private:
    HANDLE process_ = nullptr;
    bool started_ = false;
};

// Connects, with a bounded retry: the broker has to create its pipe, spawn a
// worker and ask it for the graph before it accepts anyone.
bool Connect(ipc::Connection& connection) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (ipc::ConnectToBroker(connection, ipc::PipeName(), 500)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

bool Handshake(ipc::Connection& connection) {
    ipc::Hello hello;
    hello.kind = ipc::ClientKind::kCommandLine;
    hello.clientVersion = "tests";
    if (!connection.Send(hello)) return false;

    ipc::HelloAck ack;
    if (!connection.ReceiveAs(ack)) return false;
    return ack.protocolVersion == ipc::kProtocolVersion;
}

std::filesystem::path WriteMarkdown(const std::string& name) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("morph-e2e-" + std::to_string(::GetCurrentProcessId()));
    std::filesystem::create_directories(directory);

    const std::filesystem::path path = directory / name;
    std::ofstream stream(path, std::ios::binary);
    stream << "# Report\n\nA paragraph with a [link](https://example.com) in it.\n";
    return path;
}

// Collects everything the broker says about one job.
struct JobOutcome {
    ipc::JobAccepted accepted;
    std::vector<ipc::TaskDone> tasks;
    ipc::JobDone done;
    bool complete = false;
    std::string protocolError;
};

JobOutcome AwaitJob(ipc::Connection& connection) {
    JobOutcome outcome;
    bool acceptedSeen = false;

    while (true) {
        Result failed = Result::Ok();
        const std::optional<ipc::FrameReader::Frame> frame = connection.Receive(failed);
        if (!frame) {
            outcome.protocolError = failed.ok() ? "the broker closed the connection"
                                                : failed.message();
            return outcome;
        }

        switch (frame->type) {
            case ipc::MessageType::kJobAccepted:
                if (!Decode(frame->payload, outcome.accepted)) {
                    outcome.protocolError = "undecodable JobAccepted";
                    return outcome;
                }
                acceptedSeen = true;
                break;

            case ipc::MessageType::kProgress:
                break;  // coalesced already; the test does not depend on the rate

            case ipc::MessageType::kTaskDone: {
                ipc::TaskDone done;
                if (!Decode(frame->payload, done)) {
                    outcome.protocolError = "undecodable TaskDone";
                    return outcome;
                }
                outcome.tasks.push_back(std::move(done));
                break;
            }

            case ipc::MessageType::kJobDone:
                if (!Decode(frame->payload, outcome.done)) {
                    outcome.protocolError = "undecodable JobDone";
                    return outcome;
                }
                outcome.complete = acceptedSeen;
                return outcome;

            case ipc::MessageType::kError: {
                ipc::Error error;
                (void)Decode(frame->payload, error);
                outcome.protocolError = error.message;
                return outcome;
            }

            default:
                outcome.protocolError = "unexpected " + std::string(Name(frame->type));
                return outcome;
        }
    }
}

}  // namespace

TEST_CASE("The broker converts a file end to end", "[broker][integration]") {
    if (!std::filesystem::exists(BinaryDirectory() / "morph.broker.exe")) {
        SKIP("morph.broker.exe is not beside the tests");
    }

    const BrokerProcess broker;
    REQUIRE(broker.started());

    ipc::Connection connection;
    REQUIRE(Connect(connection));
    REQUIRE(Handshake(connection));

    SECTION("it answers what a format can become") {
        ipc::QueryTargets query;
        query.sourceProfiles = {"text/markdown"};
        query.largestInputBytes = 4096;
        REQUIRE(connection.Send(query));

        ipc::Targets targets;
        REQUIRE(connection.ReceiveAs(targets));
        REQUIRE_FALSE(targets.targets.empty());

        // The graph reached the broker from a worker, so this is also the test
        // that the description round trip carried something usable.
        const auto docx = std::find_if(targets.targets.begin(), targets.targets.end(),
                                       [](const ipc::TargetOption& option) {
                                           return option.profile == "application/docx";
                                       });
        REQUIRE(docx != targets.targets.end());
        CHECK(docx->hopCount >= 1u);
    }

    SECTION("it publishes a menu table that agrees with what it answers") {
        // Section 9.2's whole claim is that the shell extension can build the
        // submenu without asking. That is only true if the table it reads says
        // the same thing the broker would have said over the pipe.
        CapabilityMapping map;
        REQUIRE(map.OpenForRead());

        const CapabilityTable table = map.Table();
        REQUIRE(table.Valid());
        CHECK(table.Generation() > 0u);

        const std::span<const CapsTarget> published = table.TargetsFor(".DOCX");
        REQUIRE_FALSE(published.empty());

        ipc::QueryTargets query;
        query.sourceProfiles = {"application/docx"};
        REQUIRE(connection.Send(query));

        ipc::Targets answered;
        REQUIRE(connection.ReceiveAs(answered));

        for (const CapsTarget& target : published) {
            const std::string profile(table.String(target.profileOff));
            const auto match = std::find_if(answered.targets.begin(), answered.targets.end(),
                                            [&profile](const ipc::TargetOption& option) {
                                                return option.profile == profile;
                                            });
            INFO(profile);
            REQUIRE(match != answered.targets.end());
            CHECK(match->fidelity == target.fidelity);
            CHECK(match->hopCount == target.hopCount);
        }
    }

    SECTION("it converts a markdown file to docx") {
        const std::filesystem::path input = WriteMarkdown("report.md");
        const std::filesystem::path expected = input.parent_path() / "report.docx";
        std::error_code ec;
        std::filesystem::remove(expected, ec);

        ipc::SubmitJob submit;
        submit.inputPaths = {input.string()};
        submit.targetProfile = "application/docx";
        submit.interactive = true;
        REQUIRE(connection.Send(submit));

        const JobOutcome outcome = AwaitJob(connection);
        INFO(outcome.protocolError);
        REQUIRE(outcome.complete);
        REQUIRE(outcome.accepted.taskCount == 1u);
        REQUIRE(outcome.tasks.size() == 1u);

        INFO(outcome.tasks[0].error);
        CHECK(outcome.tasks[0].status == static_cast<std::uint8_t>(Status::kOk));
        CHECK(outcome.done.succeeded == 1u);

        // The plan the client was shown has to be the route that ran.
        REQUIRE(outcome.accepted.planProfiles.size() >= 2u);
        CHECK(outcome.accepted.planProfiles.front() == "text/markdown");
        CHECK(outcome.accepted.planProfiles.back() == "application/docx");

        CHECK(std::filesystem::exists(outcome.tasks[0].outputPath));
        CHECK(std::filesystem::file_size(outcome.tasks[0].outputPath) > 0);
    }

    SECTION("a file it cannot identify fails that task with a reason") {
        // Section 12.2: "Failures listed with a real reason, never 'conversion
        // failed'."
        const std::filesystem::path directory =
            std::filesystem::temp_directory_path() /
            ("morph-e2e-" + std::to_string(::GetCurrentProcessId()));
        std::filesystem::create_directories(directory);
        const std::filesystem::path input = directory / "mystery.bin";
        {
            std::ofstream stream(input, std::ios::binary);
            const unsigned char noise[] = {0x00, 0xFF, 0x00, 0xFF, 0x13, 0x37};
            stream.write(reinterpret_cast<const char*>(noise), sizeof(noise));
        }

        ipc::SubmitJob submit;
        submit.inputPaths = {input.string()};
        submit.targetProfile = "application/docx";
        REQUIRE(connection.Send(submit));

        const JobOutcome outcome = AwaitJob(connection);
        INFO(outcome.protocolError);
        REQUIRE(outcome.complete);
        REQUIRE(outcome.tasks.size() == 1u);
        CHECK(outcome.tasks[0].status != static_cast<std::uint8_t>(Status::kOk));
        CHECK_FALSE(outcome.tasks[0].error.empty());
        CHECK(outcome.done.failed == 1u);
    }
}
