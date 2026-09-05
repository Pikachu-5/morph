// Opening the app on a selection (architecture section 9.2).
//
// The generic "Convert with Morph..." entry is one CreateProcess, and the only
// part of it that can be subtly wrong is the command line -- which the app takes
// apart again with CommandLineToArgvW. So the round trip is what is tested:
// build a line, hand it back to Windows, and check the paths that come out are
// the paths that went in.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include "shellext/launch.h"

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
