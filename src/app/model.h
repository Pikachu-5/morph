// What the app IS, with no window around it (architecture section 12.2).
//
// The shell extension is built this way already and for the same reason: its
// menu logic is a static library that can be tested without Explorer, a
// surrogate or a registered class. The app's four screens -- drop zone, target
// picker, queue, done -- are a state machine over the broker's wire types, and
// keeping them here means the parts that can be wrong are testable in a console
// process, while the XAML is left with layout.
//
// IT DOES NO IO AT ALL. Not the pipe, not the disk. Probing is injected, the
// broker's answers arrive as arguments, and every method returns immediately.
// That is what lets the whole of section 12.2 be exercised without a broker, and
// it is also section 12.3's escape hatch surviving contact with the code: a C#
// rewrite of the shell reimplements this file's callers, not this file's rules.
//
// THREADING. One thread owns a model -- the UI thread. Broker events arrive on
// the pump thread inside BrokerClient::RunJob and the shell marshals each onto
// its dispatcher before calling in here. There is no lock below because a lock
// would imply the other discipline is optional.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "morph/core/fidelity.h"
#include "morph/core/ipc/protocol.h"
#include "morph/core/result.h"

namespace morph::app {

// Section 12.2's four screens, in the order a conversion walks them.
enum class Screen : std::uint8_t {
    kDropZone = 0,
    kTargetPicker = 1,
    kQueue = 2,
    kDone = 3,
};

// What the drop zone learned about one file.
//
// "On drop, probe the files (cheap, 8 KiB each) and move straight to target
//  selection." A file nothing recognises is kept rather than dropped on the
//  floor: the picker shows it greyed with its reason, because silently
//  converting four of the five files a user dragged in is the kind of thing they
//  find out about later.
struct InputFile {
    std::string path;
    std::string profile;  // empty when nothing recognised it
    std::uint64_t sizeBytes = 0;
    std::string error;  // why it cannot be converted; empty when it can

    [[nodiscard]] bool usable() const noexcept { return !profile.empty(); }
};

// One row of the target picker. "Targets grouped by family, each showing its
// fidelity badge and the hop count if the path is indirect."
struct TargetChoice {
    std::string profile;  // "image/png", what SubmitJob carries
    std::string label;    // "PNG", what the button says
    std::string family;   // "image", what the group header says
    Fidelity fidelity = Fidelity::Lossless;
    std::uint32_t hopCount = 0;
    double estimatedMs = 0.0;
    bool requiresDisk = false;

    // "Clicking a badge explains, in one sentence, what will be lost."
    [[nodiscard]] std::string_view Explanation() const noexcept { return Explain(fidelity); }

    // The picker shows the hop count only when the route is not direct, because
    // "1 hop" on every other button is noise that teaches nobody anything.
    [[nodiscard]] bool indirect() const noexcept { return hopCount > 1; }
};

enum class RowState : std::uint8_t {
    kPending = 0,
    kRunning = 1,
    kSucceeded = 2,
    kFailed = 3,
    // Section 11.3: the UI says "cancelled", not "failed". They are different
    // things and users are the ones who know it.
    kCancelled = 4,
};

// One row of the queue, and then of the done screen: the same row, further on.
struct QueueRow {
    std::string inputPath;
    std::string outputPath;  // known only once the task is done
    double fraction = 0.0;   // 0..1, hops already weighted by the executor
    RowState state = RowState::kPending;
    Fidelity fidelity = Fidelity::Lossless;
    // What the conversion ACTUALLY lost, as opposed to what the plan predicted.
    std::vector<FidelityNote> notes;
    std::string error;  // a real reason, never "conversion failed"

    // The user pressed Stop on this row. The broker decides when it actually
    // stops, so the row keeps its state until a TaskDone arrives; this only
    // stops the bar advancing into a number nobody believes.
    bool cancelling = false;

    [[nodiscard]] bool finished() const noexcept { return state >= RowState::kSucceeded; }
};

// The result of probing one file, which the model asks for and never performs.
//
// Injected rather than linked so that a model under test needs neither WIC nor
// Media Foundation nor PDFium in the process -- and so that WHERE probing
// happens stays an open decision. Doing it in the UI process costs an 8 KiB read
// per file; asking the worker costs a round trip to a process that already has
// every provider loaded. Both fit behind this.
struct ProbeAnswer {
    std::string profile;
    std::uint64_t sizeBytes = 0;
    std::string error;
};
using Prober = std::function<ProbeAnswer(const std::string& path)>;

class AppModel {
public:
    explicit AppModel(Prober prober);

    [[nodiscard]] Screen screen() const noexcept { return screen_; }
    [[nodiscard]] const std::vector<InputFile>& inputs() const noexcept { return inputs_; }
    [[nodiscard]] const std::vector<TargetChoice>& targets() const noexcept { return targets_; }
    [[nodiscard]] const std::vector<QueueRow>& rows() const noexcept { return rows_; }

    // The output policy the next job will carry. Section 10.2 defaults it to
    // "beside each input, renamed on collision", which is the only default that
    // never destroys anything.
    [[nodiscard]] ipc::OutputPolicy& output() noexcept { return output_; }
    [[nodiscard]] const ipc::OutputPolicy& output() const noexcept { return output_; }

    // --- The drop zone -------------------------------------------------------

    // "The entire window is a drop target. On drop, probe the files and move
    //  straight to target selection. No 'Add files' button as the primary
    //  affordance."
    //
    // Dropping again on the picker ADDS to the selection rather than replacing
    // it, because that is what dragging a second folder onto a window means
    // everywhere else. Returns the number of files that can actually be
    // converted, which is zero when the screen should stay where it is.
    std::size_t Drop(const std::vector<std::string>& paths);

    // The question to put to the broker, once, after a drop. Distinct source
    // profiles only: a folder of two hundred photographs is one question.
    // Empty when nothing usable was dropped.
    [[nodiscard]] ipc::QueryTargets TargetQuery() const;

    // The broker's answer. Sorted into the order the picker shows: by family,
    // then best fidelity first, then shortest route, then alphabetically -- so
    // the button a user wants is the first one under its heading.
    void SetTargets(const ipc::Targets& targets);

    // The broker could not be asked. The picker has a reason to show instead of
    // an empty list, which is the difference between "nothing to offer" and
    // "we never found out".
    void SetTargetsUnavailable(std::string reason);
    [[nodiscard]] const std::string& targetsError() const noexcept { return targetsError_; }

    // --- The target picker ---------------------------------------------------

    // The job for a chosen target. Only the usable inputs go in it; the files
    // nothing recognised stay visible on the done screen with their reason.
    // The profile must be one of targets(); anything else returns a job with no
    // inputs, which the caller must not submit.
    [[nodiscard]] ipc::SubmitJob BuildJob(std::string_view targetProfile) const;

    // Moves to the queue with a row per input of the job, all pending.
    //
    // Call this with the job that was actually submitted: rows 0..n-1 ARE the
    // broker's task indices, in order, and every event below finds its row by
    // that number. The files nothing recognised are appended after them, already
    // failed and carrying the probe's reason, so that a user who dragged in five
    // files and converted four is told which one and why -- on the same list,
    // not by counting.
    void Begin(const ipc::SubmitJob& job);

    // --- The queue -----------------------------------------------------------

    void OnAccepted(const ipc::JobAccepted& accepted);
    void OnProgress(const ipc::Progress& progress);
    void OnTaskDone(const ipc::TaskDone& done);
    void OnJobDone(const ipc::JobDone& done);

    // A broker-level failure. Everything still running failed for this reason,
    // and the job is over -- so the queue does not sit at 60% forever.
    void OnError(const ipc::Error& error);

    // The job id to cancel, or 0 before the broker has accepted one.
    [[nodiscard]] std::uint64_t jobId() const noexcept { return jobId_; }

    // "the resolved plan (the hop list, so the UI can show it)", source first.
    [[nodiscard]] const std::vector<std::string>& plan() const noexcept { return plan_; }

    // Marks a row as awaiting cancellation. The broker decides when it actually
    // stops, and says so with a TaskDone carrying kCancelled -- this only stops
    // the row's progress bar from advancing into a number nobody believes.
    void MarkCancelling(std::size_t row);
    [[nodiscard]] bool cancelling() const noexcept { return cancelling_; }
    void MarkCancellingAll();

    // "aggregate progress in the title bar and on the taskbar via
    //  ITaskbarList3::SetProgressValue". Finished rows count as whole ones, so
    //  the number never goes backwards when a task completes.
    [[nodiscard]] double aggregateFraction() const noexcept;

    // --- The done screen -----------------------------------------------------

    [[nodiscard]] std::uint32_t succeeded() const noexcept { return succeeded_; }
    [[nodiscard]] std::uint32_t failed() const noexcept { return failed_; }
    [[nodiscard]] std::uint32_t cancelled() const noexcept { return cancelled_; }

    // The directory to open when the user asks. The first output actually
    // written; empty when nothing was.
    [[nodiscard]] std::string outputDirectory() const;

    // "Convert more." Back to an empty drop zone, keeping the output policy the
    // user chose -- they set it for a reason and having to set it again after
    // every batch is the kind of thing that makes a tool annoying.
    void ConvertMore();

private:
    void Recount() noexcept;

    Prober prober_;

    Screen screen_ = Screen::kDropZone;
    std::vector<InputFile> inputs_;
    std::vector<TargetChoice> targets_;
    std::string targetsError_;
    std::vector<QueueRow> rows_;
    std::vector<std::string> plan_;
    ipc::OutputPolicy output_;

    std::uint64_t jobId_ = 0;
    bool cancelling_ = false;

    std::uint32_t succeeded_ = 0;
    std::uint32_t failed_ = 0;
    std::uint32_t cancelled_ = 0;
};

}  // namespace morph::app
