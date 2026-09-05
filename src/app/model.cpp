#include "app/model.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include "morph/core/extensions.h"
#include "morph/core/media_profile.h"

namespace morph::app {
namespace {

std::string Upper(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

// "PNG" for image/png, "PDF" for application/pdf.
//
// The extension table is asked first because it is the name the OUTPUT will
// carry, and a button that says JPEG next to a file that arrives as .jpg is a
// small lie with a support question attached. A profile the table does not know
// still gets a label rather than being dropped: the broker offered it, so
// something can write it, and hiding it would be worse than an imperfect name.
std::string LabelFor(const std::string& profileId) {
    if (const std::string_view extension = ExtensionFor(profileId); !extension.empty()) {
        return Upper(extension);
    }
    const std::size_t slash = profileId.find('/');
    const std::string_view tail =
        slash == std::string::npos ? std::string_view(profileId)
                                   : std::string_view(profileId).substr(slash + 1);
    // Attributes are not part of a name a person reads: "video/mp4;codec=h264"
    // is an MP4 on the button and the codec belongs in the details.
    const std::size_t semicolon = tail.find(';');
    return Upper(semicolon == std::string_view::npos ? tail : tail.substr(0, semicolon));
}

std::string FamilyOf(const std::string& profileId) {
    if (const std::optional<MediaProfile> profile = MediaProfile::Parse(profileId)) {
        return std::string(profile->Family());
    }
    const std::size_t slash = profileId.find('/');
    return slash == std::string::npos ? std::string{} : profileId.substr(0, slash);
}

RowState StateFor(std::uint8_t wireStatus) noexcept {
    switch (static_cast<Status>(wireStatus)) {
        case Status::kOk:        return RowState::kSucceeded;
        case Status::kCancelled: return RowState::kCancelled;
        default:                 return RowState::kFailed;
    }
}

}  // namespace

AppModel::AppModel(Prober prober) : prober_(std::move(prober)) {}

std::size_t AppModel::Drop(const std::vector<std::string>& paths) {
    // A drop lands on the drop zone or on the picker. Anywhere else -- mid-job,
    // or on the done screen -- it starts a new selection, because a queue that
    // grows while it runs is a different feature and not this one.
    if (screen_ == Screen::kQueue || screen_ == Screen::kDone) ConvertMore();

    for (const std::string& path : paths) {
        if (path.empty()) continue;
        // Dragging the same folder twice is a thing people do.
        const auto already = std::find_if(inputs_.begin(), inputs_.end(),
                                          [&](const InputFile& in) { return in.path == path; });
        if (already != inputs_.end()) continue;

        InputFile file;
        file.path = path;
        if (prober_) {
            ProbeAnswer answer = prober_(path);
            file.profile = std::move(answer.profile);
            file.sizeBytes = answer.sizeBytes;
            file.error = std::move(answer.error);
        }
        if (file.profile.empty() && file.error.empty()) {
            file.error = "Morph does not recognise this kind of file.";
        }
        inputs_.push_back(std::move(file));
    }

    // The answer on screen was about a different set of files. Keeping it while
    // the new ones are still being asked about is how a picker ends up offering
    // a target half the selection cannot reach.
    targets_.clear();
    targetsError_.clear();

    const auto usable = static_cast<std::size_t>(
        std::count_if(inputs_.begin(), inputs_.end(),
                      [](const InputFile& in) { return in.usable(); }));

    // "move straight to target selection" -- but only when there is a selection
    // to make. Five unreadable files leave the user on the drop zone with five
    // reasons, rather than on a picker with nothing on it.
    if (usable > 0) screen_ = Screen::kTargetPicker;
    return usable;
}

ipc::QueryTargets AppModel::TargetQuery() const {
    ipc::QueryTargets query;
    for (const InputFile& file : inputs_) {
        if (!file.usable()) continue;
        if (std::find(query.sourceProfiles.begin(), query.sourceProfiles.end(), file.profile) ==
            query.sourceProfiles.end()) {
            query.sourceProfiles.push_back(file.profile);
        }
        // "Bytes of the largest input, so the estimate comes back scaled."
        query.largestInputBytes = std::max(query.largestInputBytes, file.sizeBytes);
    }
    return query;
}

void AppModel::SetTargets(const ipc::Targets& targets) {
    targetsError_.clear();
    targets_.clear();
    targets_.reserve(targets.targets.size());

    for (const ipc::TargetOption& option : targets.targets) {
        TargetChoice choice;
        choice.profile = option.profile;
        choice.label = LabelFor(option.profile);
        choice.family = FamilyOf(option.profile);
        choice.fidelity = static_cast<Fidelity>(option.fidelity);
        choice.hopCount = option.hopCount;
        choice.estimatedMs = option.estimatedMs;
        choice.requiresDisk = option.requiresDisk;
        targets_.push_back(std::move(choice));
    }

    // Grouped by family, best first inside each group. The picker walks this
    // list once and starts a heading wherever the family changes, so the sort IS
    // the grouping -- there is no second structure to keep in step with it.
    std::sort(targets_.begin(), targets_.end(),
              [](const TargetChoice& a, const TargetChoice& b) {
                  if (a.family != b.family) return a.family < b.family;
                  if (a.fidelity != b.fidelity) return a.fidelity < b.fidelity;
                  if (a.hopCount != b.hopCount) return a.hopCount < b.hopCount;
                  return a.label < b.label;
              });
}

void AppModel::SetTargetsUnavailable(std::string reason) {
    targets_.clear();
    targetsError_ = std::move(reason);
}

ipc::SubmitJob AppModel::BuildJob(std::string_view targetProfile) const {
    ipc::SubmitJob job;

    // A target that is not on the picker is not a target. Returning an empty job
    // rather than submitting one is the difference between a UI bug and a
    // conversion the user never asked for.
    const bool offered = std::any_of(targets_.begin(), targets_.end(),
                                     [&](const TargetChoice& choice) {
                                         return choice.profile == targetProfile;
                                     });
    if (!offered) return job;

    job.targetProfile = std::string(targetProfile);
    job.output = output_;
    // A person is watching this one: section 11.2 gives it the interactive CPU
    // weight rather than the background one.
    job.interactive = true;

    for (const InputFile& file : inputs_) {
        if (file.usable()) job.inputPaths.push_back(file.path);
    }
    return job;
}

void AppModel::Begin(const ipc::SubmitJob& job) {
    rows_.clear();
    plan_.clear();
    jobId_ = 0;
    cancelling_ = false;

    rows_.reserve(job.inputPaths.size() + inputs_.size());
    for (const std::string& path : job.inputPaths) {
        QueueRow row;
        row.inputPath = path;
        rows_.push_back(std::move(row));
    }

    // Appended AFTER the job's own inputs, so nothing above shifts: rows_[i] for
    // i < inputPaths.size() is the broker's task i, and these are extra.
    for (const InputFile& file : inputs_) {
        if (file.usable()) continue;
        QueueRow row;
        row.inputPath = file.path;
        row.state = RowState::kFailed;
        row.fraction = 1.0;
        row.error = file.error;
        rows_.push_back(std::move(row));
    }

    screen_ = Screen::kQueue;
    Recount();
}

void AppModel::OnAccepted(const ipc::JobAccepted& accepted) {
    jobId_ = accepted.jobId;
    plan_ = accepted.planProfiles;
}

void AppModel::OnProgress(const ipc::Progress& progress) {
    if (progress.taskIndex >= rows_.size()) return;
    QueueRow& row = rows_[progress.taskIndex];
    if (row.finished() || row.cancelling) return;

    row.state = RowState::kRunning;
    // Clamped and monotonic. A broker that reports a hop boundary out of order
    // would otherwise walk a progress bar backwards, which readers of a progress
    // bar treat as a bug in the whole program.
    row.fraction = std::clamp(std::max(row.fraction, progress.fraction), 0.0, 1.0);
}

void AppModel::OnTaskDone(const ipc::TaskDone& done) {
    if (done.taskIndex >= rows_.size()) return;
    QueueRow& row = rows_[done.taskIndex];

    row.state = StateFor(done.status);
    row.fraction = 1.0;
    row.outputPath = done.outputPath;
    row.fidelity = static_cast<Fidelity>(done.fidelity);
    row.error = done.error;
    row.notes.clear();
    row.notes.reserve(done.notes.size());
    for (const ipc::NoteWire& note : done.notes) {
        row.notes.push_back(FidelityNote{note.code, note.detail, note.location});
    }

    // Section 12.2: "Failures listed with a real reason, never 'conversion
    // failed'." A broker that fails a task and says nothing is a bug, but the
    // user is not the right person to pay for it.
    if (row.state == RowState::kFailed && row.error.empty()) {
        row.error = "The conversion stopped without reporting a reason.";
    }
    Recount();
}

void AppModel::OnJobDone(const ipc::JobDone&) {
    // Any row the broker never reported on. This should not happen; if it does,
    // a row stuck at "running" on the done screen is worse than one that says so.
    for (QueueRow& row : rows_) {
        if (row.finished()) continue;
        row.state = RowState::kFailed;
        row.fraction = 1.0;
        row.error = "The job finished without reporting what happened to this file.";
    }

    // The counts come from the rows rather than from JobDone's own totals, which
    // carry the same numbers. The done screen shows the summary directly above
    // the list, and a summary that disagrees with the list below it is worse
    // than either of them being wrong alone.
    Recount();
    screen_ = Screen::kDone;
}

void AppModel::OnError(const ipc::Error& error) {
    const bool wasCancelled = static_cast<Status>(error.status) == Status::kCancelled;
    for (QueueRow& row : rows_) {
        if (row.finished()) continue;
        row.state = wasCancelled ? RowState::kCancelled : RowState::kFailed;
        row.fraction = 1.0;
        if (!wasCancelled) {
            row.error = error.message.empty()
                            ? "The conversion stopped without reporting a reason."
                            : error.message;
        }
    }
    Recount();
    screen_ = Screen::kDone;
}

void AppModel::MarkCancelling(std::size_t row) {
    if (row >= rows_.size()) return;
    if (rows_[row].finished()) return;
    rows_[row].cancelling = true;
}

void AppModel::MarkCancellingAll() {
    cancelling_ = true;
    for (QueueRow& row : rows_) {
        if (!row.finished()) row.cancelling = true;
    }
}

double AppModel::aggregateFraction() const noexcept {
    if (rows_.empty()) return 0.0;
    double total = 0.0;
    for (const QueueRow& row : rows_) total += row.finished() ? 1.0 : row.fraction;
    return total / static_cast<double>(rows_.size());
}

std::string AppModel::outputDirectory() const {
    for (const QueueRow& row : rows_) {
        if (row.state != RowState::kSucceeded || row.outputPath.empty()) continue;
        std::error_code ec;
        const std::filesystem::path parent =
            std::filesystem::path(row.outputPath).parent_path();
        if (parent.empty()) continue;
        // The path came back over a pipe and names a file the broker says it
        // wrote. Asking the filesystem rather than trusting the string is what
        // keeps "Open folder" from opening nothing.
        if (!std::filesystem::exists(parent, ec)) continue;
        return parent.string();
    }
    return {};
}

void AppModel::ConvertMore() {
    screen_ = Screen::kDropZone;
    inputs_.clear();
    targets_.clear();
    targetsError_.clear();
    rows_.clear();
    plan_.clear();
    jobId_ = 0;
    cancelling_ = false;
    succeeded_ = 0;
    failed_ = 0;
    cancelled_ = 0;
    // output_ deliberately survives: a user who chose a destination folder chose
    // it for a reason, and asking again after every batch is what makes a tool
    // tiring to use.
}

void AppModel::Recount() noexcept {
    succeeded_ = 0;
    failed_ = 0;
    cancelled_ = 0;
    for (const QueueRow& row : rows_) {
        switch (row.state) {
            case RowState::kSucceeded: ++succeeded_; break;
            case RowState::kFailed:    ++failed_; break;
            case RowState::kCancelled: ++cancelled_; break;
            default: break;
        }
    }
}

}  // namespace morph::app
