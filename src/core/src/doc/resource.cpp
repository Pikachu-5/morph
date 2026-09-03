#include "morph/core/doc/resource.h"

#include <algorithm>
#include <array>
#include <cstring>

#include <blake3.h>

namespace morph::doc {
namespace {

char HexDigit(unsigned v) { return static_cast<char>(v < 10 ? '0' + v : 'a' + (v - 10)); }

std::uint32_t ReadBigEndian32(const std::byte* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

std::uint32_t ReadLittleEndian32(const std::byte* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t ReadBigEndian16(const std::byte* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint32_t>(p[0]) << 8) |
                                      static_cast<std::uint32_t>(p[1]));
}

bool StartsWith(std::span<const std::byte> bytes, std::initializer_list<unsigned char> magic) {
    if (bytes.size() < magic.size()) return false;
    std::size_t i = 0;
    for (const unsigned char expected : magic) {
        if (static_cast<unsigned char>(bytes[i++]) != expected) return false;
    }
    return true;
}

// JPEG dimensions live in a Start Of Frame marker, which is somewhere after an
// arbitrary run of other markers. Walking them is unavoidable, but the walk is
// bounded: it stops at the end of whatever prefix the caller supplied, and never
// trusts a segment length that would run past it.
PixelSize SniffJpegSize(std::span<const std::byte> bytes) {
    std::size_t at = 2;  // past SOI
    while (at + 9 < bytes.size()) {
        if (static_cast<unsigned char>(bytes[at]) != 0xFF) {
            ++at;  // resynchronise rather than give up: padding 0xFF is legal
            continue;
        }
        const auto marker = static_cast<unsigned char>(bytes[at + 1]);
        if (marker == 0xFF) {
            ++at;
            continue;
        }
        // SOF0..SOF15, excluding the DHT/JPG/DAC markers interleaved in that
        // range, all carry height and width at the same offsets.
        const bool is_sof = (marker >= 0xC0 && marker <= 0xCF) && marker != 0xC4 &&
                            marker != 0xC8 && marker != 0xCC;
        if (is_sof) {
            return PixelSize{ReadBigEndian16(&bytes[at + 7]), ReadBigEndian16(&bytes[at + 5])};
        }
        // Markers without a payload.
        if (marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7)) {
            at += 2;
            continue;
        }
        const std::uint16_t length = ReadBigEndian16(&bytes[at + 2]);
        if (length < 2) return {};
        at += 2 + length;
    }
    return {};
}

}  // namespace

std::string ContentHash(std::span<const std::byte> bytes, std::size_t hexChars) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, bytes.data(), bytes.size());

    // BLAKE3 is an extendable-output function, so ask for exactly the bytes we
    // are going to print rather than taking 32 and throwing most away.
    const std::size_t digest_bytes = (hexChars + 1) / 2;
    std::array<std::uint8_t, BLAKE3_OUT_LEN> digest{};
    blake3_hasher_finalize(&hasher, digest.data(),
                           std::min(digest_bytes, digest.size()));

    std::string out;
    out.reserve(hexChars);
    for (std::size_t i = 0; i < digest_bytes && out.size() < hexChars; ++i) {
        out.push_back(HexDigit(digest[i] >> 4));
        if (out.size() < hexChars) out.push_back(HexDigit(digest[i] & 0x0Fu));
    }
    return out;
}

ResourceId ResourceStore::Add(std::span<const std::byte> bytes, std::string mime,
                              std::string hint) {
    const PixelSize size = SniffImageSize(bytes);
    return Add(bytes, std::move(mime), std::move(hint), size.width, size.height);
}

ResourceId ResourceStore::Add(std::span<const std::byte> bytes, std::string mime, std::string hint,
                              std::uint32_t widthPx, std::uint32_t heightPx) {
    ResourceId id = ContentHash(bytes);

    // Identical bytes are one resource. A logo repeated in a header on forty
    // pages arrives forty times and is stored once.
    if (const auto it = resources_.find(id); it != resources_.end()) {
        // First insert's metadata wins; a later caller with less information
        // must not degrade what we already know.
        Resource& existing = it->second;
        if (existing.mime.empty()) existing.mime = std::move(mime);
        if (existing.hint.empty()) existing.hint = std::move(hint);
        if (existing.widthPx == 0) existing.widthPx = widthPx;
        if (existing.heightPx == 0) existing.heightPx = heightPx;
        return id;
    }

    if (mime.empty()) {
        if (const std::string_view sniffed = SniffImageMime(bytes); !sniffed.empty()) {
            mime.assign(sniffed);
        }
    }

    Resource resource;
    resource.id = id;
    resource.mime = std::move(mime);
    resource.hint = std::move(hint);
    resource.widthPx = widthPx;
    resource.heightPx = heightPx;
    resource.bytes.assign(bytes.begin(), bytes.end());

    resources_.emplace(id, std::move(resource));
    return id;
}

const Resource* ResourceStore::Find(std::string_view id) const {
    const auto it = resources_.find(id);
    return it == resources_.end() ? nullptr : &it->second;
}

std::uint64_t ResourceStore::TotalBytes() const noexcept {
    std::uint64_t total = 0;
    for (const auto& [id, resource] : resources_) total += resource.bytes.size();
    return total;
}

void ResourceStore::Retain(const std::vector<ResourceId>& referenced) {
    for (auto it = resources_.begin(); it != resources_.end();) {
        const bool keep = std::find(referenced.begin(), referenced.end(), it->first) !=
                          referenced.end();
        it = keep ? std::next(it) : resources_.erase(it);
    }
}

std::string_view ExtensionForMime(std::string_view mime) {
    if (mime == "image/png") return ".png";
    if (mime == "image/jpeg") return ".jpg";
    if (mime == "image/gif") return ".gif";
    if (mime == "image/webp") return ".webp";
    if (mime == "image/bmp") return ".bmp";
    if (mime == "image/tiff") return ".tiff";
    if (mime == "image/heif" || mime == "image/heic") return ".heic";
    if (mime == "image/avif") return ".avif";
    if (mime == "image/svg+xml") return ".svg";
    if (mime == "image/x-emf") return ".emf";
    if (mime == "image/x-wmf") return ".wmf";
    if (mime == "font/woff2") return ".woff2";
    if (mime == "font/woff") return ".woff";
    if (mime == "font/ttf") return ".ttf";
    if (mime == "font/otf") return ".otf";
    return ".bin";  // never guess from a filename
}

std::string_view SniffImageMime(std::span<const std::byte> bytes) {
    if (StartsWith(bytes, {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A})) return "image/png";
    if (StartsWith(bytes, {0xFF, 0xD8, 0xFF})) return "image/jpeg";
    if (StartsWith(bytes, {'G', 'I', 'F', '8'})) return "image/gif";
    if (StartsWith(bytes, {'B', 'M'})) return "image/bmp";
    if (StartsWith(bytes, {0x49, 0x49, 0x2A, 0x00}) ||
        StartsWith(bytes, {0x4D, 0x4D, 0x00, 0x2A})) {
        return "image/tiff";
    }
    if (StartsWith(bytes, {0x01, 0x00, 0x00, 0x00})) return "image/x-emf";
    if (StartsWith(bytes, {0xD7, 0xCD, 0xC6, 0x9A})) return "image/x-wmf";

    // RIFF and ISO-BMFF containers both need a second look at the brand.
    if (bytes.size() >= 12 && StartsWith(bytes, {'R', 'I', 'F', 'F'})) {
        const auto* p = reinterpret_cast<const char*>(bytes.data());
        if (std::memcmp(p + 8, "WEBP", 4) == 0) return "image/webp";
    }
    if (bytes.size() >= 12) {
        const auto* p = reinterpret_cast<const char*>(bytes.data());
        if (std::memcmp(p + 4, "ftyp", 4) == 0) {
            const std::string_view brand(p + 8, 4);
            if (brand == "avif" || brand == "avis") return "image/avif";
            if (brand == "heic" || brand == "heix" || brand == "mif1" || brand == "msf1") {
                return "image/heif";
            }
        }
    }
    return {};
}

PixelSize SniffImageSize(std::span<const std::byte> bytes) {
    const std::string_view mime = SniffImageMime(bytes);

    if (mime == "image/png" && bytes.size() >= 24) {
        // IHDR is required to be the first chunk, at a fixed offset.
        return PixelSize{ReadBigEndian32(&bytes[16]), ReadBigEndian32(&bytes[20])};
    }
    if (mime == "image/gif" && bytes.size() >= 10) {
        return PixelSize{
            static_cast<std::uint32_t>(static_cast<std::uint32_t>(bytes[6]) |
                                       (static_cast<std::uint32_t>(bytes[7]) << 8)),
            static_cast<std::uint32_t>(static_cast<std::uint32_t>(bytes[8]) |
                                       (static_cast<std::uint32_t>(bytes[9]) << 8))};
    }
    if (mime == "image/bmp" && bytes.size() >= 26) {
        return PixelSize{ReadLittleEndian32(&bytes[18]), ReadLittleEndian32(&bytes[22])};
    }
    if (mime == "image/jpeg") return SniffJpegSize(bytes);

    if (mime == "image/webp" && bytes.size() >= 30) {
        const auto* p = reinterpret_cast<const char*>(bytes.data());
        if (std::memcmp(p + 12, "VP8X", 4) == 0) {
            // 24-bit little-endian, stored as size minus one.
            const auto w = static_cast<std::uint32_t>(bytes[24]) |
                           (static_cast<std::uint32_t>(bytes[25]) << 8) |
                           (static_cast<std::uint32_t>(bytes[26]) << 16);
            const auto h = static_cast<std::uint32_t>(bytes[27]) |
                           (static_cast<std::uint32_t>(bytes[28]) << 8) |
                           (static_cast<std::uint32_t>(bytes[29]) << 16);
            return PixelSize{w + 1, h + 1};
        }
        if (std::memcmp(p + 12, "VP8 ", 4) == 0 && bytes.size() >= 30) {
            const auto w = (static_cast<std::uint32_t>(bytes[26]) |
                            (static_cast<std::uint32_t>(bytes[27]) << 8)) & 0x3FFFu;
            const auto h = (static_cast<std::uint32_t>(bytes[28]) |
                            (static_cast<std::uint32_t>(bytes[29]) << 8)) & 0x3FFFu;
            return PixelSize{w, h};
        }
    }

    // HEIF, AVIF and TIFF need real container parsing to answer this. That is
    // the decoder's job, not a sniffer's: returning {0,0} means "ask WIC",
    // which is exactly right.
    return {};
}

}  // namespace morph::doc
