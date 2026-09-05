// The app's four screens (architecture section 12.2).
//
// Every case below runs with no window, no broker and no provider in the
// process, which is the whole argument for the model being a library: the rules
// that can be wrong -- what a mixed drop offers, which files reach the job, what
// the done screen says about the one that failed -- are decided here and tested
// here, and the XAML is left with layout.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "app/model.h"
#include "morph/core/naming.h"

using namespace morph;
using namespace morph::app;

namespace {

// A prober that answers from a table rather than from the disk. Section 12.2
// says the real one reads 8 KiB; what the model does with the answer is the
// question here.
Prober TableProber() {
    return [](const std::string& path) {
        ProbeAnswer answer;
        answer.sizeBytes = 1024;
        if (path.ends_with(".md")) {
            answer.profile = "text/markdown";
        } else if (path.ends_with(".png")) {
            answer.profile = "image/png";
            answer.sizeBytes = 4096;
        } else if (path.ends_with(".docx")) {
            answer.profile = "application/vnd.openxmlformats-officedocument.wordprocessingml."
                             "document";
        } else {
            answer.sizeBytes = 0;
            answer.error = "Morph cannot read a .xyz file.";
        }
        return answer;
    };
}

ipc::Targets TargetList(std::vector<ipc::TargetOption> options) {
    ipc::Targets targets;
    targets.targets = std::move(options);
    return targets;
}

ipc::TargetOption Option(std::string profile, Fidelity fidelity, std::uint32_t hops) {
    ipc::TargetOption option;
    option.profile = std::move(profile);
    option.fidelity = static_cast<std::uint8_t>(fidelity);
    option.hopCount = hops;
    return option;
}

ipc::TaskDone Finished(std::uint32_t index, Status status, std::string output,
                       std::string error = {}) {
    ipc::TaskDone done;
    done.jobId = 1;
    done.taskIndex = index;
    done.status = static_cast<std::uint8_t>(status);
    done.outputPath = std::move(output);
    done.error = std::move(error);
    return done;
}

}  // namespace

// --- The drop zone -----------------------------------------------------------

TEST_CASE("a drop probes every file and moves to the picker", "[app][model]") {
    AppModel model{TableProber()};
    CHECK(model.screen() == Screen::kDropZone);

    const std::size_t usable = model.Drop({R"(C:\in\notes.md)", R"(C:\in\shot.png)"});

    CHECK(usable == 2);
    CHECK(model.screen() == Screen::kTargetPicker);
    REQUIRE(model.inputs().size() == 2);
    CHECK(model.inputs()[0].profile == "text/markdown");
    CHECK(model.inputs()[1].sizeBytes == 4096);
}

TEST_CASE("a file nothing recognises keeps its reason and does not move the screen",
          "[app][model]") {
    AppModel model{TableProber()};
    const std::size_t usable = model.Drop({R"(C:\in\thing.xyz)"});

    CHECK(usable == 0);
    // Five unreadable files leave the user looking at five reasons, not at an
    // empty picker.
    CHECK(model.screen() == Screen::kDropZone);
    REQUIRE(model.inputs().size() == 1);
    CHECK_FALSE(model.inputs()[0].usable());
    CHECK(model.inputs()[0].error == "Morph cannot read a .xyz file.");
}

TEST_CASE("the same file dropped twice is one file", "[app][model]") {
    AppModel model{TableProber()};
    model.Drop({R"(C:\in\notes.md)"});
    model.Drop({R"(C:\in\notes.md)", R"(C:\in\shot.png)"});

    REQUIRE(model.inputs().size() == 2);
}

TEST_CASE("a second drop invalidates the targets the first one asked about", "[app][model]") {
    AppModel model{TableProber()};
    model.Drop({R"(C:\in\notes.md)"});
    model.SetTargets(TargetList({Option("application/pdf", Fidelity::High, 2)}));
    REQUIRE(model.targets().size() == 1);

    // The answer on screen was about a different selection. Offering a target
    // half the files cannot reach is exactly what section 12.2 forbids.
    model.Drop({R"(C:\in\shot.png)"});
    CHECK(model.targets().empty());
}

TEST_CASE("the query names each source profile once and the largest input", "[app][model]") {
    AppModel model{TableProber()};
    model.Drop({R"(C:\a.md)", R"(C:\b.md)", R"(C:\c.png)", R"(C:\d.xyz)"});

    const ipc::QueryTargets query = model.TargetQuery();
    // A folder of two hundred photographs is one question.
    REQUIRE(query.sourceProfiles.size() == 2);
    CHECK(query.largestInputBytes == 4096);
}

// --- The target picker -------------------------------------------------------

TEST_CASE("targets are grouped by family with the best first", "[app][model]") {
    AppModel model{TableProber()};
    model.Drop({R"(C:\in\notes.md)"});
    model.SetTargets(TargetList({
        Option("text/html", Fidelity::High, 1),
        Option("image/png", Fidelity::Rasterized, 3),
        Option("application/pdf", Fidelity::Structural, 2),
        Option("image/jpeg", Fidelity::Rasterized, 3),
    }));

    REQUIRE(model.targets().size() == 4);
    // The sort IS the grouping: the picker walks the list once and starts a
    // heading wherever the family changes.
    CHECK(model.targets()[0].family == "application");
    CHECK(model.targets()[1].family == "image");
    CHECK(model.targets()[2].family == "image");
    CHECK(model.targets()[3].family == "text");
    // JPG, not JPEG: the label is the extension the output will actually
    // carry, and a button promising one while the file arrives as the other is
    // a small lie with a support question attached.
    CHECK(model.targets()[1].label == "JPG");
    CHECK(model.targets()[2].label == "PNG");
}

TEST_CASE("a badge explains itself and only an indirect route shows its hops", "[app][model]") {
    AppModel model{TableProber()};
    model.Drop({R"(C:\in\notes.md)"});
    model.SetTargets(TargetList({
        Option("text/html", Fidelity::High, 1),
        Option("application/pdf", Fidelity::Structural, 3),
    }));

    const TargetChoice& html = model.targets()[1];
    CHECK(html.fidelity == Fidelity::High);
    CHECK_FALSE(html.indirect());
    CHECK(html.Explanation() == Explain(Fidelity::High));

    CHECK(model.targets()[0].indirect());
}

TEST_CASE("a target the picker never offered builds no job", "[app][model]") {
    AppModel model{TableProber()};
    model.Drop({R"(C:\in\notes.md)"});
    model.SetTargets(TargetList({Option("application/pdf", Fidelity::Structural, 2)}));

    const ipc::SubmitJob job = model.BuildJob("video/mp4");
    CHECK(job.inputPaths.empty());
    CHECK(job.targetProfile.empty());
}

TEST_CASE("only the files that can be converted reach the job", "[app][model]") {
    AppModel model{TableProber()};
    model.Drop({R"(C:\a.md)", R"(C:\b.xyz)", R"(C:\c.png)"});
    model.SetTargets(TargetList({Option("application/pdf", Fidelity::Structural, 2)}));

    const ipc::SubmitJob job = model.BuildJob("application/pdf");
    REQUIRE(job.inputPaths.size() == 2);
    CHECK(job.inputPaths[0] == R"(C:\a.md)");
    CHECK(job.inputPaths[1] == R"(C:\c.png)");
    CHECK(job.interactive);
    CHECK(job.output.overwrite == ipc::OverwriteRule::kRename);
}

TEST_CASE("the broker being unreachable is a reason, not an empty picker", "[app][model]") {
    AppModel model{TableProber()};
    model.Drop({R"(C:\in\notes.md)"});
    model.SetTargetsUnavailable("The Morph broker did not start.");

    CHECK(model.targets().empty());
    CHECK(model.targetsError() == "The Morph broker did not start.");
}

// --- The queue ---------------------------------------------------------------

TEST_CASE("the queue's rows are the broker's task indices", "[app][model]") {
    AppModel model{TableProber()};
    model.Drop({R"(C:\a.md)", R"(C:\b.xyz)", R"(C:\c.png)"});
    model.SetTargets(TargetList({Option("application/pdf", Fidelity::Structural, 2)}));

    const ipc::SubmitJob job = model.BuildJob("application/pdf");
    model.Begin(job);

    CHECK(model.screen() == Screen::kQueue);
    // Two convertible files first, in the job's own order, then the one that
    // never got as far as the broker.
    REQUIRE(model.rows().size() == 3);
    CHECK(model.rows()[0].inputPath == R"(C:\a.md)");
    CHECK(model.rows()[1].inputPath == R"(C:\c.png)");
    CHECK(model.rows()[2].inputPath == R"(C:\b.xyz)");
    CHECK(model.rows()[2].state == RowState::kFailed);
    CHECK(model.rows()[2].error == "Morph cannot read a .xyz file.");
    CHECK(model.failed() == 1);
}

TEST_CASE("progress never walks backwards", "[app][model]") {
    AppModel model{TableProber()};
    model.Drop({R"(C:\a.md)"});
    model.SetTargets(TargetList({Option("application/pdf", Fidelity::Structural, 2)}));
    model.Begin(model.BuildJob("application/pdf"));

    model.OnProgress(ipc::Progress{1, 0, 0.6, 1, 0});
    CHECK(model.rows()[0].state == RowState::kRunning);
    // A hop boundary reported out of order would otherwise send the bar back,
    // which readers of a progress bar treat as a bug in the whole program.
    model.OnProgress(ipc::Progress{1, 0, 0.2, 0, 0});
    CHECK(model.rows()[0].fraction == 0.6);
}

TEST_CASE("aggregate progress counts a finished row as a whole one", "[app][model]") {
    AppModel model{TableProber()};
    model.Drop({R"(C:\a.md)", R"(C:\c.png)"});
    model.SetTargets(TargetList({Option("application/pdf", Fidelity::Structural, 2)}));
    model.Begin(model.BuildJob("application/pdf"));

    CHECK(model.aggregateFraction() == 0.0);
    model.OnTaskDone(Finished(0, Status::kOk, R"(C:\a.pdf)"));
    CHECK(model.aggregateFraction() == 0.5);
    model.OnProgress(ipc::Progress{1, 1, 0.5, 0, 0});
    CHECK(model.aggregateFraction() == 0.75);
}

TEST_CASE("a cancelled row stops advancing but keeps its state until the broker says",
          "[app][model]") {
    AppModel model{TableProber()};
    model.Drop({R"(C:\a.md)"});
    model.SetTargets(TargetList({Option("application/pdf", Fidelity::Structural, 2)}));
    model.Begin(model.BuildJob("application/pdf"));

    model.OnProgress(ipc::Progress{1, 0, 0.3, 0, 0});
    model.MarkCancelling(0);
    model.OnProgress(ipc::Progress{1, 0, 0.9, 1, 0});

    CHECK(model.rows()[0].cancelling);
    CHECK(model.rows()[0].fraction == 0.3);
    CHECK(model.rows()[0].state == RowState::kRunning);

    // The broker decides when it actually stopped, and section 11.3 makes that
    // "cancelled" rather than "failed".
    model.OnTaskDone(Finished(0, Status::kCancelled, {}));
    CHECK(model.rows()[0].state == RowState::kCancelled);
    CHECK(model.cancelled() == 1);
    CHECK(model.failed() == 0);
}

TEST_CASE("a task that fails without a reason is given one", "[app][model]") {
    AppModel model{TableProber()};
    model.Drop({R"(C:\a.md)"});
    model.SetTargets(TargetList({Option("application/pdf", Fidelity::Structural, 2)}));
    model.Begin(model.BuildJob("application/pdf"));

    model.OnTaskDone(Finished(0, Status::kIoError, {}));
    // Section 12.2 forbids "conversion failed". A broker that says nothing is a
    // bug, but the user is not the right person to pay for it.
    CHECK_FALSE(model.rows()[0].error.empty());
}

TEST_CASE("a broker failure ends the queue instead of leaving it at sixty percent",
          "[app][model]") {
    AppModel model{TableProber()};
    model.Drop({R"(C:\a.md)", R"(C:\c.png)"});
    model.SetTargets(TargetList({Option("application/pdf", Fidelity::Structural, 2)}));
    model.Begin(model.BuildJob("application/pdf"));

    model.OnTaskDone(Finished(0, Status::kOk, R"(C:\a.pdf)"));
    model.OnProgress(ipc::Progress{1, 1, 0.6, 0, 0});

    ipc::Error error;
    error.jobId = 1;
    error.status = static_cast<std::uint8_t>(Status::kResourceLimit);
    error.message = "The worker ran out of memory.";
    model.OnError(error);

    CHECK(model.screen() == Screen::kDone);
    CHECK(model.succeeded() == 1);
    CHECK(model.failed() == 1);
    CHECK(model.rows()[1].error == "The worker ran out of memory.");
}

// --- The done screen ---------------------------------------------------------

TEST_CASE("the done screen's summary agrees with its own list", "[app][model]") {
    AppModel model{TableProber()};
    model.Drop({R"(C:\a.md)", R"(C:\c.png)", R"(C:\b.xyz)"});
    model.SetTargets(TargetList({Option("application/pdf", Fidelity::Structural, 2)}));
    model.Begin(model.BuildJob("application/pdf"));

    ipc::TaskDone first = Finished(0, Status::kOk, R"(C:\a.pdf)");
    first.fidelity = static_cast<std::uint8_t>(Fidelity::Structural);
    first.notes.push_back(ipc::NoteWire{"table-dropped", "One table became paragraphs.", ""});
    model.OnTaskDone(first);
    model.OnTaskDone(Finished(1, Status::kMalformedInput, {}, "The PNG's header is truncated."));

    // JobDone's own totals say the same thing, but the summary sits directly
    // above the list and disagreeing with it is worse than either being wrong.
    model.OnJobDone(ipc::JobDone{1, 1, 1, 0});

    CHECK(model.screen() == Screen::kDone);
    CHECK(model.succeeded() == 1);
    CHECK(model.failed() == 2);  // the malformed PNG and the file never submitted
    REQUIRE(model.rows()[0].notes.size() == 1);
    CHECK(model.rows()[0].notes[0].code == "table-dropped");
    CHECK(model.rows()[1].error == "The PNG's header is truncated.");
}

TEST_CASE("a row the broker never reported on does not stay running", "[app][model]") {
    AppModel model{TableProber()};
    model.Drop({R"(C:\a.md)"});
    model.SetTargets(TargetList({Option("application/pdf", Fidelity::Structural, 2)}));
    model.Begin(model.BuildJob("application/pdf"));

    model.OnProgress(ipc::Progress{1, 0, 0.4, 0, 0});
    model.OnJobDone(ipc::JobDone{1, 0, 0, 0});

    CHECK(model.rows()[0].state == RowState::kFailed);
    CHECK_FALSE(model.rows()[0].error.empty());
}

TEST_CASE("convert more empties the selection and keeps the destination", "[app][model]") {
    AppModel model{TableProber()};
    model.output().directory = R"(D:\converted)";
    model.Drop({R"(C:\a.md)"});
    model.SetTargets(TargetList({Option("application/pdf", Fidelity::Structural, 2)}));
    model.Begin(model.BuildJob("application/pdf"));
    model.OnTaskDone(Finished(0, Status::kOk, R"(D:\converted\a.pdf)"));
    model.OnJobDone(ipc::JobDone{1, 1, 0, 0});

    model.ConvertMore();

    CHECK(model.screen() == Screen::kDropZone);
    CHECK(model.inputs().empty());
    CHECK(model.rows().empty());
    CHECK(model.succeeded() == 0);
    // Asking again after every batch is what makes a tool tiring to use.
    CHECK(model.output().directory == R"(D:\converted)");
}

TEST_CASE("dropping onto the done screen starts a new selection", "[app][model]") {
    AppModel model{TableProber()};
    model.Drop({R"(C:\a.md)"});
    model.SetTargets(TargetList({Option("application/pdf", Fidelity::Structural, 2)}));
    model.Begin(model.BuildJob("application/pdf"));
    model.OnTaskDone(Finished(0, Status::kOk, R"(C:\a.pdf)"));
    model.OnJobDone(ipc::JobDone{1, 1, 0, 0});

    model.Drop({R"(C:\c.png)"});

    CHECK(model.screen() == Screen::kTargetPicker);
    REQUIRE(model.inputs().size() == 1);
    CHECK(model.inputs()[0].path == R"(C:\c.png)");
    CHECK(model.rows().empty());
}

// --- The output preview (section 10.2, section 12.2's B3 panel) --------------
//
// §10.2 calls the output policy "the part users get wrong and support questions
// are about", and the reason is that nothing shows the answer until the files
// exist. These are about that answer being right BEFORE they do.

TEST_CASE("The preview shows where the first file will land", "[app][output]") {
    AppModel model(TableProber());
    model.Drop({R"(C:\notes\report.md)"});

    const auto preview = model.PreviewOutput("application/pdf");
    CHECK(preview.warning.empty());
    // Beside the input by default, stem kept, extension from the target.
    CHECK(preview.example.find("report.md") != std::string::npos);
    CHECK(preview.example.find(R"(C:\notes\report.pdf)") != std::string::npos);
}

TEST_CASE("A destination folder moves the preview with it", "[app][output]") {
    AppModel model(TableProber());
    model.Drop({R"(C:\notes\report.md)"});
    model.output().directory = R"(D:\Converted)";

    const auto preview = model.PreviewOutput("application/pdf");
    CHECK(preview.example.find(R"(D:\Converted\report.pdf)") != std::string::npos);
    // The source is still named, because the line is about the mapping and a
    // destination on its own does not say which file it is for.
    CHECK(preview.example.find("report.md") != std::string::npos);
}

TEST_CASE("The preview uses the template the user typed", "[app][output]") {
    AppModel model(TableProber());
    model.Drop({R"(C:\notes\report.md)"});
    model.output().namingTemplate = "{index}-{name}-converted.{ext}";

    const auto preview = model.PreviewOutput("application/pdf");
    CHECK(preview.example.find("0-report-converted.pdf") != std::string::npos);
    CHECK(preview.warning.empty());
}

TEST_CASE("The preview carries the same warning the core gives", "[app][output]") {
    AppModel model(TableProber());
    model.Drop({R"(C:\notes\report.md)"});
    model.output().namingTemplate = "everything.{ext}";

    const auto preview = model.PreviewOutput("application/pdf");
    CHECK(preview.warning == NamingTemplateWarning("everything.{ext}"));
    REQUIRE_FALSE(preview.warning.empty());
    // The line still resolves, because the template is legal -- it is the
    // sameness across files that is the problem, and one line cannot show it.
    CHECK(preview.example.find("everything.pdf") != std::string::npos);
}

TEST_CASE("A preview of a target Morph cannot write is not invented",
          "[app][output]") {
    AppModel model(TableProber());
    model.Drop({R"(C:\notes\report.md)"});

    // No extension in the table means nothing can be predicted, and a guessed
    // one would be a filename the broker never produces.
    CHECK(model.PreviewOutput("application/morph-ir").example.empty());
}

TEST_CASE("With nothing usable dropped there is nothing to preview",
          "[app][output]") {
    AppModel model(TableProber());
    CHECK(model.PreviewOutput("application/pdf").example.empty());

    model.Drop({R"(C:\notes\thing.xyz)"});
    CHECK(model.PreviewOutput("application/pdf").example.empty());
    // The unusable file does not become the example just because it is first.
    model.Drop({R"(C:\notes\report.md)"});
    CHECK(model.PreviewOutput("application/pdf").example.find("report") != std::string::npos);
}

TEST_CASE("Convert more keeps the output policy", "[app][output]") {
    AppModel model(TableProber());
    model.output().directory = R"(D:\Converted)";
    model.output().namingTemplate = "{name}-v2.{ext}";
    model.output().overwrite = ipc::OverwriteRule::kSkip;

    model.Drop({R"(C:\notes\report.md)"});
    model.ConvertMore();

    // Section 12.2's reason: they set it for a purpose and being made to set it
    // again after every batch is what makes a tool annoying.
    CHECK(model.output().directory == R"(D:\Converted)");
    CHECK(model.output().namingTemplate == "{name}-v2.{ext}");
    CHECK(model.output().overwrite == ipc::OverwriteRule::kSkip);
}
