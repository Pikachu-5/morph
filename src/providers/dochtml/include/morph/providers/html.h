// Morph — the HTML provider (architecture section 8.5).
//
// Reader: lexbor (Apache-2.0), a genuinely spec-compliant HTML5 tokenizer and
// tree builder in C. That matters because real-world HTML is malformed and a
// naive parser will disagree with browsers about where elements nest. Its DOM is
// mapped to the IR through a SEMANTIC WHITELIST: presentational markup is
// ignored rather than interpreted.
//
// Writer: ours. Semantic HTML5 plus a small embedded CSS theme. The writer's
// output is also the input to the PDF stage (section 8.6), so it must be
// deterministic and must not depend on network fonts -- a PDF whose typography
// changes depending on whether the machine could reach a font CDN is not a
// conversion, it is a lottery. The WebView2 host additionally blocks all
// non-data:/file: navigation (section 15.1), so a network font would not load
// even if we asked for one.
#pragma once

#include <string>
#include <string_view>

#include "morph/core/doc/pipeline.h"

namespace morph::dochtml {

// --- Free functions, for tests and for the PDF stage ------------------------

// Serialises the IR as a complete HTML5 document. Images are inlined as data
// URIs, so the result is a single self-contained file with no external
// references -- which is exactly what PrintToPdfStream needs.
[[nodiscard]] Result WriteHtml(const doc::Document& document, const OptionBag& options,
                               std::string& into);

// Parses HTML into the IR.
[[nodiscard]] Result ReadHtml(std::string_view html, const OptionBag& options,
                              doc::Document& into);

// Escapes text for an HTML text node, or for a double-quoted attribute value.
// Exposed for the same reason Markdown's EscapeText is: it is small, it is
// critical, and it deserves direct tests.
[[nodiscard]] std::string EscapeHtmlText(std::string_view text);
[[nodiscard]] std::string EscapeHtmlAttribute(std::string_view value);

// The stylesheet the writer embeds. Exposed so the PDF stage can assert that its
// page geometry and this theme's margins do not fight each other.
[[nodiscard]] std::string_view DefaultStylesheet();

// --- Provider ---------------------------------------------------------------

class HtmlReader final : public doc::IDocumentReader {
public:
    [[nodiscard]] std::string_view Id() const noexcept override { return "dochtml"; }
    [[nodiscard]] std::vector<MediaProfile> Reads() const override;
    [[nodiscard]] EdgeCost ReadCost(const MediaProfile& from) const override;
    [[nodiscard]] std::optional<MediaProfile> Probe(ISource& source,
                                                    const PathHint& hint) const override;
    [[nodiscard]] Result Read(ConvertContext& context, ISource& source, const MediaProfile& from,
                              doc::Document& into) override;
};

class HtmlWriter final : public doc::IDocumentWriter {
public:
    [[nodiscard]] std::string_view Id() const noexcept override { return "dochtml"; }
    [[nodiscard]] std::vector<MediaProfile> Writes() const override;
    [[nodiscard]] EdgeCost WriteCost(const MediaProfile& to) const override;
    [[nodiscard]] Result Write(ConvertContext& context, const doc::Document& document,
                               const MediaProfile& to, ISink& sink) override;
};

inline constexpr std::string_view kHtmlProfileId = "text/html";

}  // namespace morph::dochtml
