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
constexpr std::array<FormatExtension, 20> kTable{{
    {"text/markdown", "md", "markdown", true},
    {"text/html", "html", "htm", true},
    {"application/docx", "docx", "", true},
    {"application/pdf", "pdf", "", true},

    // Images (section 6). Whether a HEIF, WebP or AVIF edge exists at all
    // depends on the codecs installed, and the WIC provider decides that when it
    // enumerates its edges -- this table only says what such a file would be
    // called if one can be written.
    {"image/png", "png", "", true},
    {"image/jpeg", "jpg", "jpeg", true},
    {"image/bmp", "bmp", "", true},
    {"image/tiff", "tiff", "tif", true},
    // Read, never written: a GIF written from a full-colour source is quantised
    // to 256 colours, and the provider will not offer that until it carries a
    // fidelity rating that says so.
    {"image/gif", "gif", "", false},
    {"image/heif", "heic", "heif", true},
    {"image/webp", "webp", "", true},

    // Audio and video (section 7). Two profiles can share an extension -- an MP4
    // holding H.264 and one holding HEVC are different nodes and the same file
    // name -- so the order matters in one direction only: ProfileForExtension
    // takes the first match, which is what a user typing "out.mp4" means.
    {"video/mp4;codec=h264", "mp4", "m4v", true},
    {"video/mp4;codec=hevc", "mp4", "", true},
    // Read, never written: Media Foundation has no QuickTime or ASF sink, and
    // the FFmpeg backend will not change that for MOV (section 7.3).
    {"video/quicktime;codec=h264", "mov", "qt", false},
    {"video/x-ms-asf;codec=wmv", "wmv", "asf", false},

    {"audio/mp4;codec=aac", "m4a", "aac", true},
    {"audio/mpeg;codec=mp3", "mp3", "", true},
    {"audio/wav;codec=pcm", "wav", "wave", true},
    // Decoded on Windows 10 and later; never encoded. The GitHub SKU's FFmpeg
    // backend is where FLAC output will come from.
    {"audio/flac", "flac", "", false},
    {"audio/x-ms-wma;codec=wma", "wma", "", true},
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
