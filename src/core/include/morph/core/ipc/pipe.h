// Morph — the named-pipe transport (architecture section 10.1).
//
// "Named pipes, message mode, one instance per user session. You already know
//  this pattern from the PowerToys batch work; the additions here are the
//  security descriptor and the framing."
//
// The security descriptor is the part that is easy to leave until later and
// impossible to retrofit safely. A named pipe with a default DACL is reachable
// by anything running as any user on the machine, and the broker's job is to
// take file paths from a client and convert them -- which is a confused deputy
// waiting to happen. So:
//
//   D:(A;;GA;;;OW)(A;;GA;;;<current user SID>)(D;;GA;;;NU)
//
//   owner gets everything, the creating user gets everything, and NETWORK is
//   denied outright. PIPE_REJECT_REMOTE_CLIENTS says the same thing a second
//   way, because defence that depends on one flag being right is not defence.
//
// FILE_FLAG_FIRST_PIPE_INSTANCE is the squatting defence. Without it a hostile
// process that creates the pipe name first receives every client's connection
// and every client's file paths. With it, creation fails when someone already
// owns the name -- and the correct response to that failure is to give up, not
// to connect as a client.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "morph/core/ipc/codec.h"
#include "morph/core/ipc/frame.h"
#include "morph/core/result.h"

namespace morph::ipc {

// The pipe name for this user session.
//
// Section 10.1 spells it "\\.\pipe\morph.broker.<sessionId>.<packageFamilyHash>".
// The session id keeps two logged-in users apart; the package family hash keeps
// a side-loaded build from colliding with a Store install of the same product,
// which otherwise produces a broker and a client that disagree about which
// providers exist.
[[nodiscard]] std::wstring PipeName();

// Overload for tests, which need a name nothing else will claim.
[[nodiscard]] std::wstring PipeNameFor(std::wstring_view suffix);

// One end of a connection. Both the server and the client hand one of these back
// so that message exchange is written once.
class Connection {
public:
    // Starts closed. Both PipeServer::Accept and ConnectToBroker take one by
    // reference and fill it in, so a caller always declares the object first.
    Connection() = default;
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;

    // Sends one message. Blocks until the whole frame is written.
    template <typename T>
    [[nodiscard]] Result Send(const T& message) {
        return SendFrame(TypeOf<T>(), Encode(message));
    }

    [[nodiscard]] Result SendFrame(MessageType type, std::span<const std::byte> payload);

    // Waits for the next frame. Returns nullopt when the peer closed cleanly;
    // `failed` distinguishes that from an error.
    [[nodiscard]] std::optional<FrameReader::Frame> Receive(Result& failed);

    // Decodes into `into` when the next frame is of the expected type. A frame of
    // any other type is a protocol violation, because the protocol is
    // request/response per client.
    template <typename T>
    [[nodiscard]] Result ReceiveAs(T& into) {
        Result failed = Result::Ok();
        const std::optional<FrameReader::Frame> frame = Receive(failed);
        if (!frame) {
            return failed.ok() ? Result::Error(Status::kIoError,
                                               "The connection closed before a reply arrived.")
                               : failed;
        }
        if (frame->type != TypeOf<T>()) {
            return Result::Error(Status::kInternal,
                                 std::string("Expected a ") + std::string(Name(TypeOf<T>())) +
                                     " message but received " + std::string(Name(frame->type)) +
                                     ".");
        }
        if (!Decode(frame->payload, into)) {
            return Result::Error(Status::kMalformedInput,
                                 std::string("The ") + std::string(Name(frame->type)) +
                                     " message could not be decoded.");
        }
        return Result::Ok();
    }

    void Close() noexcept;
    [[nodiscard]] bool Open() const noexcept;

private:
    friend class PipeServer;
    friend Result ConnectToBroker(Connection&, const std::wstring&, std::uint32_t);

    void* handle_ = nullptr;  // HANDLE, kept opaque so this header stays portable
    FrameReader reader_;

    // One event per direction, for the overlapped I/O the transport is built on.
    //
    // The alternative -- a plain synchronous handle -- cannot carry a duplex
    // conversation at all. Windows serialises I/O on a handle opened without
    // FILE_FLAG_OVERLAPPED, so a thread blocked in ReadFile holds the handle
    // while a WriteFile from another thread waits behind it. Both sides of this
    // protocol read on one thread and write on another (the worker reports
    // progress while listening for Cancel; the broker's session thread reads
    // while job threads report), so that deadlock is not a corner case, it is
    // the steady state.
    //
    // Two events rather than one because a read and a write really do overlap,
    // and an OVERLAPPED sharing an event with another operation in flight is
    // undefined behaviour rather than a race that shows up under load.
    void* readEvent_ = nullptr;
    void* writeEvent_ = nullptr;

    [[nodiscard]] void* EventFor(bool read);
};

// Creates the pipe and accepts clients. Only the broker uses this.
class PipeServer {
public:
    PipeServer() = default;
    ~PipeServer();

    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    // Creates the first instance of the pipe. Fails with kResourceLimit when the
    // name is already owned, which means another broker is running -- or someone
    // is squatting. Either way this process must not become the broker.
    [[nodiscard]] Result Create(const std::wstring& name);

    // Waits for a client. `timeoutMs` of 0 polls; a negative value waits
    // indefinitely. Returns kCancelled on timeout so the broker's idle loop can
    // distinguish "nothing yet" from "something broke".
    [[nodiscard]] Result Accept(Connection& into, int timeoutMs);

    void Close() noexcept;

private:
    [[nodiscard]] Result CreateInstance(bool first);

    std::wstring name_;
    void* pending_ = nullptr;  // the instance currently waiting for a client
};

// Connects to a running broker. Does not start one: section 10.3 makes starting
// the broker the caller's decision, because only the caller knows whether it is
// allowed to spawn a process.
[[nodiscard]] Result ConnectToBroker(Connection& into, const std::wstring& name,
                                     std::uint32_t timeoutMs = 5000);

}  // namespace morph::ipc
