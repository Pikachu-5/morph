#include "morph/core/path_safety.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace morph::io {
namespace {

// The full DOS device set. COM0 and LPT0 are in it: the parser accepts them even
// though no such device is ever present, which is exactly the kind of gap a
// hand-written list gets wrong.
constexpr std::array<std::string_view, 26> kDeviceNames{
    "CON",  "PRN",  "AUX",  "NUL",  "CONIN$", "CONOUT$",
    "COM0", "COM1", "COM2", "COM3", "COM4",   "COM5", "COM6", "COM7", "COM8", "COM9",
    "LPT0", "LPT1", "LPT2", "LPT3", "LPT4",   "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};

bool EqualsIgnoringCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::toupper(static_cast<unsigned char>(x)) ==
                      std::toupper(static_cast<unsigned char>(y));
           });
}

// The part of a component the device parser looks at: everything before the
// first '.', with trailing spaces removed. "com1 .txt" is COM1.
std::string_view DeviceCandidate(std::string_view name) {
    std::string_view head = name.substr(0, name.find('.'));
    while (!head.empty() && head.back() == ' ') head.remove_suffix(1);
    return head;
}

bool IsIllegal(char c) {
    switch (c) {
        case '<': case '>': case ':': case '"': case '/':
        case '\\': case '|': case '?': case '*':
            return true;
        default:
            // Control characters are not storable either, and a filename
            // carrying one is a sign someone is trying something.
            return static_cast<unsigned char>(c) < 0x20;
    }
}

}  // namespace

bool IsDeviceName(std::string_view name) {
    const std::string_view head = DeviceCandidate(name);
    return std::any_of(kDeviceNames.begin(), kDeviceNames.end(),
                       [head](std::string_view device) {
                           return EqualsIgnoringCase(head, device);
                       });
}

std::string SafeComponent(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) out.push_back(IsIllegal(c) ? '-' : c);

    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();

    if (IsDeviceName(out)) {
        // Append to the stem rather than the whole name, so the extension --
        // which is what decides how the result opens -- survives intact.
        out.insert(std::min(out.find('.'), out.size()), "_");
    }
    return out;
}

PathVerdict CheckDestination(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    if (name.empty()) {
        return {false, "That path names a directory, not a file to write."};
    }
    if (IsDeviceName(name)) {
        // Naming the reason matters here: "cannot write to NUL" reads like a
        // permissions problem, and the user's next move would be to retry as
        // administrator.
        return {false, "\"" + name +
                           "\" is the name of a Windows device, not a file. Writing there "
                           "would send the conversion nowhere."};
    }
    if (name.back() == '.' || name.back() == ' ') {
        return {false, "\"" + name +
                           "\" ends in a dot or a space, which Windows drops silently. The "
                           "file would not be where you asked for it."};
    }
    // Every component between here and the root is checked too: a device name in
    // the middle of a path is not creatable, but a trailing dot on a DIRECTORY
    // resolves to a different directory without saying so.
    for (const auto& component : path.parent_path()) {
        const std::string text = component.string();
        if (text.empty() || text == "/" || text == "\\") continue;
        if (IsDeviceName(text)) {
            return {false, "\"" + text + "\" in that path is a Windows device name."};
        }
    }
    return {true, {}};
}

}  // namespace morph::io
