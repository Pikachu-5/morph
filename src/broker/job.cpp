#include "job.h"

namespace morph::broker {
namespace {

// Section 11.4: "the broker coalesces per job to 10 Hz before fan-out".
constexpr auto kProgressInterval = std::chrono::milliseconds(100);

}  // namespace

std::string_view Name(TaskClass value) noexcept {
    switch (value) {
        case TaskClass::kImage: return "image";
        case TaskClass::kDocument: return "document";
        case TaskClass::kAudioVideo: return "audio/video";
        case TaskClass::kPdfRender: return "pdf render";
        case TaskClass::kCount: break;
    }
    return "unknown";
}

TaskClass ClassOf(const FormatGraph& graph, const Plan& plan) {
    bool sawImage = false;
    bool sawAudioVideo = false;

    for (const PlanHop& hop : plan.hops) {
        const MediaProfile& to = graph.Node(hop.to);

        // PDF output goes through the one WebView2 environment (section 8.6),
        // and section 11.1 gives that a concurrency of exactly 1. It outranks
        // everything else on the route because it is the binding constraint.
        if (to.Id() == "application/pdf") return TaskClass::kPdfRender;

        const std::string_view family = to.Family();
        if (family == "image") sawImage = true;
        if (family == "video" || family == "audio") sawAudioVideo = true;
    }

    // Audio/video before image: FFmpeg saturates the machine on its own, so a
    // route that touches it must be scheduled as if that is all it does.
    if (sawAudioVideo) return TaskClass::kAudioVideo;
    if (sawImage) return TaskClass::kImage;
    return TaskClass::kDocument;
}

bool ClientLink::SendProgress(const ipc::Progress& message,
                              std::chrono::steady_clock::time_point now) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return false;
    if (now - lastProgress_ < kProgressInterval) return false;
    lastProgress_ = now;

    if (const Result sent = connection_.Send(message); !sent) {
        open_ = false;
        return false;
    }
    return true;
}

std::optional<ipc::FrameReader::Frame> ClientLink::Receive(Result& failed) {
    return connection_.Receive(failed);
}

void ClientLink::Detach() noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    open_ = false;
    connection_.Close();
}

bool ClientLink::Open() const noexcept {
    // const_cast rather than a mutable mutex: the lock is the whole point of the
    // accessor and hiding it in the member list would be worse.
    const std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
    return open_;
}

bool Job::Finish(Status status) {
    switch (status) {
        case Status::kOk: succeeded_.fetch_add(1); break;
        case Status::kCancelled: cancelled_.fetch_add(1); break;
        default: failed_.fetch_add(1); break;
    }
    const std::uint32_t done = finished_.fetch_add(1) + 1;
    return done == tasks.size();
}

void ReportTask(const std::shared_ptr<Job>& job, const ipc::TaskDone& done) {
    // The accounting happens whether or not anyone is listening. A client that
    // disconnected mid-batch still leaves a job that has to reach its last task
    // and stop being work the broker is waiting on.
    const bool last = job->Finish(static_cast<Status>(done.status));
    if (!job->client) return;
    job->client->Send(done);
    if (last) job->client->Send(job->Summary());
}

ipc::JobDone Job::Summary() const {
    ipc::JobDone done;
    done.jobId = id;
    done.succeeded = succeeded_.load();
    done.failed = failed_.load();
    done.cancelled = cancelled_.load();
    return done;
}

}  // namespace morph::broker
