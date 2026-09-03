// Morph — resolved document styles (architecture section 8.2, design rule 2).
//
// "Styles resolved on read. DOCX style inheritance (docDefaults -> style chain
//  via basedOn -> direct rPr/pPr) is flattened into concrete values when
//  reading. Writers never walk an inheritance chain. This eliminates an entire
//  category of bugs."
//
// So every type here holds concrete values. An unset optional means "the
// producing format did not specify this", not "inherit from somewhere" -- there
// is nowhere left to inherit from by the time a writer sees it.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace morph::doc {

struct Colour {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;

    friend bool operator==(const Colour&, const Colour&) noexcept = default;

    // "#rrggbb", or "#rrggbbaa" when not fully opaque.
    [[nodiscard]] std::string ToHex() const;

    // Accepts "#rgb", "#rrggbb", "#rrggbbaa" and bare forms without the hash,
    // which is what OOXML writes. nullopt on anything else -- notably on
    // OOXML's "auto", which genuinely means "unset" and must stay unset.
    [[nodiscard]] static std::optional<Colour> Parse(std::string_view text);
};

enum class Alignment : std::uint8_t { kStart, kCenter, kEnd, kJustify };

enum class VerticalAlign : std::uint8_t { kBaseline, kSuperscript, kSubscript };

enum class UnderlineStyle : std::uint8_t { kNone, kSingle, kDouble, kDotted, kDashed, kWavy };

// Character-level formatting that has no semantic inline node of its own.
//
// Bold, italic, strikethrough and code are deliberately NOT here: they are
// Strong, Emphasis, Strikethrough and InlineCode nodes, because every target
// format has a semantic construct for them and a Markdown writer should emit
// **bold**, not a span carrying a weight. What lands here is the presentational
// remainder that only DOCX and HTML can express.
struct TextStyle {
    std::optional<std::string> fontFamily;
    std::optional<float> fontSizePt;
    std::optional<Colour> colour;
    std::optional<Colour> highlight;
    std::optional<UnderlineStyle> underline;
    std::optional<VerticalAlign> vertical;
    std::optional<bool> smallCaps;
    std::optional<float> letterSpacingPt;

    [[nodiscard]] bool Empty() const noexcept;
    friend bool operator==(const TextStyle&, const TextStyle&) noexcept = default;

    // Values already set here win; `under` supplies the rest. This is the single
    // operation the readers use to collapse an inheritance chain: apply
    // docDefaults, then each basedOn ancestor from the root down, then direct
    // formatting last.
    void Underlay(const TextStyle& under);
};

struct ParagraphStyle {
    std::optional<Alignment> alignment;
    std::optional<float> spaceBeforePt;
    std::optional<float> spaceAfterPt;
    std::optional<float> lineHeight;  // multiple of the font size, not points
    std::optional<float> indentLeftPt;
    std::optional<float> indentRightPt;
    std::optional<float> indentFirstLinePt;
    std::optional<bool> keepWithNext;

    // The run formatting a paragraph imposes on its runs by default.
    TextStyle text;

    [[nodiscard]] bool Empty() const noexcept;
    friend bool operator==(const ParagraphStyle&, const ParagraphStyle&) noexcept = default;

    void Underlay(const ParagraphStyle& under);
};

// A named style, already flattened. `name` is the human-facing name ("Heading
// 1"); the map key is the format's internal id ("Heading1"), because that is
// what paragraphs reference and what a DOCX round-trip must reproduce.
struct NamedStyle {
    std::string name;
    ParagraphStyle paragraph;

    // Set when the style means "this is a heading at level N". Readers detect it
    // once, here, so that neither the writers nor the semantic mapping have to
    // pattern-match on style names.
    std::optional<int> headingLevel;
};

class StyleTable {
public:
    void Set(std::string id, NamedStyle style);

    [[nodiscard]] const NamedStyle* Find(std::string_view id) const;
    [[nodiscard]] bool Empty() const noexcept { return styles_.empty(); }
    [[nodiscard]] std::size_t Size() const noexcept { return styles_.size(); }

    [[nodiscard]] const std::map<std::string, NamedStyle, std::less<>>& All() const noexcept {
        return styles_;
    }

    // The style applied to a paragraph that names none. Already flattened from
    // docDefaults, so a writer that wants "what does normal text look like"
    // asks here rather than reconstructing it.
    [[nodiscard]] const ParagraphStyle& Default() const noexcept { return default_; }
    void SetDefault(ParagraphStyle style) { default_ = std::move(style); }

private:
    std::map<std::string, NamedStyle, std::less<>> styles_;
    ParagraphStyle default_;
};

}  // namespace morph::doc
