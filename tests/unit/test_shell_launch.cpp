// The two seams in section 9 that nothing else watches.
//
// OPENING THE APP on a selection (9.2) is one CreateProcess, and the only part
// of it that can be subtly wrong is the command line -- which the app takes
// apart again with CommandLineToArgvW. So the round trip is what is tested:
// build a line, hand it back to Windows, and check the paths that come out are
// the paths that went in.
//
// THE CLSID (9.1) joins src/shellext to packaging/msix and lives in two files
// that no compiler reads together. The last case in this file reads them
// together.

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include <pugixml.hpp>

#include "shellext/launch.h"
#include "shellext/shellext.h"

using namespace morph::shellext;

namespace {

// What Morph.App.exe's wWinMain sees: argv with the exe dropped.
std::vector<std::wstring> ArgumentsOf(const std::wstring& commandLine) {
    std::vector<std::wstring> out;
    int count = 0;
    LPWSTR* argv = ::CommandLineToArgvW(commandLine.c_str(), &count);
    if (argv == nullptr) return out;
    for (int i = 1; i < count; ++i) out.emplace_back(argv[i]);
    ::LocalFree(argv);
    return out;
}

std::wstring ExecutableOf(const std::wstring& commandLine) {
    int count = 0;
    LPWSTR* argv = ::CommandLineToArgvW(commandLine.c_str(), &count);
    if (argv == nullptr || count == 0) return {};
    std::wstring out(argv[0]);
    ::LocalFree(argv);
    return out;
}

}  // namespace

TEST_CASE("The app command line survives CommandLineToArgvW", "[shellext][launch]") {
    const std::vector<std::wstring> paths = {
        LR"(C:\Users\a\holiday.png)",
        // The two that a naive join gets wrong.
        LR"(C:\Users\a b\a file with spaces.png)",
        LR"(C:\)",
        // Unicode, because a selection is whatever the user named it.
        L"C:\\Users\\a\\\u65e5\u672c\u8a9e.png",
    };

    const std::wstring line = AppCommandLine(LR"(C:\Program Files\Morph)", paths);
    REQUIRE_FALSE(line.empty());

    CHECK(ExecutableOf(line) == LR"(C:\Program Files\Morph\Morph.App.exe)");
    CHECK(ArgumentsOf(line) == paths);
}

TEST_CASE("A trailing backslash does not escape the closing quote",
          "[shellext][launch]") {
    // "C:\" quoted naively becomes "C:\" -- and the backslash escapes the quote,
    // so the argument swallows whatever follows it. With two paths the damage is
    // visible: one argument comes back instead of two.
    const std::vector<std::wstring> paths = {LR"(D:\)", LR"(E:\photos\)"};
    const std::wstring line = AppCommandLine(LR"(C:\Morph)", paths);
    REQUIRE_FALSE(line.empty());
    CHECK(ArgumentsOf(line) == paths);
}

TEST_CASE("A quote in a name is escaped rather than terminating the argument",
          "[shellext][launch]") {
    // NTFS will not take a quote in a file name, but the paths here come from a
    // shell item and not from NTFS, and QuoteArgument is the wrong place to
    // assume otherwise.
    CHECK(ArgumentsOf(LR"(x.exe )" + QuoteArgument(LR"(a"b)")) ==
          std::vector<std::wstring>{LR"(a"b)"});
    CHECK(ArgumentsOf(LR"(x.exe )" + QuoteArgument(LR"(a\"b)")) ==
          std::vector<std::wstring>{LR"(a\"b)"});
    CHECK(ArgumentsOf(LR"(x.exe )" + QuoteArgument(LR"(a\\b)")) ==
          std::vector<std::wstring>{LR"(a\\b)"});
}

TEST_CASE("There is no command line without somewhere to send it",
          "[shellext][launch]") {
    CHECK(AppCommandLine(L"", {LR"(C:\a.png)"}).empty());
    CHECK(AppCommandLine(LR"(C:\Morph)", {}).empty());
    // All-empty paths are not a selection: argv[0] alone would open the app on
    // nothing, and the menu doing nothing is the better answer.
    CHECK(AppCommandLine(LR"(C:\Morph)", {L"", L""}).empty());
}

TEST_CASE("A selection too long for a command line is refused, not truncated",
          "[shellext][launch]") {
    // Truncating would convert some of the user's files and quietly drop the
    // rest, which is the one outcome worse than the menu doing nothing.
    std::vector<std::wstring> paths;
    paths.reserve(2000);
    for (int i = 0; i < 2000; ++i) {
        paths.push_back(LR"(C:\a\)" + std::to_wstring(i) +
                        L"-0123456789012345678901234567890.png");
    }
    CHECK(AppCommandLine(LR"(C:\Morph)", paths).empty());

    // And the boundary holds from the other side: a big-but-legal selection is
    // still built, and still comes back intact.
    paths.resize(100);
    const std::wstring line = AppCommandLine(LR"(C:\Morph)", paths);
    REQUIRE_FALSE(line.empty());
    CHECK(line.size() <= kMaxCommandLineChars);
    CHECK(ArgumentsOf(line) == paths);
}

// --- The other seam: the class id in the package manifest --------------------

TEST_CASE("The manifest declares the class the DLL implements",
          "[shellext][manifest]") {
    // Section 9.1's registration and src/shellext are joined by exactly one
    // thing, and it is a GUID written out twice in two languages. Nothing
    // compiles both, so nothing catches a typo: what it produces is a package
    // that installs cleanly, a verb that registers, and a class that fails to
    // activate -- reported by Explorer as no menu at all, on a machine where
    // everything looks right.

    GUID declared{};
    // The manifest spells it bare; IIDFromString wants the braces.
    const std::wstring braced =
        L"{" + std::wstring(L"" MORPH_SHELLEXT_CLSID_STRING) + L"}";
    REQUIRE(::IIDFromString(braced.c_str(), &declared) == S_OK);
    CHECK(::IsEqualGUID(declared, kMorphContextMenuClsid));

    // And that CMake's string is the one the template actually carries, rather
    // than a fourth copy that happens to agree with the header.
    std::ifstream file(MORPH_APPXMANIFEST_TEMPLATE);
    REQUIRE(file.is_open());
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string manifest = buffer.str();

    // Both places it appears -- the surrogate's class and the verb -- are
    // substituted from the same variable, so what is checked here is that the
    // substitution is still what the template asks for.
    CHECK(manifest.find("@MORPH_SHELLEXT_CLSID@") != std::string::npos);
    // A literal GUID left in the template would be a copy that configure_file
    // cannot keep in step.
    CHECK(manifest.find(MORPH_SHELLEXT_CLSID_STRING) == std::string::npos);
}

TEST_CASE("The manifest template is well-formed XML", "[shellext][manifest]") {
    // Found by makeappx, which is a bad place to find it: the manifest carries
    // several paragraphs of prose explaining itself, a double hyphen is not
    // legal inside an XML comment, and the deployment error names a line and a
    // column rather than the reason. The template's own warning about this had
    // been there since M2 and did not stop me writing one.
    //
    // The template rather than the generated file, because the template is what
    // a person edits and because it parses either way: an unsubstituted
    // @VARIABLE@ is an ordinary attribute value.
    pugi::xml_document document;
    const pugi::xml_parse_result parsed = document.load_file(MORPH_APPXMANIFEST_TEMPLATE);
    INFO(parsed.description() << " at offset " << parsed.offset);
    REQUIRE(parsed);

    // And it is the manifest we think it is, rather than a well-formed file
    // that happens to be somewhere else.
    const pugi::xml_node package = document.child("Package");
    REQUIRE(package);
    CHECK(std::string(package.child("Applications")
                          .child("Application")
                          .attribute("Executable")
                          .as_string()) == "Morph.App.exe");

    // The dependency whose absence from the old shipping manifest is why there
    // is a template at all. A packaged app declares the runtime; one that does
    // not is created and dies before main, with nothing written anywhere.
    CHECK(package.child("Dependencies").child("PackageDependency"));
}
