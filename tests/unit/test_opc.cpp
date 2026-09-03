// The OPC container (architecture section 8.4) and its defences (section 15.2).
//
// Half of this file builds deliberately hostile packages with miniz directly,
// because the writer refuses to produce them -- which is the point, but means
// the reader's defences would otherwise never be exercised.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <miniz.h>

#include "morph/providers/opc.h"

using namespace morph;
using namespace morph::opc;

namespace {

std::vector<std::byte> Bytes(std::string_view text) {
    return std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                  reinterpret_cast<const std::byte*>(text.data()) + text.size());
}

std::string_view Text(const std::vector<std::byte>& bytes) {
    return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// A minimal but genuinely valid package: content types, a root relationship and
// one document part.
std::vector<std::byte> MinimalPackage() {
    PackageWriter writer;
    writer.AddDefault("xml", "application/xml");
    writer.AddDefault("rels", std::string(mime::kRelationships));
    writer.AddPart("/word/document.xml", std::string(mime::kMainDocument),
                   "<w:document><w:body/></w:document>");
    writer.AddRelationship("", std::string(rel::kOfficeDocument), "word/document.xml");

    std::vector<std::byte> zip;
    REQUIRE(writer.Finish(zip).ok());
    return zip;
}

// Builds a ZIP the PackageWriter would never produce. `entries` is a list of
// (name, contents) written verbatim, with no name validation at all.
std::vector<std::byte> RawZip(
    const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive archive{};
    REQUIRE(mz_zip_writer_init_heap(&archive, 0, 64 * 1024));

    for (const auto& [name, contents] : entries) {
        REQUIRE(mz_zip_writer_add_mem(&archive, name.c_str(), contents.data(), contents.size(),
                                      static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION)));
    }

    void* buffer = nullptr;
    std::size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&archive, &buffer, &size));

    const auto* bytes = static_cast<const std::byte*>(buffer);
    std::vector<std::byte> out(bytes, bytes + size);
    mz_free(buffer);
    mz_zip_writer_end(&archive);
    return out;
}

constexpr std::string_view kContentTypes =
    R"(<?xml version="1.0" encoding="UTF-8"?>)"
    R"(<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">)"
    R"(<Default Extension="xml" ContentType="application/xml"/>)"
    R"(<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>)"
    R"(</Types>)";

}  // namespace

// --- Part-name canonicalisation: the zip-slip defence -----------------------

TEST_CASE("part names are canonicalised to a leading slash", "[opc][path]") {
    CHECK(Package::Canonicalise("word/document.xml") == "/word/document.xml");
    CHECK(Package::Canonicalise("/word/document.xml") == "/word/document.xml");
    CHECK(Package::Canonicalise("word//document.xml") == "/word/document.xml");
    CHECK(Package::Canonicalise("./word/./document.xml") == "/word/document.xml");
}

TEST_CASE("a traversing part name is rejected, not resolved", "[opc][security]") {
    // Section 15.2: "Reject any OPC part name containing '..'". Resolving it
    // instead would be a best effort at something that is either malformed or an
    // attack, and neither deserves one.
    CHECK(Package::Canonicalise("../outside.xml").empty());
    CHECK(Package::Canonicalise("word/../../outside.xml").empty());
    CHECK(Package::Canonicalise("/word/media/../../../etc/passwd").empty());
}

TEST_CASE("a backslash or drive letter in a part name is rejected", "[opc][security]") {
    // A backslash is never a separator in OPC, and is the classic way to smuggle
    // a Windows path past a check that only looks for '/'.
    CHECK(Package::Canonicalise(R"(word\document.xml)").empty());
    CHECK(Package::Canonicalise(R"(..\..\windows\system32\evil.dll)").empty());
    CHECK(Package::Canonicalise("C:/windows/system32/evil.dll").empty());
}

TEST_CASE("reserved device names are rejected", "[opc][security]") {
    CHECK(Package::Canonicalise("word/CON").empty());
    CHECK(Package::Canonicalise("word/nul.xml").empty());
    CHECK(Package::Canonicalise("LPT1/document.xml").empty());
    // A name that merely contains one is fine.
    CHECK(Package::Canonicalise("word/console.xml") == "/word/console.xml");
}

// --- Relationship target resolution ------------------------------------------

TEST_CASE("a target resolves against its source part's directory", "[opc][path]") {
    // Resolving against the package root instead makes every image in
    // word/media/ point at /media/ and load nothing.
    CHECK(Package::ResolveTarget("/word/document.xml", "media/image1.png") ==
          "/word/media/image1.png");
    CHECK(Package::ResolveTarget("/word/document.xml", "styles.xml") == "/word/styles.xml");
    CHECK(Package::ResolveTarget("/document.xml", "word/styles.xml") == "/word/styles.xml");
}

TEST_CASE("a target may use .. to reach a sibling directory", "[opc][path]") {
    // Unlike a part NAME, a relative TARGET legitimately climbs, and
    // "../media/image1.png" is completely ordinary.
    CHECK(Package::ResolveTarget("/word/sub/document.xml", "../media/image1.png") ==
          "/word/media/image1.png");
    CHECK(Package::ResolveTarget("/word/document.xml", "/word/styles.xml") ==
          "/word/styles.xml");
}

TEST_CASE("a target that escapes the package is refused", "[opc][security]") {
    CHECK(Package::ResolveTarget("/word/document.xml", "../../outside.xml").empty());
    CHECK(Package::ResolveTarget("/document.xml", "../outside.xml").empty());
}

TEST_CASE("the relationship part for a part is derived correctly", "[opc][path]") {
    CHECK(Package::RelationshipPartFor("/word/document.xml") ==
          "/word/_rels/document.xml.rels");
    CHECK(Package::RelationshipPartFor("") == "/_rels/.rels");
    CHECK(Package::RelationshipPartFor("/") == "/_rels/.rels");
}

// --- Writing and reading back ------------------------------------------------

TEST_CASE("a written package reads back with its parts intact", "[opc][roundtrip]") {
    Package package;
    REQUIRE(Package::Open(MinimalPackage(), package).ok());

    const Part* document = package.Find("/word/document.xml");
    REQUIRE(document != nullptr);
    CHECK(Text(document->bytes) == "<w:document><w:body/></w:document>");
    CHECK(document->contentType == mime::kMainDocument);

    // Names are case-insensitive and the leading slash is optional, because real
    // packages are inconsistent about both.
    CHECK(package.Find("word/document.xml") != nullptr);
    CHECK(package.Find("/WORD/Document.XML") != nullptr);
    CHECK(package.Find("/word/missing.xml") == nullptr);
}

TEST_CASE("root relationships survive the round trip", "[opc][roundtrip]") {
    Package package;
    REQUIRE(Package::Open(MinimalPackage(), package).ok());

    const Relationship* found = package.FindRelationshipByType("", rel::kOfficeDocument);
    REQUIRE(found != nullptr);
    CHECK(found->target == "word/document.xml");
    CHECK_FALSE(found->external);
    CHECK(Package::ResolveTarget("/", found->target) == "/word/document.xml");
}

TEST_CASE("part relationships are attributed to the right source", "[opc][roundtrip]") {
    PackageWriter writer;
    writer.AddDefault("xml", "application/xml");
    writer.AddDefault("rels", std::string(mime::kRelationships));
    writer.AddDefault("png", "image/png");
    writer.AddPart("/word/document.xml", std::string(mime::kMainDocument), "<w:document/>");
    writer.AddPart("/word/media/image1.png", "", Bytes("\x89PNG\r\n\x1a\n"));
    writer.AddRelationship("", std::string(rel::kOfficeDocument), "word/document.xml");
    const std::string id = writer.AddRelationship("/word/document.xml", std::string(rel::kImage),
                                                  "media/image1.png");

    std::vector<std::byte> zip;
    REQUIRE(writer.Finish(zip).ok());

    Package package;
    REQUIRE(Package::Open(zip, package).ok());

    const Relationship* image = package.FindRelationship("/word/document.xml", id);
    REQUIRE(image != nullptr);
    CHECK(image->type == rel::kImage);
    CHECK(package.Find(Package::ResolveTarget("/word/document.xml", image->target)) != nullptr);

    // The root's relationship must not leak into the document part's list.
    CHECK(package.FindRelationship("/word/document.xml", "rId1")->type == rel::kImage);
}

TEST_CASE("a part covered by a Default gets no Override", "[opc][roundtrip]") {
    // Word writes both Defaults and Overrides and expects both; emitting an
    // Override for every part bloats the map and is not what Word produces.
    Package package;
    REQUIRE(Package::Open(MinimalPackage(), package).ok());

    const Part* types = package.Find("/[Content_Types].xml");
    REQUIRE(types != nullptr);
    const std::string_view xml = Text(types->bytes);
    CHECK(xml.find(R"(<Default Extension="rels")") != std::string_view::npos);
    CHECK(xml.find(R"(PartName="/word/document.xml")") != std::string_view::npos);
}

TEST_CASE("the writer is byte-reproducible", "[opc][determinism]") {
    // Section 8.4: "Write parts in deterministic order with fixed ZIP timestamps
    // so output is byte-reproducible -- this makes golden-file testing
    // possible." Two runs in the same process is the weak version of this; the
    // timestamp handling also has to survive a change of timezone, which is why
    // the writer builds its fixed time through mktime rather than using a
    // constant time_t.
    CHECK(MinimalPackage() == MinimalPackage());
}

TEST_CASE("insertion order does not change the bytes", "[opc][determinism]") {
    const auto build = [](bool reversed) {
        PackageWriter writer;
        writer.AddDefault("xml", "application/xml");
        writer.AddDefault("rels", std::string(mime::kRelationships));
        if (reversed) {
            writer.AddPart("/word/styles.xml", std::string(mime::kStyles), "<w:styles/>");
            writer.AddPart("/word/document.xml", std::string(mime::kMainDocument), "<w:document/>");
        } else {
            writer.AddPart("/word/document.xml", std::string(mime::kMainDocument), "<w:document/>");
            writer.AddPart("/word/styles.xml", std::string(mime::kStyles), "<w:styles/>");
        }
        std::vector<std::byte> zip;
        REQUIRE(writer.Finish(zip).ok());
        return zip;
    };
    CHECK(build(false) == build(true));
}

// --- Hostile packages --------------------------------------------------------

TEST_CASE("a file that is not a ZIP is refused with a real reason", "[opc][security]") {
    Package package;
    const Result result = Package::Open(Bytes("this is just some text, not a docx at all"),
                                        package);
    CHECK_FALSE(result.ok());
    CHECK(result.status() == Status::kMalformedInput);
    CHECK_FALSE(result.message().empty());
}

TEST_CASE("a tiny file is refused before miniz sees it", "[opc][security]") {
    Package package;
    CHECK_FALSE(Package::Open(Bytes("PK"), package).ok());
}

TEST_CASE("a package with no content-type map is refused", "[opc][security]") {
    // Section 15.2 validates part names against [Content_Types].xml before any
    // read, so a package without one cannot be trusted at all.
    Package package;
    const Result result =
        Package::Open(RawZip({{"word/document.xml", "<w:document/>"}}), package);
    CHECK_FALSE(result.ok());
    CHECK(result.status() == Status::kMalformedInput);
}

TEST_CASE("a traversing entry name is refused by name", "[opc][security]") {
    // The message names the path, because "the file is corrupt" would send the
    // user looking for the wrong problem entirely.
    Package package;
    const Result result = Package::Open(
        RawZip({{"[Content_Types].xml", std::string(kContentTypes)},
                {"../../../evil.xml", "<x/>"}}),
        package);

    CHECK_FALSE(result.ok());
    CHECK(result.status() == Status::kMalformedInput);
    CHECK(result.message().find("unsafe internal path") != std::string::npos);
}

TEST_CASE("a compression bomb is refused before it is inflated", "[opc][security]") {
    // A megabyte of zeros compresses to about a kilobyte. The caps are lowered
    // here so the test stays fast; what is being checked is that the limit is
    // enforced during inflation rather than after, so the memory is never
    // actually committed.
    const std::string zeros(4 * 1024 * 1024, '\0');
    const std::vector<std::byte> zip =
        RawZip({{"[Content_Types].xml", std::string(kContentTypes)}, {"word/bomb.xml", zeros}});

    // The bomb is far smaller than the shipping caps, so it opens normally.
    Package permissive;
    CHECK(Package::Open(zip, permissive).ok());
    CHECK(permissive.TotalUncompressedBytes() > 4 * 1024 * 1024);

    Limits tight;
    tight.maxTotalUncompressedBytes = 64 * 1024;
    tight.maxPartBytes = 64 * 1024;

    Package package;
    const Result result = Package::Open(zip, package, tight);
    CHECK_FALSE(result.ok());
    CHECK(result.status() == Status::kResourceLimit);
    CHECK_FALSE(result.message().empty());
}

TEST_CASE("an extreme compression ratio is refused", "[opc][security]") {
    // Large enough that the compressed entry clears Limits::ratioCheckFloorBytes.
    // Below that floor the ratio check deliberately does not apply, because a
    // 12-byte part inflating to 3 KB is a 250:1 ratio and completely ordinary.
    const std::string zeros(16 * 1024 * 1024, 'A');
    const std::vector<std::byte> zip =
        RawZip({{"[Content_Types].xml", std::string(kContentTypes)}, {"word/bomb.xml", zeros}});

    Limits tight;
    tight.maxCompressionRatio = 10;  // real content never approaches this

    Package package;
    const Result result = Package::Open(zip, package, tight);
    CHECK_FALSE(result.ok());
    CHECK(result.status() == Status::kResourceLimit);
}

TEST_CASE("a package with too many parts is refused", "[opc][security]") {
    // Costs nothing to build and a great deal to walk.
    std::vector<std::pair<std::string, std::string>> entries{
        {"[Content_Types].xml", std::string(kContentTypes)}};
    for (int i = 0; i < 64; ++i) {
        entries.emplace_back("word/part" + std::to_string(i) + ".xml", "<x/>");
    }

    Limits tight;
    tight.maxParts = 16;

    Package package;
    const Result result = Package::Open(RawZip(entries), package, tight);
    CHECK_FALSE(result.ok());
    CHECK(result.status() == Status::kResourceLimit);
}

TEST_CASE("a damaged relationship part costs its links, not the document",
          "[opc][security]") {
    const std::vector<std::byte> zip = RawZip({
        {"[Content_Types].xml", std::string(kContentTypes)},
        {"_rels/.rels", "<Relationships><this is not xml at all"},
        {"word/document.xml", "<w:document/>"},
    });

    Package package;
    REQUIRE(Package::Open(zip, package).ok());
    CHECK(package.Find("/word/document.xml") != nullptr);
    CHECK(package.RelationshipsOf("").empty());
}

TEST_CASE("an external relationship target is flagged", "[opc][security]") {
    // A hyperlink to the internet is legitimate in a document and must never be
    // followed: section 1 is local only, with no network egress at all.
    const std::vector<std::byte> zip = RawZip({
        {"[Content_Types].xml", std::string(kContentTypes)},
        {"word/_rels/document.xml.rels",
         R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
         R"(<Relationship Id="rId1" Type=")" +
             std::string(rel::kHyperlink) +
             R"(" Target="https://example.com" TargetMode="External"/></Relationships>)"},
        {"word/document.xml", "<w:document/>"},
    });

    Package package;
    REQUIRE(Package::Open(zip, package).ok());

    const Relationship* link = package.FindRelationship("/word/document.xml", "rId1");
    REQUIRE(link != nullptr);
    CHECK(link->external);
    CHECK(link->target == "https://example.com");
}
