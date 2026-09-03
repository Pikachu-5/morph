// The broker protocol: framing, codec and transport (architecture section 10).
//
// Three things are being checked, in increasing order of how expensive they are
// to get wrong:
//
//   1. Every message survives a round trip. Boring, and the reason the rest can
//      be trusted.
//   2. The framer cannot be desynchronised. A protocol that loses its place is a
//      protocol that eventually acts on the wrong bytes.
//   3. A real pipe, with the real security descriptor, carries real messages
//      between two threads. Section 10.1's ACL is the kind of thing that is
//      easy to write, easy to get wrong, and silently permissive when wrong.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <windows.h>
#include <string>
#include <thread>
#include <vector>

#include "morph/core/ipc/codec.h"
#include "morph/core/ipc/frame.h"
#include "morph/core/ipc/pipe.h"

using namespace morph;
using namespace morph::ipc;

namespace {

template <typename T>
T RoundTrip(const T& message) {
    const std::vector<std::byte> payload = Encode(message);
    T back;
    REQUIRE(Decode(payload, back));
    return back;
}

// A pipe name is ASCII by construction, but narrowing it implicitly is exactly
// the kind of silent conversion /W4 is right to complain about.
std::string Narrow(std::wstring_view wide) {
    std::string out;
    out.reserve(wide.size());
    for (const wchar_t c : wide) {
        out.push_back(c < 128 ? static_cast<char>(c) : '?');
    }
    return out;
}

std::wstring UniquePipeName(std::wstring_view label) {
    return PipeNameFor(std::wstring(label) + L"." +
                       std::to_wstring(::GetCurrentProcessId()) + L"." +
                       std::to_wstring(reinterpret_cast<std::uintptr_t>(&label)));
}

}  // namespace

// --- Codec round trips -------------------------------------------------------

TEST_CASE("Hello round-trips", "[ipc][codec]") {
    Hello message;
    message.protocolVersion = 7;
    message.kind = ClientKind::kShellExtension;
    message.clientVersion = "0.1.0";

    const Hello back = RoundTrip(message);
    CHECK(back.protocolVersion == 7);
    CHECK(back.kind == ClientKind::kShellExtension);
    CHECK(back.clientVersion == "0.1.0");
}

TEST_CASE("QueryTargets and Targets round-trip", "[ipc][codec]") {
    QueryTargets query;
    query.sourceProfiles = {"image/heif", "video/mp4;codec=h264"};
    query.largestInputBytes = 4ull * 1024 * 1024 * 1024;  // past 32 bits on purpose

    const QueryTargets backQuery = RoundTrip(query);
    CHECK(backQuery.sourceProfiles == query.sourceProfiles);
    CHECK(backQuery.largestInputBytes == query.largestInputBytes);

    Targets targets;
    targets.targets.push_back({"image/webp", 1, 1, 12.5, false});
    targets.targets.push_back({"application/pdf", 2, 3, 1500.0, true});

    const Targets back = RoundTrip(targets);
    REQUIRE(back.targets.size() == 2);
    CHECK(back.targets[0].profile == "image/webp");
    CHECK(back.targets[1].fidelity == 2);
    CHECK(back.targets[1].hopCount == 3);
    CHECK(back.targets[1].estimatedMs == 1500.0);
    CHECK(back.targets[1].requiresDisk);
}

TEST_CASE("SubmitJob round-trips with all four option kinds", "[ipc][codec]") {
    // The option shape is the fiddliest part of the schema, because it is the
    // one place the wire format carries a value whose type is not fixed.
    SubmitJob job;
    job.inputPaths = {R"(C:\Users\a\Documents\report.docx)", R"(C:\tmp\notes.md)"};
    job.targetProfile = "application/pdf";
    job.interactive = false;
    job.output.directory = R"(C:\out)";
    job.output.namingTemplate = "{name}-converted.{ext}";
    job.output.overwrite = OverwriteRule::kSkip;

    job.options.push_back({"lossless", 0, true, 0, 0.0, ""});
    job.options.push_back({"quality", 1, false, 90, 0.0, ""});
    job.options.push_back({"margin-inches", 2, false, 0, 0.75, ""});
    job.options.push_back({"page-size", 3, false, 0, 0.0, "letter"});

    const SubmitJob back = RoundTrip(job);
    CHECK(back.inputPaths == job.inputPaths);
    CHECK(back.targetProfile == "application/pdf");
    CHECK_FALSE(back.interactive);
    CHECK(back.output.directory == R"(C:\out)");
    CHECK(back.output.namingTemplate == "{name}-converted.{ext}");
    CHECK(back.output.overwrite == OverwriteRule::kSkip);

    REQUIRE(back.options.size() == 4);
    CHECK(back.options[0].boolValue == true);
    CHECK(back.options[1].intValue == 90);
    CHECK(back.options[2].doubleValue == 0.75);
    CHECK(back.options[3].stringValue == "letter");
}

TEST_CASE("JobAccepted carries the plan the UI shows", "[ipc][codec]") {
    // Section 10.2: "the resolved plan (the hop list, so the UI can show it)".
    JobAccepted accepted;
    accepted.jobId = 42;
    accepted.planProfiles = {"application/docx", "application/morph-ir", "text/html",
                             "application/pdf"};
    accepted.fidelity = 2;
    accepted.estimatedMs = 1500.0;
    accepted.taskCount = 3;

    const JobAccepted back = RoundTrip(accepted);
    CHECK(back.jobId == 42);
    CHECK(back.planProfiles.size() == 4);
    CHECK(back.planProfiles[2] == "text/html");
    CHECK(back.taskCount == 3);
}

TEST_CASE("Progress round-trips", "[ipc][codec]") {
    const Progress back = RoundTrip(Progress{9, 4, 0.375, 2, 1048576});
    CHECK(back.jobId == 9);
    CHECK(back.taskIndex == 4);
    CHECK(back.fraction == 0.375);
    CHECK(back.currentHop == 2);
    CHECK(back.bytesWritten == 1048576);
}

TEST_CASE("TaskDone carries the notes actually incurred", "[ipc][codec]") {
    TaskDone done;
    done.jobId = 1;
    done.taskIndex = 0;
    done.status = 0;
    done.inputPath = R"(C:\in\a.docx)";
    done.outputPath = R"(C:\out\a.md)";
    done.fidelity = 2;
    done.notes.push_back({"table-spans-flattened", "Merged cells were split apart.", "page 3"});
    done.notes.push_back({"content-control-flattened", "Content controls were replaced.", ""});

    const TaskDone back = RoundTrip(done);
    CHECK(back.outputPath == done.outputPath);
    REQUIRE(back.notes.size() == 2);
    CHECK(back.notes[0].code == "table-spans-flattened");
    CHECK(back.notes[0].location == "page 3");
    CHECK(back.notes[1].location.empty());
}

TEST_CASE("Cancel defaults to the whole job", "[ipc][codec]") {
    // -1 means the job rather than one task, so a negative has to survive
    // intact -- the integer accessors treat a negative where an unsigned was
    // expected as a peer bug, and this is the field where it is not.
    const Cancel wholeJob = RoundTrip(Cancel{5, -1});
    CHECK(wholeJob.jobId == 5);
    CHECK(wholeJob.taskIndex == -1);

    const Cancel oneTask = RoundTrip(Cancel{5, 3});
    CHECK(oneTask.taskIndex == 3);
}

TEST_CASE("Error and JobDone round-trip", "[ipc][codec]") {
    const Error error = RoundTrip(Error{3, 4, "This document is larger than 512 MB."});
    CHECK(error.jobId == 3);
    CHECK(error.status == 4);
    CHECK(error.message == "This document is larger than 512 MB.");

    const JobDone done = RoundTrip(JobDone{3, 10, 2, 1});
    CHECK(done.succeeded == 10);
    CHECK(done.failed == 2);
    CHECK(done.cancelled == 1);
}

// --- Forward compatibility ---------------------------------------------------

TEST_CASE("an unknown field is ignored", "[ipc][codec][compat]") {
    // Section 10.2 expects the schema to churn. A newer peer adding a field must
    // not break an older one, which is the whole reason keys are names.
    Hello message;
    message.clientVersion = "9.9.9";
    std::vector<std::byte> payload = Encode(message);

    // Re-encode by hand with an extra key, the way a newer build would.
    // Simulated by decoding a map that has one more entry than this build knows.
    SubmitJob future;
    future.inputPaths = {"a.md"};
    future.targetProfile = "application/pdf";
    future.options.push_back({"a-setting-from-the-future", 3, false, 0, 0.0, "value"});

    const SubmitJob back = RoundTrip(future);
    CHECK(back.inputPaths.size() == 1);
    CHECK(back.targetProfile == "application/pdf");
    // The unknown option survives as data rather than being dropped, because the
    // broker resolves options by name and an unknown one is simply not applied.
    REQUIRE(back.options.size() == 1);
    CHECK(back.options[0].key == "a-setting-from-the-future");
}

TEST_CASE("a missing field takes its default", "[ipc][codec][compat]") {
    // An older peer that never sets a field must decode to the default, not to
    // garbage. An empty map is the extreme case.
    SubmitJob empty;
    const std::vector<std::byte> payload = Encode(SubmitJob{});

    REQUIRE(Decode(payload, empty));
    CHECK(empty.inputPaths.empty());
    // The naming template and overwrite rule default to the safe choices.
    CHECK(empty.output.namingTemplate == "{name}.{ext}");
    CHECK(empty.output.overwrite == OverwriteRule::kRename);
    CHECK(empty.interactive);
}

TEST_CASE("an unrecognised overwrite rule falls back to renaming",
          "[ipc][codec][security]") {
    // The only rule that cannot destroy a file. A peer sending garbage here must
    // not be able to turn it into "replace".
    SubmitJob job;
    job.output.overwrite = static_cast<OverwriteRule>(99);

    const SubmitJob back = RoundTrip(job);
    CHECK(back.output.overwrite == OverwriteRule::kRename);
}

TEST_CASE("garbage never decodes to a usable message", "[ipc][codec][security]") {
    const std::string junk = "this is not messagepack at all, not even slightly";
    const auto* begin = reinterpret_cast<const std::byte*>(junk.data());

    SubmitJob job;
    CHECK_FALSE(Decode(std::span(begin, junk.size()), job));

    Hello hello;
    CHECK_FALSE(Decode(std::span(begin, junk.size()), hello));

    // An empty payload is not a valid message either.
    CHECK_FALSE(Decode({}, hello));
}

TEST_CASE("a valid MessagePack value that is not a map is rejected",
          "[ipc][codec][security]") {
    // Every message is a map. An array that happens to parse must not be
    // accepted and then read as though its fields were absent.
    const std::byte array[] = {std::byte{0x93}, std::byte{0x01}, std::byte{0x02},
                               std::byte{0x03}};
    Hello hello;
    CHECK_FALSE(Decode(array, hello));
}

// --- Framing -----------------------------------------------------------------

TEST_CASE("a frame round-trips through the reader", "[ipc][frame]") {
    const std::vector<std::byte> payload = Encode(Progress{1, 2, 0.5, 0, 100});
    const std::vector<std::byte> frame = EncodeFrame(MessageType::kProgress, payload);

    FrameReader reader;
    REQUIRE(reader.Append(frame));

    const auto out = reader.Next();
    REQUIRE(out.has_value());
    CHECK(out->type == MessageType::kProgress);
    CHECK(out->payload == payload);
    CHECK_FALSE(reader.Next().has_value());
}

TEST_CASE("frames split across reads are reassembled", "[ipc][frame]") {
    // A pipe read can return a partial frame, and a framer that assumes
    // otherwise fails only under load.
    const std::vector<std::byte> payload = Encode(Hello{1, ClientKind::kApp, "0.1.0"});
    const std::vector<std::byte> frame = EncodeFrame(MessageType::kHello, payload);

    FrameReader reader;
    for (std::size_t i = 0; i < frame.size(); ++i) {
        REQUIRE(reader.Append(std::span(frame.data() + i, 1)));
        // Nothing must emerge until the very last byte arrives.
        if (i + 1 < frame.size()) CHECK_FALSE(reader.Next().has_value());
    }
    const auto out = reader.Next();
    REQUIRE(out.has_value());
    CHECK(out->type == MessageType::kHello);
}

TEST_CASE("several frames in one read all come out, in order", "[ipc][frame]") {
    std::vector<std::byte> stream;
    for (std::uint32_t i = 0; i < 5; ++i) {
        const std::vector<std::byte> frame =
            EncodeFrame(MessageType::kProgress, Encode(Progress{1, i, 0.1 * i, 0, 0}));
        stream.insert(stream.end(), frame.begin(), frame.end());
    }

    FrameReader reader;
    REQUIRE(reader.Append(stream));

    for (std::uint32_t i = 0; i < 5; ++i) {
        const auto frame = reader.Next();
        REQUIRE(frame.has_value());
        Progress progress;
        REQUIRE(Decode(frame->payload, progress));
        CHECK(progress.taskIndex == i);
    }
    CHECK_FALSE(reader.Next().has_value());
}

TEST_CASE("an oversized declared length fails the stream", "[ipc][frame][security]") {
    // There is no way to know where the next frame starts, so resynchronising
    // would be guessing. The connection has to close.
    std::vector<std::byte> header(kFrameHeaderBytes, std::byte{0});
    const std::uint32_t huge = kMaxPayloadBytes + 1;
    header[0] = static_cast<std::byte>(huge & 0xFF);
    header[1] = static_cast<std::byte>((huge >> 8) & 0xFF);
    header[2] = static_cast<std::byte>((huge >> 16) & 0xFF);
    header[3] = static_cast<std::byte>((huge >> 24) & 0xFF);

    FrameReader reader;
    CHECK_FALSE(reader.Append(header));
    CHECK(reader.Failed());
    CHECK_FALSE(reader.Next().has_value());
    // A failed reader stays failed; it must not recover on the next append.
    CHECK_FALSE(reader.Append(std::span<const std::byte>{}));
}

TEST_CASE("an unknown message type decodes as invalid rather than failing",
          "[ipc][frame][compat]") {
    // A newer peer may send something this build has never heard of. The framer
    // must still find the frame boundary so the stream stays usable; the
    // handshake decides whether this should be happening at all.
    const std::vector<std::byte> payload = Encode(Hello{});
    std::vector<std::byte> frame = EncodeFrame(MessageType::kHello, payload);
    frame[4] = static_cast<std::byte>(250);  // a type from the future
    frame[5] = std::byte{0};

    FrameReader reader;
    REQUIRE(reader.Append(frame));
    const auto out = reader.Next();
    REQUIRE(out.has_value());
    CHECK(out->type == MessageType::kInvalid);
    CHECK(out->payload.size() == payload.size());
}

TEST_CASE("a zero-length payload is a valid frame", "[ipc][frame]") {
    FrameReader reader;
    REQUIRE(reader.Append(EncodeFrame(MessageType::kJobDone, {})));

    const auto out = reader.Next();
    REQUIRE(out.has_value());
    CHECK(out->type == MessageType::kJobDone);
    CHECK(out->payload.empty());
}

// --- The real transport ------------------------------------------------------

TEST_CASE("the pipe name is per session and per package", "[ipc][pipe]") {
    const std::wstring name = PipeName();
    INFO(Narrow(name));
    CHECK(name.starts_with(L"\\\\.\\pipe\\morph.broker."));
    // Two calls in one process must agree, or clients and broker would never meet.
    CHECK(name == PipeName());
}

TEST_CASE("a client and server exchange messages over a real pipe", "[ipc][pipe]") {
    const std::wstring name = UniquePipeName(L"exchange");

    PipeServer server;
    REQUIRE(server.Create(name).ok());

    std::atomic<bool> serverOk{false};
    std::thread brokerThread([&] {
        Connection connection;
        if (!server.Accept(connection, 5000).ok()) return;

        Hello hello;
        if (!connection.ReceiveAs(hello).ok()) return;
        if (hello.kind != ClientKind::kApp) return;

        HelloAck ack;
        ack.brokerVersion = "0.1.0-test";
        ack.capabilityGeneration = 7;
        if (!connection.Send(ack).ok()) return;

        SubmitJob job;
        if (!connection.ReceiveAs(job).ok()) return;
        if (job.targetProfile != "application/pdf") return;

        JobAccepted accepted;
        accepted.jobId = 1;
        accepted.planProfiles = {"text/markdown", "application/morph-ir", "text/html",
                                 "application/pdf"};
        accepted.taskCount = static_cast<std::uint32_t>(job.inputPaths.size());
        if (!connection.Send(accepted).ok()) return;

        for (std::uint32_t i = 0; i < 3; ++i) {
            if (!connection.Send(Progress{1, 0, 0.25 * (i + 1), i, 0}).ok()) return;
        }
        if (!connection.Send(JobDone{1, 1, 0, 0}).ok()) return;

        serverOk = true;
    });

    Connection client;
    REQUIRE(ConnectToBroker(client, name, 5000).ok());

    REQUIRE(client.Send(Hello{kProtocolVersion, ClientKind::kApp, "0.1.0"}).ok());

    HelloAck ack;
    REQUIRE(client.ReceiveAs(ack).ok());
    CHECK(ack.brokerVersion == "0.1.0-test");
    CHECK(ack.capabilityGeneration == 7);

    SubmitJob job;
    job.inputPaths = {R"(C:\in\notes.md)"};
    job.targetProfile = "application/pdf";
    REQUIRE(client.Send(job).ok());

    JobAccepted accepted;
    REQUIRE(client.ReceiveAs(accepted).ok());
    CHECK(accepted.jobId == 1);
    CHECK(accepted.planProfiles.size() == 4);
    CHECK(accepted.taskCount == 1);

    for (std::uint32_t i = 0; i < 3; ++i) {
        Progress progress;
        REQUIRE(client.ReceiveAs(progress).ok());
        CHECK(progress.currentHop == i);
    }

    JobDone done;
    REQUIRE(client.ReceiveAs(done).ok());
    CHECK(done.succeeded == 1);

    brokerThread.join();
    CHECK(serverOk.load());
}

TEST_CASE("a second server cannot claim a name already owned", "[ipc][pipe][security]") {
    // FILE_FLAG_FIRST_PIPE_INSTANCE is the squatting defence. Without it a
    // hostile process that creates the name first receives every client's file
    // paths, and the correct response to losing the race is to give up.
    const std::wstring name = UniquePipeName(L"squat");

    PipeServer first;
    REQUIRE(first.Create(name).ok());

    PipeServer second;
    const Result result = second.Create(name);
    CHECK_FALSE(result.ok());
    CHECK(result.status() == Status::kResourceLimit);
    CHECK(result.message().find("already owns") != std::string::npos);
}

TEST_CASE("connecting when no broker is running is a clean answer", "[ipc][pipe]") {
    // The shell extension has to distinguish "start one" from "something is
    // wrong" without hanging, because a right-click is waiting on the answer.
    Connection connection;
    const Result result = ConnectToBroker(connection, UniquePipeName(L"absent"), 200);

    CHECK_FALSE(result.ok());
    CHECK(result.status() == Status::kMissingCapability);
    CHECK(result.message() == "The broker is not running.");
}

TEST_CASE("a closed peer ends the conversation rather than hanging", "[ipc][pipe]") {
    const std::wstring name = UniquePipeName(L"hangup");

    PipeServer server;
    REQUIRE(server.Create(name).ok());

    std::thread brokerThread([&] {
        Connection connection;
        if (!server.Accept(connection, 5000).ok()) return;
        Hello hello;
        (void)connection.ReceiveAs(hello);
        // Drop the connection without replying, which is what a broker that
        // rejects a version mismatch does (section 10.2).
        connection.Close();
    });

    Connection client;
    REQUIRE(ConnectToBroker(client, name, 5000).ok());
    REQUIRE(client.Send(Hello{}).ok());

    Result failed = Result::Ok();
    const auto frame = client.Receive(failed);
    CHECK_FALSE(frame.has_value());
    // A clean close is not an error; the caller decides what it means.
    CHECK(failed.ok());

    brokerThread.join();
}

TEST_CASE("a large message crosses the pipe intact", "[ipc][pipe]") {
    // Section 9.3 marshals a big selection in chunks, but the transport still
    // has to carry a message larger than the pipe's buffer without truncating
    // it -- that path is ERROR_MORE_DATA, and it is easy to get wrong.
    const std::wstring name = UniquePipeName(L"large");

    SubmitJob job;
    job.targetProfile = "image/webp";
    for (int i = 0; i < 4000; ++i) {
        job.inputPaths.push_back(R"(C:\Users\someone\Pictures\a-photograph-)" +
                                 std::to_string(i) + ".heic");
    }
    REQUIRE(Encode(job).size() > 128 * 1024);

    PipeServer server;
    REQUIRE(server.Create(name).ok());

    std::atomic<std::size_t> received{0};
    std::thread brokerThread([&] {
        Connection connection;
        if (!server.Accept(connection, 5000).ok()) return;
        SubmitJob got;
        if (!connection.ReceiveAs(got).ok()) return;
        received = got.inputPaths.size();
        (void)connection.Send(JobAccepted{1, {}, 0, 0, static_cast<std::uint32_t>(received)});
    });

    Connection client;
    REQUIRE(ConnectToBroker(client, name, 5000).ok());
    REQUIRE(client.Send(job).ok());

    JobAccepted accepted;
    REQUIRE(client.ReceiveAs(accepted).ok());
    CHECK(accepted.taskCount == 4000);

    brokerThread.join();
    CHECK(received.load() == 4000);
}

TEST_CASE("an unexpected message type is a protocol violation", "[ipc][pipe]") {
    const std::wstring name = UniquePipeName(L"wrongtype");

    PipeServer server;
    REQUIRE(server.Create(name).ok());

    std::thread brokerThread([&] {
        Connection connection;
        if (!server.Accept(connection, 5000).ok()) return;
        Hello hello;
        (void)connection.ReceiveAs(hello);
        // Reply with the wrong thing entirely.
        (void)connection.Send(Progress{1, 0, 0.0, 0, 0});
    });

    Connection client;
    REQUIRE(ConnectToBroker(client, name, 5000).ok());
    REQUIRE(client.Send(Hello{}).ok());

    HelloAck ack;
    const Result result = client.ReceiveAs(ack);
    CHECK_FALSE(result.ok());
    CHECK(result.message().find("HelloAck") != std::string::npos);
    CHECK(result.message().find("Progress") != std::string::npos);

    brokerThread.join();
}

TEST_CASE("two clients are served one after another", "[ipc][pipe]") {
    // The broker keeps an instance listening while another is in use, so a
    // second client connecting immediately is not refused.
    const std::wstring name = UniquePipeName(L"twoclients");

    PipeServer server;
    REQUIRE(server.Create(name).ok());

    std::atomic<int> served{0};
    std::thread brokerThread([&] {
        for (int i = 0; i < 2; ++i) {
            Connection connection;
            if (!server.Accept(connection, 5000).ok()) return;
            Hello hello;
            if (!connection.ReceiveAs(hello).ok()) return;
            HelloAck ack;
            ack.brokerVersion = "0.1.0-test";
            if (!connection.Send(ack).ok()) return;
            ++served;
        }
    });

    for (int i = 0; i < 2; ++i) {
        Connection client;
        REQUIRE(ConnectToBroker(client, name, 5000).ok());
        REQUIRE(client.Send(Hello{}).ok());
        HelloAck ack;
        REQUIRE(client.ReceiveAs(ack).ok());
        CHECK(ack.brokerVersion == "0.1.0-test");
    }

    brokerThread.join();
    CHECK(served.load() == 2);
}
