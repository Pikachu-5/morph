// Morph — the resource store (architecture section 8.2).
//
// "ResourceStore res;  // images/fonts, content-addressed by BLAKE3"
//
// Content addressing is not a nicety here. A DOCX with the same logo in a header
// on forty pages has forty relationship targets pointing at one part; a naive
// reader emits forty copies into the IR and a naive writer emits forty files.
// Hashing the bytes collapses that on the way in, and section 8.4's
// "images -> ResourceStore, deduplicated by content hash" is the requirement.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace morph::doc {

// The lowercase hex BLAKE3 digest of the resource's bytes, truncated to 32 hex
// characters (128 bits). Long enough that a collision is not a thing that
// happens; short enough to be a readable filename in an assets directory.
using ResourceId = std::string;

struct Resource {
    ResourceId id;
    std::string mime;              // "image/png", "font/woff2"
    std::vector<std::byte> bytes;

    // Intrinsic pixel size for raster images, when the reader could determine it
    // cheaply from the header. Writers need it to emit sensible width/height and
    // the PDF stage needs it to avoid reflowing.
    std::uint32_t widthPx = 0;
    std::uint32_t heightPx = 0;

    // A filename suggestion carried through from the source package, used only
    // when writing an assets directory. Never trusted as a path: section 15.2
    // rejects traversal in OPC part names, and this is where such a name would
    // otherwise leak back out.
    std::string hint;
};

class ResourceStore {
public:
    // Hashes, deduplicates, and returns the id. Adding the same bytes twice
    // returns the same id and stores one copy; metadata from the first insert
    // wins, because a second caller with worse metadata should not degrade it.
    ResourceId Add(std::span<const std::byte> bytes, std::string mime, std::string hint = {});

    // As above, with intrinsic dimensions the caller already knows.
    ResourceId Add(std::span<const std::byte> bytes, std::string mime, std::string hint,
                   std::uint32_t widthPx, std::uint32_t heightPx);

    [[nodiscard]] const Resource* Find(std::string_view id) const;
    [[nodiscard]] bool Contains(std::string_view id) const { return Find(id) != nullptr; }

    [[nodiscard]] std::size_t Size() const noexcept { return resources_.size(); }
    [[nodiscard]] bool Empty() const noexcept { return resources_.empty(); }

    [[nodiscard]] const std::map<ResourceId, Resource, std::less<>>& All() const noexcept {
        return resources_;
    }

    // Total bytes held. The worker's memory budget is derived from the input
    // size (section 11.2), and a document's resources are usually most of it.
    [[nodiscard]] std::uint64_t TotalBytes() const noexcept;

    // Drops resources nothing refers to. Called by writers that prune content --
    // a Markdown writer that discards a header image should not leave it in an
    // assets directory.
    void Retain(const std::vector<ResourceId>& referenced);

private:
    std::map<ResourceId, Resource, std::less<>> resources_;
};

// Lowercase hex BLAKE3, truncated to `hexChars`. Exposed because the OOXML
// writer also uses it for deterministic part naming (section 8.4: "Write parts
// in deterministic order with fixed ZIP timestamps so output is byte
// reproducible").
[[nodiscard]] std::string ContentHash(std::span<const std::byte> bytes,
                                      std::size_t hexChars = 32);

// The file extension conventionally used for a MIME type, with the dot.
// Falls back to ".bin" rather than guessing.
[[nodiscard]] std::string_view ExtensionForMime(std::string_view mime);

// The MIME type for a raster image, sniffed from its magic bytes. Empty when
// unrecognised. Never consults a filename: section 15 treats every input as
// hostile and an attacker controls the extension.
[[nodiscard]] std::string_view SniffImageMime(std::span<const std::byte> bytes);

// Intrinsic pixel dimensions read from an image header, without decoding the
// image. Returns {0,0} when unavailable. Bounded work: reads a fixed prefix and
// gives up rather than walking an arbitrary chunk chain.
struct PixelSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
[[nodiscard]] PixelSize SniffImageSize(std::span<const std::byte> bytes);

}  // namespace morph::doc
