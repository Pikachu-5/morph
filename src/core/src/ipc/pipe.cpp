#include "morph/core/ipc/pipe.h"

#include <algorithm>
#include <string>

#include <windows.h>

#include <aclapi.h>
#include <appmodel.h>  // GetCurrentPackageFamilyName
#include <sddl.h>

namespace morph::ipc {
namespace {

constexpr DWORD kOutBufferBytes = 64 * 1024;
constexpr DWORD kInBufferBytes = 64 * 1024;
constexpr DWORD kDefaultTimeoutMs = 5000;

std::wstring CurrentUserSid() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    struct Closer {
        HANDLE handle;
        ~Closer() { ::CloseHandle(handle); }
    } closer{token};

    DWORD needed = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) return {};

    std::vector<std::byte> buffer(needed);
    if (!::GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed)) return {};

    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR text = nullptr;
    if (!::ConvertSidToStringSidW(user->User.Sid, &text) || text == nullptr) return {};

    std::wstring sid(text);
    ::LocalFree(text);
    return sid;
}

// Section 10.1's descriptor, built from the running user's actual SID rather
// than a well-known alias. "Current user" has no fixed SID, so it has to be
// looked up; falling back to a descriptor without it would silently widen access
// and is therefore treated as a failure by the caller.
bool BuildSecurityAttributes(SECURITY_ATTRIBUTES& attributes, PSECURITY_DESCRIPTOR& descriptor) {
    const std::wstring sid = CurrentUserSid();
    if (sid.empty()) return false;

    // OW  = owner rights
    // sid = the user this process runs as
    // NU  = NETWORK, denied. A deny ACE precedes the allow ACEs deliberately:
    //       Windows evaluates a DACL in order and a deny must be reachable
    //       before any allow that would otherwise grant the access.
    const std::wstring sddl = L"D:(D;;GA;;;NU)(A;;GA;;;OW)(A;;GA;;;" + sid + L")";

    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        return false;
    }

    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    return true;
}

std::wstring PackageFamilyTag() {
    // Inside an MSIX the package family name is the right discriminator. Outside
    // one -- a developer build, or the tests -- GetCurrentPackageFamilyName
    // fails, and the executable's directory is a good enough stand-in: it keeps
    // two side-by-side checkouts from sharing a broker.
    UINT32 length = 0;
    const LONG rc = ::GetCurrentPackageFamilyName(&length, nullptr);
    if (rc == ERROR_INSUFFICIENT_BUFFER && length > 0) {
        std::wstring name(length, L'\0');
        if (::GetCurrentPackageFamilyName(&length, name.data()) == ERROR_SUCCESS) {
            name.resize(length == 0 ? 0 : length - 1);
            return name;
        }
    }

    wchar_t path[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);

    // FNV-1a over the path, rendered as hex. A hash rather than the path itself
    // because a pipe name has a length limit and paths do not.
    std::uint64_t hash = 1469598103934665603ull;
    for (const wchar_t c : std::wstring_view(path)) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ull;
    }
    wchar_t hex[17]{};
    ::swprintf_s(hex, L"%016llx", static_cast<unsigned long long>(hash));
    return hex;
}

Result LastErrorAs(Status status, std::string_view what) {
    const DWORD code = ::GetLastError();
    return Result::Error(status, std::string(what) + " (Windows error " + std::to_string(code) +
                                     ").");
}

}  // namespace

std::wstring PipeName() {
    DWORD session = 0;
    ::ProcessIdToSessionId(::GetCurrentProcessId(), &session);
    return L"\\\\.\\pipe\\morph.broker." + std::to_wstring(session) + L"." + PackageFamilyTag();
}

std::wstring PipeNameFor(std::wstring_view suffix) {
    DWORD session = 0;
    ::ProcessIdToSessionId(::GetCurrentProcessId(), &session);
    return L"\\\\.\\pipe\\morph.broker." + std::to_wstring(session) + L"." +
           std::wstring(suffix);
}

// --- Connection --------------------------------------------------------------

Connection::~Connection() { Close(); }

Connection::Connection(Connection&& other) noexcept
    : handle_(other.handle_), reader_(std::move(other.reader_)) {
    other.handle_ = nullptr;
}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        Close();
        handle_ = other.handle_;
        reader_ = std::move(other.reader_);
        other.handle_ = nullptr;
    }
    return *this;
}

bool Connection::Open() const noexcept {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
}

void Connection::Close() noexcept {
    if (!Open()) {
        handle_ = nullptr;
        return;
    }
    // Let the peer drain what has already been written before the handle goes
    // away, so a final JobDone is not lost to a race with process exit.
    ::FlushFileBuffers(static_cast<HANDLE>(handle_));
    ::CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
}

Result Connection::SendFrame(MessageType type, std::span<const std::byte> payload) {
    if (!Open()) {
        return Result::Error(Status::kIoError, "The connection is not open.");
    }
    if (payload.size() > kMaxPayloadBytes) {
        return Result::Error(Status::kResourceLimit,
                             "This message is too large to send over the broker pipe.");
    }

    const std::vector<std::byte> frame = EncodeFrame(type, payload);

    // One WriteFile per frame. In message mode the pipe preserves the boundary,
    // so splitting a frame across writes would split it across messages too.
    std::size_t written = 0;
    while (written < frame.size()) {
        DWORD wrote = 0;
        const DWORD want = static_cast<DWORD>(
            std::min<std::size_t>(frame.size() - written, 1024u * 1024u));
        if (!::WriteFile(static_cast<HANDLE>(handle_), frame.data() + written, want, &wrote,
                         nullptr) ||
            wrote == 0) {
            return LastErrorAs(Status::kIoError, "Could not write to the broker pipe");
        }
        written += wrote;
    }
    return Result::Ok();
}

std::optional<FrameReader::Frame> Connection::Receive(Result& failed) {
    failed = Result::Ok();
    if (!Open()) {
        failed = Result::Error(Status::kIoError, "The connection is not open.");
        return std::nullopt;
    }

    // A frame may already be buffered from a previous read: message mode
    // preserves boundaries but a single ReadFile can still return several
    // frames' worth when the peer wrote them back to back.
    if (std::optional<FrameReader::Frame> frame = reader_.Next()) return frame;

    std::vector<std::byte> chunk(kInBufferBytes);
    while (true) {
        DWORD got = 0;
        const BOOL ok = ::ReadFile(static_cast<HANDLE>(handle_), chunk.data(),
                                   static_cast<DWORD>(chunk.size()), &got, nullptr);
        if (!ok) {
            const DWORD code = ::GetLastError();
            if (code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED) {
                return std::nullopt;  // a clean close, not an error
            }
            if (code != ERROR_MORE_DATA) {
                failed = Result::Error(Status::kIoError,
                                       "Lost the connection to the broker (Windows error " +
                                           std::to_string(code) + ").");
                return std::nullopt;
            }
            // ERROR_MORE_DATA: the message is longer than the buffer, and `got`
            // bytes are valid. Fall through and read the rest.
        }
        if (got == 0 && ok) return std::nullopt;

        if (!reader_.Append(std::span(chunk.data(), got))) {
            failed = Result::Error(Status::kMalformedInput,
                                   "The broker connection sent a malformed frame.");
            return std::nullopt;
        }
        if (std::optional<FrameReader::Frame> frame = reader_.Next()) return frame;
    }
}

// --- PipeServer --------------------------------------------------------------

PipeServer::~PipeServer() { Close(); }

void PipeServer::Close() noexcept {
    if (pending_ != nullptr && pending_ != INVALID_HANDLE_VALUE) {
        ::CloseHandle(static_cast<HANDLE>(pending_));
    }
    pending_ = nullptr;
}

Result PipeServer::CreateInstance(bool first) {
    SECURITY_ATTRIBUTES attributes{};
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!BuildSecurityAttributes(attributes, descriptor)) {
        return Result::Error(Status::kInternal,
                             "Could not build the broker pipe's security descriptor. Refusing "
                             "to create the pipe with default permissions.");
    }
    struct Freer {
        PSECURITY_DESCRIPTOR descriptor;
        ~Freer() { if (descriptor != nullptr) ::LocalFree(descriptor); }
    } freer{descriptor};

    DWORD openMode = PIPE_ACCESS_DUPLEX;
    // Only the FIRST instance carries the squatting defence; later instances of
    // a name this process already owns must not.
    if (first) openMode |= FILE_FLAG_FIRST_PIPE_INSTANCE;

    const HANDLE handle = ::CreateNamedPipeW(
        name_.c_str(), openMode,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, kOutBufferBytes, kInBufferBytes, kDefaultTimeoutMs,
        &attributes);

    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        if (first && (code == ERROR_ACCESS_DENIED || code == ERROR_PIPE_BUSY ||
                      code == ERROR_ALREADY_EXISTS)) {
            // Somebody already owns the name. That is either a broker that beat
            // us to it or a squatter, and in both cases this process must not
            // become the broker.
            return Result::Error(Status::kResourceLimit,
                                 "Another process already owns the broker pipe.");
        }
        return LastErrorAs(Status::kInternal, "Could not create the broker pipe");
    }

    pending_ = handle;
    return Result::Ok();
}

Result PipeServer::Create(const std::wstring& name) {
    name_ = name;
    return CreateInstance(/*first=*/true);
}

Result PipeServer::Accept(Connection& into, int timeoutMs) {
    if (pending_ == nullptr) {
        return Result::Error(Status::kInternal, "The broker pipe has not been created.");
    }

    // ConnectNamedPipe on a blocking handle waits indefinitely, which the idle
    // loop cannot use. Overlapped I/O with an event gives a timeout without
    // making the rest of the transport asynchronous.
    OVERLAPPED overlapped{};
    overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        return LastErrorAs(Status::kInternal, "Could not create the accept event");
    }
    struct Closer {
        HANDLE handle;
        ~Closer() { ::CloseHandle(handle); }
    } closer{overlapped.hEvent};

    // A blocking-mode handle cannot take an OVERLAPPED, so the wait is done with
    // a poll loop instead: ConnectNamedPipe returns immediately when a client is
    // already queued, which is the common case under load.
    const DWORD deadline =
        timeoutMs < 0 ? 0 : ::GetTickCount() + static_cast<DWORD>(timeoutMs);

    while (true) {
        if (::ConnectNamedPipe(static_cast<HANDLE>(pending_), nullptr)) break;

        const DWORD code = ::GetLastError();
        if (code == ERROR_PIPE_CONNECTED) break;  // connected between create and connect
        if (code != ERROR_NO_DATA && code != ERROR_PIPE_LISTENING) {
            return Result::Error(Status::kIoError,
                                 "Could not accept a broker connection (Windows error " +
                                     std::to_string(code) + ").");
        }
        if (timeoutMs >= 0 && ::GetTickCount() >= deadline) {
            return Result::Cancelled();
        }
        ::Sleep(10);
    }

    into.Close();
    into.handle_ = pending_;
    into.reader_.Reset();
    pending_ = nullptr;

    // Have the next instance listening before this one is handed to a client, so
    // a second client connecting immediately is not refused.
    if (Result next = CreateInstance(/*first=*/false); !next) return next;
    return Result::Ok();
}

// --- Client ------------------------------------------------------------------

Result ConnectToBroker(Connection& into, const std::wstring& name, std::uint32_t timeoutMs) {
    const DWORD deadline = ::GetTickCount() + timeoutMs;

    while (true) {
        const HANDLE handle =
            ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                          0, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            if (!::SetNamedPipeHandleState(handle, &mode, nullptr, nullptr)) {
                ::CloseHandle(handle);
                return LastErrorAs(Status::kInternal,
                                   "Could not put the broker pipe into message mode");
            }
            into.Close();
            into.handle_ = handle;
            into.reader_.Reset();
            return Result::Ok();
        }

        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND) {
            return Result::Error(Status::kMissingCapability, "The broker is not running.");
        }
        if (code != ERROR_PIPE_BUSY) {
            return Result::Error(Status::kIoError,
                                 "Could not connect to the broker (Windows error " +
                                     std::to_string(code) + ").");
        }
        // Every instance is busy. Wait for one, but never past the caller's
        // deadline: a client that blocks forever here is a hung right-click.
        if (::GetTickCount() >= deadline) {
            return Result::Error(Status::kIoError, "The broker did not accept a connection.");
        }
        if (!::WaitNamedPipeW(name.c_str(), 100)) continue;
    }
}

}  // namespace morph::ipc
