// The output policy (architecture section 10.2's OutputPolicy).
//
// These are filesystem tests on purpose. The rename rule is a question about
// what is already on disk, and a fake that answers it from a set would be
// testing the fake.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <process.h>
#include <fstream>
#include <string>
#include <vector>

#include "broker/output.h"

using namespace morph;
using namespace morph::broker;

namespace {

// A directory that cleans up after itself, named per test case so two of them
// running in the same session cannot collide.
class Sandbox {
public:
    explicit Sandbox(const std::string& label) {
        root_ = std::filesystem::temp_directory_path() /
                ("morph-output-" + label + "-" + std::to_string(::_getpid()));
        std::filesystem::create_directories(root_);
    }
    ~Sandbox() {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

    std::filesystem::path Write(const std::string& name, std::string_view contents = "x") {
        const std::filesystem::path path = root_ / name;
        std::ofstream stream(path, std::ios::binary);
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        return path;
    }

private:
    std::filesystem::path root_;
};

ipc::OutputPolicy DefaultPolicy() {
    ipc::OutputPolicy policy;
    policy.namingTemplate = "{name}.{ext}";
    policy.overwrite = ipc::OverwriteRule::kRename;
    return policy;
}

}  // namespace

TEST_CASE("A template substitutes every placeholder it knows", "[broker][output]") {
    CHECK(ApplyTemplate("{name}.{ext}", "report", "pdf", 0, "application/pdf") == "report.pdf");
    CHECK(ApplyTemplate("{index}-{name}.{ext}", "a", "md", 7, "text/markdown") == "7-a.md");

    // A media profile has a slash in it and a file name may not.
    CHECK(ApplyTemplate("{target}.{ext}", "a", "pdf", 0, "application/pdf") ==
          "application-pdf.pdf");
}

TEST_CASE("An unknown or unterminated placeholder survives visibly", "[broker][output]") {
    // Better a filename with {nmae} in it than one that silently lost half the
    // template: the user can see their typo and fix it.
    CHECK(ApplyTemplate("{nmae}.{ext}", "report", "pdf", 0, "") == "{nmae}.pdf");
    CHECK(ApplyTemplate("{name", "report", "pdf", 0, "") == "{name");
}

TEST_CASE("An output lands beside its input by default", "[broker][output]") {
    Sandbox sandbox("beside");
    const std::filesystem::path input = sandbox.Write("notes.md");

    std::vector<std::filesystem::path> taken;
    const OutputPlan plan = ResolveOutput(input, "application/pdf", DefaultPolicy(), 0, taken);

    CHECK(plan.error.empty());
    CHECK(plan.path == sandbox.root() / "notes.pdf");
    CHECK_FALSE(plan.skip);
}

TEST_CASE("A target Morph does not write fails the task, not the job", "[broker][output]") {
    Sandbox sandbox("unknown-target");
    const std::filesystem::path input = sandbox.Write("notes.md");

    std::vector<std::filesystem::path> taken;

    // QuickTime is read and never written -- Media Foundation has no .mov sink
    // -- which is exactly the case the `writable` flag in the extension table
    // exists for. A format that is merely unknown must fail the same way.
    CHECK_FALSE(
        ResolveOutput(input, "video/quicktime;codec=h264", DefaultPolicy(), 0, taken)
            .error.empty());
    CHECK_FALSE(
        ResolveOutput(input, "application/x-nonesuch", DefaultPolicy(), 0, taken).error.empty());
}

TEST_CASE("The rename rule counts up, and never destroys anything", "[broker][output]") {
    Sandbox sandbox("rename");
    const std::filesystem::path input = sandbox.Write("notes.md");
    sandbox.Write("notes.pdf");

    std::vector<std::filesystem::path> taken;
    const OutputPlan first = ResolveOutput(input, "application/pdf", DefaultPolicy(), 0, taken);
    CHECK(first.path.filename() == "notes (1).pdf");

    // The second call must not hand back the same name: the first one is claimed
    // even though nothing has written it yet.
    const std::filesystem::path second_input = sandbox.Write("notes2.md");
    ipc::OutputPolicy policy = DefaultPolicy();
    policy.namingTemplate = "notes.{ext}";
    const OutputPlan second =
        ResolveOutput(second_input, "application/pdf", policy, 1, taken);
    CHECK(second.path.filename() == "notes (2).pdf");
}

TEST_CASE("The skip rule reports rather than overwrites", "[broker][output]") {
    Sandbox sandbox("skip");
    const std::filesystem::path input = sandbox.Write("notes.md");
    sandbox.Write("notes.pdf");

    ipc::OutputPolicy policy = DefaultPolicy();
    policy.overwrite = ipc::OverwriteRule::kSkip;

    std::vector<std::filesystem::path> taken;
    const OutputPlan plan = ResolveOutput(input, "application/pdf", policy, 0, taken);
    CHECK(plan.skip);
    CHECK(plan.error.empty());
    CHECK(plan.path.filename() == "notes.pdf");
}

TEST_CASE("The replace rule takes the name it was given", "[broker][output]") {
    Sandbox sandbox("replace");
    const std::filesystem::path input = sandbox.Write("notes.md");
    sandbox.Write("notes.pdf");

    ipc::OutputPolicy policy = DefaultPolicy();
    policy.overwrite = ipc::OverwriteRule::kReplace;

    std::vector<std::filesystem::path> taken;
    const OutputPlan plan = ResolveOutput(input, "application/pdf", policy, 0, taken);
    CHECK_FALSE(plan.skip);
    CHECK(plan.path.filename() == "notes.pdf");
}

TEST_CASE("Nothing is allowed to write over its own input", "[broker][output]") {
    // html -> text/html with kReplace is the reachable case: the name resolves
    // to the input itself, and the reader is still streaming from it.
    Sandbox sandbox("self");
    const std::filesystem::path input = sandbox.Write("page.html");

    ipc::OutputPolicy policy = DefaultPolicy();
    policy.overwrite = ipc::OverwriteRule::kReplace;

    std::vector<std::filesystem::path> taken;
    const OutputPlan plan = ResolveOutput(input, "text/html", policy, 0, taken);
    CHECK_FALSE(plan.error.empty());
}
