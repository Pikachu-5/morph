#include "ffmpeg_process.h"

#include <algorithm>

#include <windows.h>

namespace morph::ffmpeg {
namespace {

constexpr std::size_t kMaxDiagnosticsBytes = 16 * 1024;
constexpr DWORD kPollIntervalMs = 25;

// A pipe end pair that closes itself. Four handles have to be closed in the
// right order around a CreateProcess, and doing it by hand is how a parent ends
// up waiting on a pipe that will never see EOF because it still holds the write
// end.
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE handle) : handle_(handle) {}
    ~Handle() { Close(); }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            Close();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    void Close() noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_);
        handle_ = nullptr;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] HANDLE* put() noexcept { return &handle_; }
    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_ = nullptr;
};

Result LastErrorAs(Status status, std::string_view what) {
    const DWORD code = ::GetLastError();
    return Result::Error(status,
                         std::string(what) + " (Windows error " + std::to_string(code) + ").");
}

// Reads whatever is available without blocking. PeekNamedPipe works on anonymous
// pipes too, which is what makes a poll loop possible without overlapped I/O on
// a handle the child also holds.
bool DrainPipe(HANDLE pipe, std::string& into) {
    while (true) {
        DWORD available = 0;
        if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available == 0) return true;

        char buffer[4096];
        DWORD got = 0;
        const DWORD want = std::min<DWORD>(available, sizeof(buffer));
        if (!::ReadFile(pipe, buffer, want, &got, nullptr) || got == 0) return false;
        into.append(buffer, got);
    }
}

// Hands every complete line to the callback and keeps the remainder.
void EmitLines(std::string& buffer, const std::function<void(std::string_view)>& onLine) {
    if (!onLine) {
        buffer.clear();
        return;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t newline = buffer.find('\n', start);
        if (newline == std::string::npos) break;
        onLine(std::string_view(buffer).substr(start, newline - start));
        start = newline + 1;
    }
    buffer.erase(0, start);
}

struct Child {
    Handle process;
    Handle job;
    Handle stdoutRead;
    Handle stderrRead;
};

Result Spawn(const std::wstring& executable, const std::vector<std::wstring>& arguments,
             Child& into) {
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    Handle stdoutWrite;
    Handle stderrWrite;
    if (!::CreatePipe(into.stdoutRead.put(), stdoutWrite.put(), &inheritable, 64 * 1024) ||
        !::CreatePipe(into.stderrRead.put(), stderrWrite.put(), &inheritable, 64 * 1024)) {
        return LastErrorAs(Status::kInternal, "Could not create a pipe for FFmpeg");
    }
    // Only the child inherits the write ends; the read ends must NOT be
    // inheritable or the child holds a copy and the parent never sees EOF.
    ::SetHandleInformation(into.stdoutRead.get(), HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(into.stderrRead.get(), HANDLE_FLAG_INHERIT, 0);

    const HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) return LastErrorAs(Status::kInternal, "Could not contain FFmpeg");
    into.job = Handle(job);

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    (void)::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                    sizeof(limits));

    std::wstring command = QuoteArgument(executable);
    for (const std::wstring& argument : arguments) {
        command += L' ';
        command += QuoteArgument(argument);
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = stdoutWrite.get();
    startup.hStdError = stderrWrite.get();
    // No stdin at all. -nostdin tells FFmpeg not to read one; this makes sure
    // there is nothing there to read even if a future version forgets.
    startup.hStdInput = nullptr;

    PROCESS_INFORMATION info{};
    if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                          CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                          &info)) {
        return LastErrorAs(Status::kMissingCapability, "Could not start FFmpeg");
    }

    if (!::AssignProcessToJobObject(job, info.hProcess)) {
        ::TerminateProcess(info.hProcess, 1);
        ::CloseHandle(info.hThread);
        ::CloseHandle(info.hProcess);
        return LastErrorAs(Status::kInternal, "Could not contain FFmpeg in its job object");
    }
    ::ResumeThread(info.hThread);
    ::CloseHandle(info.hThread);
    into.process = Handle(info.hProcess);

    // The parent's copies of the write ends go now, so that EOF on the read ends
    // means the child is done writing rather than "someone still holds this".
    stdoutWrite.Close();
    stderrWrite.Close();
    return Result::Ok();
}

}  // namespace

std::wstring QuoteArgument(const std::wstring& argument) {
    // The rule CommandLineToArgvW implements: backslashes are literal unless
    // they precede a quote, in which case they double and the quote escapes.
    if (!argument.empty() &&
        argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }

    std::wstring quoted;
    quoted.push_back(L'"');
    for (auto it = argument.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != argument.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }

        if (it == argument.end()) {
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
        } else {
            quoted.append(backslashes, L'\\');
        }
        quoted.push_back(*it);
    }
    quoted.push_back(L'"');
    return quoted;
}

Result RunProcess(const std::wstring& executable, const std::vector<std::wstring>& arguments,
                  const std::function<void(std::string_view)>& onLine,
                  const std::function<bool()>& shouldStop, RunResult& into) {
    Child child;
    if (const Result spawned = Spawn(executable, arguments, child); !spawned) return spawned;

    std::string outBuffer;
    std::string errBuffer;

    while (true) {
        const bool outAlive = DrainPipe(child.stdoutRead.get(), outBuffer);
        const bool errAlive = DrainPipe(child.stderrRead.get(), errBuffer);
        EmitLines(outBuffer, onLine);
        if (errBuffer.size() > kMaxDiagnosticsBytes) errBuffer.resize(kMaxDiagnosticsBytes);

        if (shouldStop && shouldStop()) {
            // Section 7.2: cancellation is TerminateJobObject on the child's job.
            // There is no cooperative step to try first -- FFmpeg's is to type q
            // on a stdin we deliberately did not give it.
            ::TerminateJobObject(child.job.get(), 1);
            (void)::WaitForSingleObject(child.process.get(), 2000);
            into.cancelled = true;
            into.diagnostics = errBuffer;
            return Result::Cancelled();
        }

        const DWORD waited = ::WaitForSingleObject(child.process.get(), kPollIntervalMs);
        if (waited == WAIT_OBJECT_0) {
            // One last drain: the child exited, and what it wrote just before
            // exiting is exactly the part that explains why.
            (void)DrainPipe(child.stdoutRead.get(), outBuffer);
            (void)DrainPipe(child.stderrRead.get(), errBuffer);
            EmitLines(outBuffer, onLine);
            if (!outBuffer.empty() && onLine) onLine(outBuffer);
            break;
        }
        if (waited != WAIT_TIMEOUT) {
            return LastErrorAs(Status::kInternal, "Lost track of the FFmpeg process");
        }
        if (!outAlive && !errAlive) {
            // Both pipes are closed but the process has not exited. Nothing more
            // will be read, so just wait for it rather than spinning.
            (void)::WaitForSingleObject(child.process.get(), INFINITE);
            break;
        }
    }

    DWORD code = 0;
    ::GetExitCodeProcess(child.process.get(), &code);
    into.exitCode = code;
    if (errBuffer.size() > kMaxDiagnosticsBytes) errBuffer.resize(kMaxDiagnosticsBytes);
    into.diagnostics = errBuffer;
    return Result::Ok();
}

Result RunAndCapture(const std::wstring& executable, const std::vector<std::wstring>& arguments,
                     std::string& output, std::uint32_t timeoutMs) {
    Child child;
    if (const Result spawned = Spawn(executable, arguments, child); !spawned) return spawned;

    const DWORD deadline = ::GetTickCount() + timeoutMs;
    std::string errors;
    while (true) {
        (void)DrainPipe(child.stdoutRead.get(), output);
        (void)DrainPipe(child.stderrRead.get(), errors);

        const DWORD waited = ::WaitForSingleObject(child.process.get(), kPollIntervalMs);
        if (waited == WAIT_OBJECT_0) {
            (void)DrainPipe(child.stdoutRead.get(), output);
            (void)DrainPipe(child.stderrRead.get(), errors);
            break;
        }
        if (::GetTickCount() > deadline) {
            ::TerminateJobObject(child.job.get(), 1);
            return Result::Error(Status::kIoError, "FFmpeg did not answer in time.");
        }
    }

    // ffmpeg -version writes to stdout; some builds write parts of it to stderr.
    // Both are the answer.
    if (output.empty()) output = errors;
    return Result::Ok();
}

}  // namespace morph::ffmpeg
