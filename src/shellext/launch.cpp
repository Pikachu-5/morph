#include "launch.h"

namespace morph::shellext {

std::wstring QuoteArgument(std::wstring_view argument) {
    // Always quoted, even when nothing needs it. An unquoted argument is only
    // safe if it contains no space and no tab, and deciding that per path is one
    // more thing to be wrong about; CommandLineToArgvW takes the quoted form back
    // apart identically either way.
    std::wstring out;
    out.reserve(argument.size() + 2);
    out.push_back(L'"');

    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            // A run of backslashes before a quote is doubled, and the quote is
            // escaped: 2n+1 backslashes yield n literal ones and a literal quote.
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        // Elsewhere a backslash means a backslash.
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }

    // Trailing backslashes are doubled, because the quote that follows them is
    // the terminator and must not be escaped by the last of them. "C:\" is the
    // ordinary way to reach this.
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring AppCommandLine(std::wstring_view installDirectory,
                            const std::vector<std::wstring>& paths) {
    if (installDirectory.empty() || paths.empty()) return {};

    std::wstring line = QuoteArgument(std::wstring(installDirectory) + L"\\Morph.App.exe");
    bool any = false;
    for (const std::wstring& path : paths) {
        if (path.empty()) continue;
        line.push_back(L' ');
        line.append(QuoteArgument(path));
        any = true;
        // Checked as it grows rather than at the end, so that a selection of ten
        // thousand paths does not build a megabyte of string before being thrown
        // away.
        if (line.size() > kMaxCommandLineChars) return {};
    }

    // Every path was empty: argv[0] alone would open the app on nothing, which
    // the caller can do deliberately but should not get by accident. Counted
    // rather than looked for, because the install directory is allowed a space
    // of its own ("C:\Program Files\...") and a search would find that one.
    return any ? line : std::wstring();
}

}  // namespace morph::shellext
