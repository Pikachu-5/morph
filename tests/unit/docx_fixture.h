// Builds real .docx packages from fragments of WordprocessingML, so the DOCX
// tests exercise the actual OPC layer and the actual reader rather than a
// convenient shortcut past both.
#pragma once

#include <string>
#include <vector>

#include "morph/providers/opc.h"

namespace morph::test {

struct DocxParts {
    // Goes inside <w:body>.
    std::string body;

    // Goes inside <w:styles>. Empty means no styles part at all, which is legal
    // and common.
    std::string styles;

    // Goes inside <w:numbering>.
    std::string numbering;

    // Goes inside <cp:coreProperties>.
    std::string coreProperties;

    // partName -> bytes, added under /word/ and related from document.xml with
    // the given relationship id.
    struct Media {
        std::string relationshipId;
        std::string fileName;  // "image1.png"
        std::string contentType;
        std::vector<std::byte> bytes;
    };
    std::vector<Media> media;

    // Extra relationships from document.xml, e.g. hyperlinks.
    std::vector<opc::Relationship> documentRelationships;
};

inline std::vector<std::byte> BuildDocx(const DocxParts& parts) {
    opc::PackageWriter writer;
    writer.AddDefault("rels", std::string(opc::mime::kRelationships));
    writer.AddDefault("xml", "application/xml");
    writer.AddDefault("png", "image/png");
    writer.AddDefault("jpeg", "image/jpeg");

    const std::string kNamespaces =
        R"( xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main")"
        R"( xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships")"
        R"( xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing")"
        R"( xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main")"
        R"( xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture")";

    writer.AddPart("/word/document.xml", std::string(opc::mime::kMainDocument),
                   R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:document)" +
                       kNamespaces + "><w:body>" + parts.body + "</w:body></w:document>");
    writer.AddRelationship("", std::string(opc::rel::kOfficeDocument), "word/document.xml");

    if (!parts.styles.empty()) {
        writer.AddPart("/word/styles.xml", std::string(opc::mime::kStyles),
                       R"(<?xml version="1.0" encoding="UTF-8"?><w:styles)" + kNamespaces + ">" +
                           parts.styles + "</w:styles>");
        writer.AddRelationship("/word/document.xml",
                               opc::Relationship{"rIdStyles", std::string(opc::rel::kStyles),
                                                 "styles.xml", false});
    }

    if (!parts.numbering.empty()) {
        writer.AddPart("/word/numbering.xml", std::string(opc::mime::kNumbering),
                       R"(<?xml version="1.0" encoding="UTF-8"?><w:numbering)" + kNamespaces +
                           ">" + parts.numbering + "</w:numbering>");
        writer.AddRelationship("/word/document.xml",
                               opc::Relationship{"rIdNum", std::string(opc::rel::kNumbering),
                                                 "numbering.xml", false});
    }

    if (!parts.coreProperties.empty()) {
        writer.AddPart(
            "/docProps/core.xml", std::string(opc::mime::kCoreProperties),
            R"(<?xml version="1.0" encoding="UTF-8"?><cp:coreProperties)"
            R"( xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties")"
            R"( xmlns:dc="http://purl.org/dc/elements/1.1/")"
            R"( xmlns:dcterms="http://purl.org/dc/terms/">)" +
                parts.coreProperties + "</cp:coreProperties>");
        writer.AddRelationship("", opc::Relationship{"rIdCore",
                                                     std::string(opc::rel::kCoreProperties),
                                                     "docProps/core.xml", false});
    }

    for (const DocxParts::Media& media : parts.media) {
        writer.AddPart("/word/media/" + media.fileName, media.contentType, media.bytes);
        writer.AddRelationship("/word/document.xml",
                               opc::Relationship{media.relationshipId,
                                                 std::string(opc::rel::kImage),
                                                 "media/" + media.fileName, false});
    }

    for (const opc::Relationship& relationship : parts.documentRelationships) {
        writer.AddRelationship("/word/document.xml", relationship);
    }

    std::vector<std::byte> zip;
    const Result finished = writer.Finish(zip);
    if (!finished) return {};
    return zip;
}

// A single paragraph of unstyled text, which most tests only need as scenery.
inline std::string Para(std::string_view text) {
    return "<w:p><w:r><w:t>" + std::string(text) + "</w:t></w:r></w:p>";
}

}  // namespace morph::test
