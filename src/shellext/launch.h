// Opening the app on a selection (architecture section 9.2).
//
// Section 9.2's degraded path is a single "Convert with Morph..." entry that
// "opens the app" -- shown when the capability table is absent, unreadable, or
// the selection is too varied for a submenu to be useful. The app takes files on
// its command line (`main.cpp`'s wWinMain), so the whole of that path is one
// string and one CreateProcess.
//
// The string is here, beside the menu logic and away from the COM, for the same
// reason the menu is: quoting a hundred paths into a command line that
// CommandLineToArgvW will take apart again is exactly the kind of thing that is
// wrong for one file in ten thousand, and testing it should not need Explorer.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace morph::shellext {

// A selection this long is not opened as a command line.
//
// The documented ceiling for CreateProcessW's lpCommandLine is 32,767
// characters. Truncating a selection silently would open the app on some of the
// user's files and not others, which is worse than the menu doing nothing, so
// the caller is told instead -- see AppCommandLine.
inline constexpr std::size_t kMaxCommandLineChars = 32'000;

// The command line that opens Morph.App.exe on these paths, or empty when there
// is no sensible one: no install directory, no paths, or a selection that would
// not fit. A caller must not fall back to a truncated line.
[[nodiscard]] std::wstring AppCommandLine(std::wstring_view installDirectory,
                                          const std::vector<std::wstring>& paths);

// One argument, quoted the way CommandLineToArgvW un-quotes it.
//
// Exposed for the test: the backslash rule is the part that is easy to get wrong
// and impossible to notice, because a Windows path never ends in one until the
// day someone selects a drive root.
[[nodiscard]] std::wstring QuoteArgument(std::wstring_view argument);

}  // namespace morph::shellext
