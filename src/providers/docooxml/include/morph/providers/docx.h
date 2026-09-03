// Morph — the DOCX provider (architecture section 8.4).
//
// "This section is the project. Images are an afternoon with WIC; video is a
//  week of plumbing around FFmpeg. Documents are three months, and they are the
//  reason anyone would choose Morph over the other three hundred converters."
//
// SCOPE, held deliberately (section 8.4's scope-discipline note, reinforced by
// section 20's "DOCX reader scope creep -- the single most likely thing to stall
// this project"):
//
//   P1  paragraphs, runs, headings, bold/italic/underline, hyperlinks, images
//   P2  lists and tables
//   P3  footnotes, headers/footers, section properties      NOT IMPLEMENTED
//   P4  tracked changes and comments                        NOT IMPLEMENTED
//
// P1 and P2 are implemented. Anything above them is refused no matter how small
// it looks; the corpus of real-world documents P1+P2 handles correctly is far
// larger than it sounds.
#pragma once

#include <string>
#include <string_view>

#include "morph/core/doc/pipeline.h"

namespace morph::docooxml {

// --- Free functions, for tests and for the CLI ------------------------------

// Parses a .docx package into the IR.
[[nodiscard]] Result ReadDocx(std::span<const std::byte> package, const OptionBag& options,
                              doc::Document& into);

// Serialises the IR as a .docx package. Byte-reproducible for a given input.
[[nodiscard]] Result WriteDocx(const doc::Document& document, const OptionBag& options,
                               std::vector<std::byte>& into);

// --- Provider ---------------------------------------------------------------

class DocxReader final : public doc::IDocumentReader {
public:
    [[nodiscard]] std::string_view Id() const noexcept override { return "docooxml"; }
    [[nodiscard]] std::vector<MediaProfile> Reads() const override;
    [[nodiscard]] EdgeCost ReadCost(const MediaProfile& from) const override;
    [[nodiscard]] std::optional<MediaProfile> Probe(ISource& source,
                                                    const PathHint& hint) const override;
    [[nodiscard]] Result Read(ConvertContext& context, ISource& source, const MediaProfile& from,
                              doc::Document& into) override;
};

class DocxWriter final : public doc::IDocumentWriter {
public:
    [[nodiscard]] std::string_view Id() const noexcept override { return "docooxml"; }
    [[nodiscard]] std::vector<MediaProfile> Writes() const override;
    [[nodiscard]] EdgeCost WriteCost(const MediaProfile& to) const override;
    [[nodiscard]] Result Write(ConvertContext& context, const doc::Document& document,
                               const MediaProfile& to, ISink& sink) override;
};

inline constexpr std::string_view kDocxProfileId = "application/docx";

}  // namespace morph::docooxml
