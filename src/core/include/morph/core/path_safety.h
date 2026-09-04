// Morph — file name and destination safety (architecture section 15.2).
//
// Section 15.2's last row asks for "long paths enabled, \\?\ prefixing,
// reserved-name rejection (CON, PRN, NUL, ...), and no shell expansion anywhere
// near a user-supplied filename". The first two live in stream.cpp, which is
// where paths become handles. The third is here, because it is a decision about
// a NAME and has to be made in two quite different places: when the broker
// derives an output name from an input name (§10.2's OutputPolicy), and when a
// caller hands us a whole path it typed itself.
//
// Why this is worth having even though it looks like superstition: Win32 path
// parsing resolves a name to a device before it ever reaches the filesystem, so
// CreateFileW("C:\photos\con.png") can succeed while creating nothing at all.
// Every byte written goes to the console device and Commit renames a file that
// is not there. The user is told the conversion succeeded and has nothing.
//
// The precise rule has moved between Windows builds -- on 11 26220 only a bare
// `NUL` still reaches the device and `nul.pdf` is an ordinary file, whereas on
// 22H2 both are the device -- so this rejects the whole family rather than
// tracking which builds intercept what. A name Morph refuses to write is a name
// no Windows shell will let a user create in the first place.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace morph::io {

// True for the DOS device names, matched the way the Win32 parser matches them:
// case-insensitively, on the text up to the first '.', ignoring trailing spaces.
// So "NUL", "nul.pdf", "COM1  " and "conout$.md" are all device names.
[[nodiscard]] bool IsDeviceName(std::string_view name);

// Makes one path COMPONENT safe to create: strips the characters Windows will
// not store, folds control characters, removes the trailing dots and spaces the
// filesystem would silently drop anyway, and defuses a device name by appending
// an underscore to the part before the first dot ("nul.pdf" -> "nul_.pdf").
//
// It repairs rather than rejects because this is the path a DERIVED name takes:
// refusing to convert someone's file because of what it is called would be a
// worse answer than converting it to a name one character different.
//
// The trailing-dot rule is not cosmetic. Win32 stores "report.pdf." as
// "report.pdf", so a broker that recorded the first spelling would report an
// output path that does not exist and would miss the collision when the next
// file in the batch claims the second.
[[nodiscard]] std::string SafeComponent(std::string_view name);

// What CheckDestination decided. `reason` is empty iff `ok`, and is written to
// be shown to a user rather than logged.
struct PathVerdict {
    bool ok = true;
    std::string reason;
};

// Vets a whole destination path that a caller supplied verbatim -- the CLI's
// second argument, or a directory that arrived over IPC. This one REFUSES,
// because a path someone typed should come back as an error rather than be
// quietly written somewhere else.
[[nodiscard]] PathVerdict CheckDestination(const std::filesystem::path& path);

}  // namespace morph::io
