#include "worker_channel.h"

#include <atomic>
#include <filesystem>

#include <windows.h>

namespace morph::broker {
namespace {

// Section 11.2: "ActiveProcessLimit = 4 // worker + ffmpeg + slack".
constexpr DWORD kActiveProcessLimit = 4;

// Distinct pipe names per worker. A counter rather than a GUID because the name
// only has to be unique within this broker, and a readable name is worth
// something when a handle shows up in Process Explorer.
std::atomic<std::uint64_t> gWorkerSerial{0};

Result LastErrorAs(Status status, std::string_view what) {
    const DWORD code = ::GetLastError();
    return Result::Error(status,
                         std::string(what) + " (Windows error " + std::to_string(code) + ").");
}

}  // namespace

std::wstring WorkerExecutablePath() {
    wchar_t self[MAX_PATH]{};
    const DWORD length = ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (length == 0 || length == MAX_PATH) return L"morph.worker.exe";

    std::filesystem::path path(self);
    path.replace_filename(L"morph.worker.exe");
    return path.wstring();
}

WorkerChannel::~WorkerChannel() { Close(); }

bool WorkerChannel::Running() const noexcept {
    if (process_ == nullptr) return false;
    return ::WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT;
}

Result WorkerChannel::CreateContainer(const WorkerLimits& limits) {
    const HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) return LastErrorAs(Status::kInternal, "Could not create a job object");
    job_ = job;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION extended{};
    extended.BasicLimitInformation.LimitFlags =
        // Orphan cleanup, free: when the broker dies, every worker dies with it.
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    extended.BasicLimitInformation.ActiveProcessLimit = kActiveProcessLimit;
    if (limits.memoryBytes > 0) {
        extended.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
        extended.JobMemoryLimit = static_cast<SIZE_T>(limits.memoryBytes);
    }
    if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &extended,
                                   sizeof(extended))) {
        return LastErrorAs(Status::kInternal, "Could not apply the worker's job limits");
    }

    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION rate{};
    rate.ControlFlags =
        JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_WEIGHT_BASED;
    rate.Weight = limits.interactive ? 3 : 9;
    // Best effort: CPU rate control is unavailable in a container or under some
    // policies, and a worker that runs at the default weight is worse than one
    // that does not run at all.
    ::SetInformationJobObject(job, JobObjectCpuRateControlInformation, &rate, sizeof(rate));

    return Result::Ok();
}

Result WorkerChannel::Spawn(const std::wstring& pipeName) {
    std::wstring command = L"\"" + WorkerExecutablePath() + L"\" --serve \"" + pipeName + L"\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};

    // CREATE_SUSPENDED so the assignment to the job object happens before the
    // process runs a single instruction. CREATE_NO_WINDOW because the worker has
    // a console front end and would otherwise flash one on every conversion.
    const BOOL created =
        ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                         CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
    if (!created) return LastErrorAs(Status::kInternal, "Could not start morph.worker.exe");

    if (!::AssignProcessToJobObject(static_cast<HANDLE>(job_), info.hProcess)) {
        // Refuse to run an uncontained worker. Section 11.2 calls the job object
        // "the backstop for everything", and a backstop that is sometimes absent
        // is not one.
        ::TerminateProcess(info.hProcess, 1);
        ::CloseHandle(info.hThread);
        ::CloseHandle(info.hProcess);
        return LastErrorAs(Status::kInternal, "Could not contain the worker in its job object");
    }

    ::ResumeThread(info.hThread);
    ::CloseHandle(info.hThread);

    process_ = info.hProcess;
    processId_ = info.dwProcessId;
    return Result::Ok();
}

Result WorkerChannel::Start(std::wstring_view suffix, const WorkerLimits& limits) {
    const std::uint64_t serial = gWorkerSerial.fetch_add(1) + 1;
    const std::wstring name =
        ipc::PipeNameFor(std::wstring(suffix) + L"." + std::to_wstring(::GetCurrentProcessId()) +
                         L"." + std::to_wstring(serial));

    if (const Result created = server_.Create(name); !created) return created;
    if (const Result job = CreateContainer(limits); !job) return job;
    if (const Result spawned = Spawn(name); !spawned) return spawned;

    // The worker connects back immediately. Ten seconds is far more than it
    // needs and still bounded, because a worker that never connects must not
    // wedge the scheduler slot waiting for it.
    if (const Result accepted = server_.Accept(connection_, 10000); !accepted) {
        Terminate();
        return accepted.cancelled()
                   ? Result::Error(Status::kInternal, "The worker did not connect back.")
                   : accepted;
    }
    // Nothing else may connect to this worker's name once its worker has.
    server_.Close();

    ipc::WorkerHello hello;
    if (const Result received = connection_.ReceiveAs(hello); !received) {
        Terminate();
        return received;
    }
    if (hello.protocolVersion != ipc::kProtocolVersion) {
        Terminate();
        return Result::Error(Status::kInternal,
                             "The worker speaks protocol version " +
                                 std::to_string(hello.protocolVersion) + " and this broker " +
                                 std::to_string(ipc::kProtocolVersion) +
                                 ". The install is half upgraded.");
    }
    return Result::Ok();
}

Result WorkerChannel::Describe(ipc::GraphDescription& into) {
    if (const Result sent = connection_.Send(ipc::DescribeGraph{}); !sent) return sent;
    return connection_.ReceiveAs(into);
}

Result WorkerChannel::Probe(const std::vector<std::string>& paths, ipc::ProbeResult& into) {
    ipc::ProbeFiles request;
    request.paths = paths;
    if (const Result sent = connection_.Send(request); !sent) return sent;
    if (const Result received = connection_.ReceiveAs(into); !received) return received;

    if (into.profiles.size() != paths.size()) {
        return Result::Error(Status::kInternal,
                             "The worker answered " + std::to_string(into.profiles.size()) +
                                 " of " + std::to_string(paths.size()) + " probes.");
    }
    return Result::Ok();
}

Result WorkerChannel::Run(const ipc::RunTask& task,
                          const std::function<void(const ipc::Progress&)>& onProgress,
                          ipc::TaskDone& into) {
    if (const Result sent = connection_.Send(task); !sent) return sent;

    while (true) {
        Result failed = Result::Ok();
        const std::optional<ipc::FrameReader::Frame> frame = connection_.Receive(failed);
        if (!frame) {
            if (!failed.ok()) return failed;
            // A worker that closed the pipe mid-task died -- terminated for
            // exceeding its memory limit, or crashed on hostile input, which
            // section 3.1 treats as an expected outcome rather than an incident.
            return Result::Error(Status::kInternal,
                                 "The worker stopped before finishing this file.");
        }

        if (frame->type == ipc::MessageType::kProgress) {
            ipc::Progress progress;
            if (Decode(frame->payload, progress) && onProgress) onProgress(progress);
            continue;
        }
        if (frame->type == ipc::MessageType::kTaskDone) {
            if (!Decode(frame->payload, into)) {
                return Result::Error(Status::kInternal,
                                     "The worker's result could not be decoded.");
            }
            return Result::Ok();
        }
        return Result::Error(Status::kInternal, "The worker sent an unexpected " +
                                                    std::string(Name(frame->type)) + ".");
    }
}

void WorkerChannel::Cancel(std::uint64_t jobId, std::int64_t taskIndex) {
    ipc::Cancel cancel;
    cancel.jobId = jobId;
    cancel.taskIndex = taskIndex;
    // Sent from a different thread than the one in Run, which is reading. One
    // reader and one writer on a duplex pipe is exactly what it is for.
    (void)connection_.SendFrame(ipc::MessageType::kCancel, Encode(cancel));
}

void WorkerChannel::Terminate() noexcept {
    if (job_ != nullptr) ::TerminateJobObject(static_cast<HANDLE>(job_), 1);
}

void WorkerChannel::Close() noexcept {
    connection_.Close();
    server_.Close();

    if (process_ != nullptr) {
        // The worker exits on its own when the pipe closes. Give it a moment to
        // do that tidily before the job object takes it: a worker killed
        // mid-write leaves its temp directory behind.
        ::WaitForSingleObject(static_cast<HANDLE>(process_), 2000);
        ::CloseHandle(static_cast<HANDLE>(process_));
        process_ = nullptr;
    }
    if (job_ != nullptr) {
        // KILL_ON_JOB_CLOSE: closing the last handle to the job kills whatever
        // is still inside it, including a worker that ignored the closed pipe.
        ::CloseHandle(static_cast<HANDLE>(job_));
        job_ = nullptr;
    }
    processId_ = 0;
}

}  // namespace morph::broker
