// Morph — the OPC container (architecture section 8.4).
//
// "A .docx is an OPC package: a ZIP containing XML parts joined by a
//  relationship graph."
//
// This layer knows nothing about WordprocessingML. It knows about ZIP entries,
// content types, relationships and part-name resolution -- and about the four
// ways a hostile package attacks us (section 15.2), which is most of why it is
// its own layer rather than fifty lines inside the DOCX reader:
//
//   ZIP BOMB. Cap total uncompressed size and the per-entry compression ratio.
//   Stream-inflate; never allocate based on the size declared in the header.
//
//   ZIP SLIP. Reject any part name containing "..", a drive letter, a backslash
//   or a leading slash. Part names are validated against [Content_Types].xml
//   before any read.
//
//   XXE / BILLION LAUGHS. Handled one layer up by pugixml, which does not expand
//   external entities by construction. This layer additionally caps part count.
//
//   RESOURCE EXHAUSTION. A package with a hundred thousand tiny parts costs
//   nothing to build and a great deal to walk.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "morph/core/result.h"

namespace morph::opc {

// Section 15.2's numbers, in one place so the fuzzer and the tests can point at
// them rather than repeating magic values.
struct Limits {
    // "Reject above 200:1 or 500 MB uncompressed, whichever first."
    std::uint64_t maxTotalUncompressedBytes = 500ull * 1024 * 1024;
    std::uint64_t maxCompressionRatio = 200;

    // A single part large enough to matter on its own.
    std::uint64_t maxPartBytes = 256ull * 1024 * 1024;

    // Walking cost, not memory cost. A real .docx has tens of parts.
    std::size_t maxParts = 10'000;

    // Below this, the ratio check is meaningless: a 12-byte part that inflates
    // to 3 KB is a 250:1 ratio and is completely ordinary.
    std::uint64_t ratioCheckFloorBytes = 4096;
};

struct Relationship {
    std::string id;      // "rId3"
    std::string type;    // the full relationship-type URI
    std::string target;  // as written in the .rels part, unresolved
    bool external = false;
};

struct Part {
    std::string name;         // canonical, with a leading slash: "/word/document.xml"
    std::string contentType;  // resolved from [Content_Types].xml
    std::vector<std::byte> bytes;
};

// A read-only OPC package. Everything is inflated up front, because a .docx is
// read in a random order dictated by its relationship graph and the ZIP central
// directory is at the end of the file anyway -- there is no useful streaming
// decomposition. The limits above are what keep that honest.
class Package {
public:
    Package() = default;

    // `zip` must remain valid only for the duration of the call.
    [[nodiscard]] static Result Open(std::span<const std::byte> zip, Package& into,
                                     const Limits& limits = {});

    // Part names are matched case-insensitively on the ASCII range and with or
    // without the leading slash, because real packages are inconsistent about
    // both and the specification says names are case-insensitive.
    [[nodiscard]] const Part* Find(std::string_view partName) const;
    [[nodiscard]] bool Contains(std::string_view partName) const { return Find(partName); }

    [[nodiscard]] const std::map<std::string, Part, std::less<>>& Parts() const noexcept {
        return parts_;
    }

    // Relationships declared by `sourcePart`. Pass "" or "/" for the package
    // root, whose relationships live in "/_rels/.rels".
    [[nodiscard]] const std::vector<Relationship>& RelationshipsOf(
        std::string_view sourcePart) const;

    [[nodiscard]] const Relationship* FindRelationship(std::string_view sourcePart,
                                                       std::string_view id) const;

    // The first relationship of a given type, which is how the main document
    // part and the styles part are located. Returns nullptr when absent.
    [[nodiscard]] const Relationship* FindRelationshipByType(std::string_view sourcePart,
                                                            std::string_view type) const;

    // Resolves a relationship target against the part that declared it, the way
    // OPC specifies. "media/image1.png" declared by "/word/document.xml" becomes
    // "/word/media/image1.png". Returns an empty string for a target that
    // escapes the package, which the caller must treat as a rejection.
    [[nodiscard]] static std::string ResolveTarget(std::string_view sourcePart,
                                                   std::string_view target);

    // The .rels part name for a given part: "/word/document.xml" ->
    // "/word/_rels/document.xml.rels".
    [[nodiscard]] static std::string RelationshipPartFor(std::string_view partName);

    // Canonical form: leading slash, forward slashes, no "." or ".." segments.
    // Returns an empty string for a name that is not safe to use, which is the
    // zip-slip rejection.
    [[nodiscard]] static std::string Canonicalise(std::string_view partName);

    [[nodiscard]] std::uint64_t TotalUncompressedBytes() const noexcept { return totalBytes_; }

private:
    [[nodiscard]] Result LoadContentTypes();
    [[nodiscard]] Result LoadRelationships();

    std::map<std::string, Part, std::less<>> parts_;
    std::map<std::string, std::string, std::less<>> defaultsByExtension_;
    std::map<std::string, std::string, std::less<>> overridesByPart_;
    std::map<std::string, std::vector<Relationship>, std::less<>> relationships_;
    std::uint64_t totalBytes_ = 0;
};

// Builds a package. Section 8.4: "Write parts in deterministic order with fixed
// ZIP timestamps so output is byte-reproducible -- this makes golden-file
// testing possible and is worth the small effort."
class PackageWriter {
public:
    // A Default maps a file extension to a content type; an Override names one
    // part explicitly. Word writes both and expects both.
    void AddDefault(std::string extension, std::string contentType);

    // `contentType` empty means "covered by a Default for this extension".
    void AddPart(std::string partName, std::string contentType, std::vector<std::byte> bytes);
    void AddPart(std::string partName, std::string contentType, std::string_view text);

    void AddRelationship(std::string sourcePart, Relationship relationship);

    // Convenience: adds the relationship with the next free rId for that source
    // and returns the id it chose.
    std::string AddRelationship(std::string sourcePart, std::string type, std::string target,
                                bool external = false);

    // Serialises [Content_Types].xml, every .rels part and every content part.
    [[nodiscard]] Result Finish(std::vector<std::byte>& into);

private:
    [[nodiscard]] std::string BuildContentTypes() const;
    [[nodiscard]] std::string BuildRelationships(const std::vector<Relationship>& list) const;

    std::map<std::string, Part, std::less<>> parts_;
    std::map<std::string, std::string, std::less<>> defaultsByExtension_;
    std::map<std::string, std::vector<Relationship>, std::less<>> relationships_;
};

// Relationship type URIs, which are long, easy to mistype, and silently produce
// a package Word refuses to open when wrong.
namespace rel {
inline constexpr std::string_view kOfficeDocument =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument";
inline constexpr std::string_view kStyles =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles";
inline constexpr std::string_view kNumbering =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering";
inline constexpr std::string_view kImage =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image";
inline constexpr std::string_view kHyperlink =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink";
inline constexpr std::string_view kFootnotes =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/footnotes";
inline constexpr std::string_view kEndnotes =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/endnotes";
inline constexpr std::string_view kCoreProperties =
    "http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties";
inline constexpr std::string_view kExtendedProperties =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties";
}  // namespace rel

namespace mime {
inline constexpr std::string_view kMainDocument =
    "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml";
inline constexpr std::string_view kStyles =
    "application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml";
inline constexpr std::string_view kNumbering =
    "application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml";
inline constexpr std::string_view kFootnotes =
    "application/vnd.openxmlformats-officedocument.wordprocessingml.footnotes+xml";
inline constexpr std::string_view kRelationships =
    "application/vnd.openxmlformats-package.relationships+xml";
inline constexpr std::string_view kCoreProperties =
    "application/vnd.openxmlformats-package.core-properties+xml";
inline constexpr std::string_view kExtendedProperties =
    "application/vnd.openxmlformats-officedocument.extended-properties+xml";
}  // namespace mime

}  // namespace morph::opc
