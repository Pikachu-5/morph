// Morph — the Markdown provider (architecture section 8.3).
//
// Reader: md4c (MIT, C, ~5x faster than cmark, CommonMark 0.30 + GFM tables,
// strikethrough, task lists, autolinks). It is a SAX-style callback parser, so
// the reader is a small state machine that builds IR nodes directly with no
// intermediate AST allocation.
//
// Writer: ours, with a flavour switch. The writer's real work is escaping and
// reference management -- get the escaping wrong and you produce Markdown that
// does not round-trip, which is the one thing a converter must never do.
#pragma once

#include <string>
#include <string_view>

#include "morph/core/doc/pipeline.h"

namespace morph::docmd {

// Section 8.3's flavour switch.
enum class Flavour {
    // CommonMark 0.30 only. No tables, no strikethrough, no task lists.
    kCommonMark,

    // CommonMark plus the GFM extensions md4c supports. The default: it is what
    // GitHub, VS Code and every static site generator expect.
    kGitHub,

    // Avoids constructs many renderers get wrong: no setext headings, no
    // reference links, asterisk bullets only, fenced code always, and tables
    // padded to align. Wider output, but it renders the same everywhere.
    kPortable,
};

[[nodiscard]] Flavour FlavourFromString(std::string_view name);
[[nodiscard]] std::string_view ToString(Flavour flavour);

// --- Free functions, for tests and for the CLI ------------------------------

// Parses UTF-8 Markdown into the IR. Never fails on malformed input: Markdown
// has no parse errors by design, every byte sequence is a valid document. It
// can still fail on resource limits and on cancellation.
[[nodiscard]] Result ReadMarkdown(std::string_view text, const OptionBag& options,
                                  doc::Document& into);

// Serialises the IR as Markdown. Appends fidelity notes for everything the
// flavour cannot express.
[[nodiscard]] Result WriteMarkdown(const doc::Document& document, const OptionBag& options,
                                   std::string& into);

// Escapes a literal text run so that it survives a round trip. Exposed because
// it is the single most correctness-critical function in the writer and it
// deserves to be tested directly rather than only through whole documents.
//
// `atLineStart` matters: '#' is only a heading marker in the first column, and
// over-escaping mid-line produces ugly output for no benefit.
[[nodiscard]] std::string EscapeText(std::string_view text, bool atLineStart, Flavour flavour);

// --- Provider ---------------------------------------------------------------

class MarkdownReader final : public doc::IDocumentReader {
public:
    [[nodiscard]] std::string_view Id() const noexcept override { return "docmd"; }
    [[nodiscard]] std::vector<MediaProfile> Reads() const override;
    [[nodiscard]] EdgeCost ReadCost(const MediaProfile& from) const override;
    [[nodiscard]] std::optional<MediaProfile> Probe(ISource& source,
                                                    const PathHint& hint) const override;
    [[nodiscard]] Result Read(ConvertContext& context, ISource& source, const MediaProfile& from,
                              doc::Document& into) override;
};

class MarkdownWriter final : public doc::IDocumentWriter {
public:
    [[nodiscard]] std::string_view Id() const noexcept override { return "docmd"; }
    [[nodiscard]] std::vector<MediaProfile> Writes() const override;
    [[nodiscard]] EdgeCost WriteCost(const MediaProfile& to) const override;
    [[nodiscard]] Result Write(ConvertContext& context, const doc::Document& document,
                               const MediaProfile& to, ISink& sink) override;
};

// The profile id every Markdown edge uses.
inline constexpr std::string_view kMarkdownProfileId = "text/markdown";

}  // namespace morph::docmd
