#include "morph/core/extensions.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace morph {
namespace {

struct Entry {
    std::string_view profile;
    std::string_view extension;  // the canonical one, used when naming an output
    std::string_view alternate;  // also accepted on input; empty when there is none
};

// One row per format we WRITE. A format we only read has no business here: the
// question this table answers is "what do I call the output", and a reader has
// no answer to it.
constexpr std::array<Entry, 4> kTable{{
    {"text/markdown", "md", "markdown"},
    {"text/html", "html", "htm"},
    {"application/docx", "docx", ""},
    {"application/pdf", "pdf", ""},
}};

std::string Lowered(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

std::string_view ExtensionFor(std::string_view profileId) noexcept {
    for (const Entry& entry : kTable) {
        if (entry.profile == profileId) return entry.extension;
    }
    return {};
}

std::string_view ProfileForExtension(std::string_view extension) noexcept {
    if (!extension.empty() && extension.front() == '.') extension.remove_prefix(1);
    const std::string lowered = Lowered(extension);

    for (const Entry& entry : kTable) {
        if (entry.extension == lowered) return entry.profile;
        if (!entry.alternate.empty() && entry.alternate == lowered) return entry.profile;
    }
    return {};
}

}  // namespace morph
