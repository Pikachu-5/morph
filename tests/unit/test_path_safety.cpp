// Device names and the names Windows quietly rewrites (architecture section
// 15.2's path-handling row).
//
// One of these tests asserts a fact about the operating system rather than about
// Morph, and it is here for a reason: the whole justification for this module is
// that CreateFileW can succeed while creating nothing. If that ever stops being
// true on every supported build, the code below is dead weight and this test is
// where someone would find that out.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <process.h>
#include <string>

#include <windows.h>

#include "morph/core/path_safety.h"
#include "morph/core/stream.h"

using namespace morph;
using namespace morph::io;

namespace {

class Sandbox {
public:
    explicit Sandbox(const std::string& label) {
        root_ = std::filesystem::temp_directory_path() /
                ("morph-paths-" + label + "-" + std::to_string(::_getpid()));
        std::filesystem::create_directories(root_);
    }
    ~Sandbox() {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }
    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

private:
    std::filesystem::path root_;
};

}  // namespace

TEST_CASE("device names are recognised however they are spelled", "[paths]") {
    CHECK(IsDeviceName("NUL"));
    CHECK(IsDeviceName("nul"));
    CHECK(IsDeviceName("Con"));
    CHECK(IsDeviceName("COM1"));
    CHECK(IsDeviceName("lpt9"));
    CHECK(IsDeviceName("AUX"));
    CHECK(IsDeviceName("conout$"));

    // The parser looks at the text before the first dot, and ignores trailing
    // spaces, so an extension does not make a device name safe.
    CHECK(IsDeviceName("nul.pdf"));
    CHECK(IsDeviceName("con.png"));
    CHECK(IsDeviceName("com1 .txt"));
    CHECK(IsDeviceName("nul.pdf.morph-partial"));

    // Near misses. These are ordinary files and refusing them would be a bug of
    // its own -- "console.md" and "nullable.txt" are things people name files.
    CHECK_FALSE(IsDeviceName("console.md"));
    CHECK_FALSE(IsDeviceName("nullable.txt"));
    CHECK_FALSE(IsDeviceName("com.txt"));
    CHECK_FALSE(IsDeviceName("com10.txt"));
    CHECK_FALSE(IsDeviceName("my-nul.pdf"));
    CHECK_FALSE(IsDeviceName(""));
}

TEST_CASE("a derived name is repaired rather than refused", "[paths]") {
    // The extension has to survive: it is what decides how the result opens.
    CHECK(SafeComponent("nul.pdf") == "nul_.pdf");
    CHECK(SafeComponent("CON.png") == "CON_.png");
    CHECK(SafeComponent("nul") == "nul_");

    // Windows drops these silently, so we drop them visibly and the recorded
    // path is the path that ends up on disk.
    CHECK(SafeComponent("report.pdf.") == "report.pdf");
    CHECK(SafeComponent("report.pdf  ") == "report.pdf");

    // Separators cannot survive: a stem carrying one would escape the directory
    // the output policy chose.
    CHECK(SafeComponent("../../etc/passwd") == "..-..-etc-passwd");
    CHECK(SafeComponent("a:b|c?.png") == "a-b-c-.png");
    CHECK(SafeComponent(std::string("tab\there.md")) == "tab-here.md");

    // Ordinary names come through untouched, including the unicode and the
    // spaces real filenames are full of.
    CHECK(SafeComponent("My Report (final).docx") == "My Report (final).docx");
}

TEST_CASE("a typed destination is refused with a sentence", "[paths]") {
    const PathVerdict device = CheckDestination("C:\\photos\\con.png");
    REQUIRE_FALSE(device.ok);
    CHECK(device.reason.find("device") != std::string::npos);

    const PathVerdict trailing = CheckDestination("C:\\photos\\report.pdf.");
    REQUIRE_FALSE(trailing.ok);

    CHECK(CheckDestination("C:\\photos\\report.pdf").ok);
    CHECK(CheckDestination("report.pdf").ok);

    // A directory component that is a device name is refused too. The open would
    // fail anyway; the point is that it fails saying what is wrong.
    CHECK_FALSE(CheckDestination("C:\\nul\\report.pdf").ok);
}

TEST_CASE("FileSink refuses a device destination", "[paths]") {
    Sandbox sandbox("sink");

    // This is the failure the module exists to prevent, demonstrated first: the
    // bare device name opens, accepts the bytes, and leaves nothing behind.
    const std::filesystem::path device = sandbox.root() / "NUL";
    const HANDLE raw = ::CreateFileW(device.native().c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(raw != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    ::WriteFile(raw, "hello", 5, &written, nullptr);
    ::CloseHandle(raw);
    CHECK(written == 5);
    // Nothing was created. Asked directly, the path does not even answer -- it
    // is a device, and exists() reports "the parameter is incorrect" rather than
    // false -- so the directory is what gets asked.
    CHECK(std::filesystem::is_empty(sandbox.root()));

    // FileSink, given the same path, says so instead.
    CHECK_THROWS(FileSink(device));
    CHECK_THROWS(FileSink(sandbox.root() / "con.png"));

    // And still writes an ordinary file.
    {
        FileSink sink(sandbox.root() / "fine.png");
        const char bytes[] = "hello";
        sink.Write(std::as_bytes(std::span(bytes, 5)));
        sink.Commit();
    }
    CHECK(std::filesystem::file_size(sandbox.root() / "fine.png") == 5);
}
