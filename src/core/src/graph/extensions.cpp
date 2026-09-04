#include "morph/core/extensions.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace morph {
namespace {

// One row per format Morph reads, writes, or both. The `writable` flag is what
// keeps a read-only format out of the target list: PDF input arrived in §8.7
// long before anything could be said about writing every format we can read, and
// a menu that offers a target we cannot produce is worse than one that is short.
constexpr std::array<FormatExtension, 4> kTable{{
    {"text/markdown", "md", "markdown", true},
    {"text/html", "html", "htm", true},
    {"application/docx", "docx", "", true},
    {"application/pdf", "pdf", "", true},
}};

std::string Lowered(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

std::span<const FormatExtension> KnownExtensions() noexcept {
    return std::span<const FormatExtension>(kTable.data(), kTable.size());
}

std::string_view ExtensionFor(std::string_view profileId) noexcept {
    for (const FormatExtension& entry : kTable) {
        if (entry.profile == profileId && entry.writable) return entry.extension;
    }
    return {};
}

std::string_view ProfileForExtension(std::string_view extension) noexcept {
    if (!extension.empty() && extension.front() == '.') extension.remove_prefix(1);
    const std::string lowered = Lowered(extension);

    for (const FormatExtension& entry : kTable) {
        if (entry.extension == lowered) return entry.profile;
        if (!entry.alternate.empty() && entry.alternate == lowered) return entry.profile;
    }
    return {};
}

}  // namespace morph
