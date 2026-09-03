#include "docx_styles.h"

#include <algorithm>
#include <cctype>

#include "docx_xml.h"

namespace morph::docooxml {
namespace {

template <typename T>
void UnderlayField(std::optional<T>& over, const std::optional<T>& under) {
    if (!over.has_value()) over = under;
}

std::string ToLowerAscii(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::optional<doc::UnderlineStyle> ParseUnderline(const pugi::xml_node& rPr) {
    const pugi::xml_node u = Child(rPr, "u");
    if (!u) return std::nullopt;

    const std::string value = Attr(u, "val");
    // "none" is an explicit instruction to switch underlining OFF, which is
    // different from the element being absent. Both end up as "no underline" in
    // the IR, but only this one overrides an inherited underline, so it must be
    // a value rather than a nullopt.
    if (value == "none") return doc::UnderlineStyle::kNone;
    if (value.empty() || value == "single" || value == "words") {
        return doc::UnderlineStyle::kSingle;
    }
    if (value == "double") return doc::UnderlineStyle::kDouble;
    if (value == "dotted" || value == "dottedHeavy") return doc::UnderlineStyle::kDotted;
    if (value == "dash" || value == "dashed" || value == "dashLong") {
        return doc::UnderlineStyle::kDashed;
    }
    if (value == "wave" || value == "wavyDouble" || value == "wavyHeavy") {
        return doc::UnderlineStyle::kWavy;
    }
    // Any of the two dozen remaining variants: it is underlined, and which
    // flourish it uses is not something the IR or any target format expresses.
    return doc::UnderlineStyle::kSingle;
}

std::optional<doc::Alignment> ParseAlignment(const pugi::xml_node& pPr) {
    const pugi::xml_node jc = Child(pPr, "jc");
    if (!jc) return std::nullopt;

    const std::string value = Attr(jc, "val");
    if (value == "left" || value == "start") return doc::Alignment::kStart;
    if (value == "center" || value == "centre") return doc::Alignment::kCenter;
    if (value == "right" || value == "end") return doc::Alignment::kEnd;
    if (value == "both" || value == "distribute" || value == "justify") {
        return doc::Alignment::kJustify;
    }
    return std::nullopt;
}

// "heading 1" or "Heading1" or "heading-1" -> 1. Returns nullopt for anything
// else, including "Heading" with no number and "Heading 7", which has no IR
// node because the IR follows HTML's six levels.
std::optional<int> HeadingLevelFromName(std::string_view name) {
    const std::string lowered = ToLowerAscii(name);
    const auto at = lowered.find("heading");
    if (at == std::string::npos) return std::nullopt;

    std::size_t digit = at + 7;
    while (digit < lowered.size() &&
           (lowered[digit] == ' ' || lowered[digit] == '-' || lowered[digit] == '_')) {
        ++digit;
    }
    if (digit >= lowered.size() || lowered[digit] < '1' || lowered[digit] > '9') {
        return std::nullopt;
    }
    // Two digits would be "heading 10", which does not exist.
    if (digit + 1 < lowered.size() && std::isdigit(static_cast<unsigned char>(lowered[digit + 1]))) {
        return std::nullopt;
    }
    const int level = lowered[digit] - '0';
    return level <= 6 ? std::optional<int>(level) : std::nullopt;
}

}  // namespace

void RunFormat::Underlay(const RunFormat& under) {
    UnderlayField(bold, under.bold);
    UnderlayField(italic, under.italic);
    UnderlayField(strike, under.strike);
    style.Underlay(under.style);
}

bool RunFormat::SameAs(const RunFormat& other) const {
    return bold == other.bold && italic == other.italic && strike == other.strike &&
           style == other.style;
}

RunFormat ParseRunProperties(const pugi::xml_node& rPr) {
    RunFormat format;
    if (!rPr) return format;

    format.bold = OnOff(rPr, "b");
    format.italic = OnOff(rPr, "i");
    format.strike = OnOff(rPr, "strike");
    // Double strikethrough is still strikethrough as far as any target format is
    // concerned.
    if (!format.strike) format.strike = OnOff(rPr, "dstrike");

    format.style.underline = ParseUnderline(rPr);
    format.style.smallCaps = OnOff(rPr, "smallCaps");

    if (const pugi::xml_node sz = Child(rPr, "sz")) {
        // HALF-points. w:sz val="22" is 11pt, and reading it as 22 is the single
        // most common OOXML unit mistake.
        if (const auto half = IntAttr(sz, "val"); half && *half > 0 && *half < 32000) {
            format.style.fontSizePt = HalfPointsToPoints(*half);
        }
    }

    if (const pugi::xml_node colour = Child(rPr, "color")) {
        // Colour::Parse returns nullopt for "auto", which genuinely means
        // unspecified. Assigning it here keeps that distinction.
        format.style.colour = doc::Colour::Parse(Attr(colour, "val"));
    }

    if (const pugi::xml_node highlight = Child(rPr, "highlight")) {
        // w:highlight is a named colour from a fixed palette, not a hex value.
        static const std::map<std::string, doc::Colour, std::less<>> kPalette = {
            {"yellow", {0xFF, 0xFF, 0x00}},   {"green", {0x00, 0xFF, 0x00}},
            {"cyan", {0x00, 0xFF, 0xFF}},     {"magenta", {0xFF, 0x00, 0xFF}},
            {"blue", {0x00, 0x00, 0xFF}},     {"red", {0xFF, 0x00, 0x00}},
            {"darkBlue", {0x00, 0x00, 0x8B}}, {"darkCyan", {0x00, 0x8B, 0x8B}},
            {"darkGreen", {0x00, 0x64, 0x00}},{"darkMagenta", {0x8B, 0x00, 0x8B}},
            {"darkRed", {0x8B, 0x00, 0x00}},  {"darkYellow", {0x80, 0x80, 0x00}},
            {"darkGray", {0xA9, 0xA9, 0xA9}}, {"lightGray", {0xD3, 0xD3, 0xD3}},
            {"black", {0x00, 0x00, 0x00}},    {"white", {0xFF, 0xFF, 0xFF}},
        };
        const std::string value = Attr(highlight, "val");
        if (const auto it = kPalette.find(value); it != kPalette.end()) {
            format.style.highlight = it->second;
        }
        // "none" and anything unrecognised leave it unset, which is right: a
        // highlight we cannot name is not a highlight we can reproduce.
    }

    if (const pugi::xml_node fonts = Child(rPr, "rFonts")) {
        // w:ascii is the one that matters for Latin text. w:hAnsi, w:cs and
        // w:eastAsia describe the same run in other scripts, and the IR has one
        // font slot, so preferring ascii is the honest simplification.
        std::string family = Attr(fonts, "ascii");
        if (family.empty()) family = Attr(fonts, "hAnsi");
        if (family.empty()) family = Attr(fonts, "cs");
        if (!family.empty()) format.style.fontFamily = family;
    }

    if (const pugi::xml_node vertAlign = Child(rPr, "vertAlign")) {
        const std::string value = Attr(vertAlign, "val");
        if (value == "superscript") format.style.vertical = doc::VerticalAlign::kSuperscript;
        else if (value == "subscript") format.style.vertical = doc::VerticalAlign::kSubscript;
        else if (value == "baseline") format.style.vertical = doc::VerticalAlign::kBaseline;
    }

    if (const pugi::xml_node spacing = Child(rPr, "spacing")) {
        if (const auto twips = IntAttr(spacing, "val")) {
            format.style.letterSpacingPt = TwipsToPoints(*twips);
        }
    }

    return format;
}

doc::ParagraphStyle ParseParagraphProperties(const pugi::xml_node& pPr) {
    doc::ParagraphStyle style;
    if (!pPr) return style;

    style.alignment = ParseAlignment(pPr);
    style.keepWithNext = OnOff(pPr, "keepNext");

    if (const pugi::xml_node spacing = Child(pPr, "spacing")) {
        if (const auto before = IntAttr(spacing, "before")) {
            style.spaceBeforePt = TwipsToPoints(*before);
        }
        if (const auto after = IntAttr(spacing, "after")) {
            style.spaceAfterPt = TwipsToPoints(*after);
        }
        // w:line means different things depending on w:lineRule: "auto" makes it
        // 240ths of a line, anything else makes it twips. Only the multiple is
        // portable, so exact spacing is deliberately not carried.
        if (const auto line = IntAttr(spacing, "line")) {
            const std::string rule = Attr(spacing, "lineRule");
            if ((rule.empty() || rule == "auto") && *line > 0) {
                style.lineHeight = static_cast<float>(*line) / 240.0f;
            }
        }
    }

    if (const pugi::xml_node indent = Child(pPr, "ind")) {
        if (const auto left = IntAttr(indent, "left")) {
            style.indentLeftPt = TwipsToPoints(*left);
        } else if (const auto start = IntAttr(indent, "start")) {
            style.indentLeftPt = TwipsToPoints(*start);  // the newer spelling
        }
        if (const auto right = IntAttr(indent, "right")) {
            style.indentRightPt = TwipsToPoints(*right);
        } else if (const auto end = IntAttr(indent, "end")) {
            style.indentRightPt = TwipsToPoints(*end);
        }
        if (const auto first = IntAttr(indent, "firstLine")) {
            style.indentFirstLinePt = TwipsToPoints(*first);
        } else if (const auto hanging = IntAttr(indent, "hanging")) {
            style.indentFirstLinePt = -TwipsToPoints(*hanging);
        }
    }

    style.text = ParseRunProperties(Child(pPr, "rPr")).style;
    return style;
}

void StyleResolver::Load(const pugi::xml_node& stylesRoot) {
    if (!stylesRoot) return;

    if (const pugi::xml_node docDefaults = Child(stylesRoot, "docDefaults")) {
        defaults_ = ParseParagraphProperties(Descend(docDefaults, "pPrDefault", "pPr"));
        runDefaults_ = ParseRunProperties(Descend(docDefaults, "rPrDefault", "rPr"));
        // docDefaults' run formatting is also the paragraph default's text
        // formatting, so a writer asking "what does normal text look like" gets
        // one answer rather than two.
        defaults_.text.Underlay(runDefaults_.style);
    }

    for (pugi::xml_node node = stylesRoot.first_child(); node; node = node.next_sibling()) {
        if (LocalName(node) != "style") continue;

        RawStyle style;
        style.id = Attr(node, "styleId");
        if (style.id.empty()) continue;

        style.type = Attr(node, "type");
        style.name = Attr(Child(node, "name"), "val");
        style.basedOn = Attr(Child(node, "basedOn"), "val");

        const pugi::xml_node pPr = Child(node, "pPr");
        style.paragraph = ParseParagraphProperties(pPr);
        style.run = ParseRunProperties(Child(node, "rPr"));

        if (const auto outline = IntAttr(Child(pPr, "outlineLvl"), "val")) {
            // 0-based in the file, 1-based everywhere a human looks at it.
            style.outlineLevel = static_cast<int>(*outline) + 1;
        }
        if (const pugi::xml_node numPr = Child(pPr, "numPr")) {
            if (const auto numId = IntAttr(Child(numPr, "numId"), "val")) {
                style.numId = std::to_string(*numId);
            }
            if (const auto level = IntAttr(Child(numPr, "ilvl"), "val")) {
                style.numLevel = static_cast<int>(*level);
            }
        }

        if (style.type == "paragraph" && OnOff(node, "default").value_or(false)) {
            defaultParagraphStyleId_ = style.id;
        }
        styles_.insert_or_assign(style.id, std::move(style));
    }
}

const RawStyle* StyleResolver::Find(std::string_view styleId) const {
    const auto it = styles_.find(styleId);
    return it == styles_.end() ? nullptr : &it->second;
}

std::vector<const RawStyle*> StyleResolver::Chain(std::string_view styleId) const {
    std::vector<const RawStyle*> chain;
    std::vector<std::string_view> seen;

    for (std::string_view at = styleId; !at.empty();) {
        const RawStyle* style = Find(at);
        if (style == nullptr) break;
        // A basedOn cycle can be hand-edited or hostile, and Word tolerates it.
        // Truncating is the only sensible response; following it is a hang.
        if (std::find(seen.begin(), seen.end(), at) != seen.end()) break;
        seen.push_back(at);

        chain.push_back(style);
        at = style->basedOn;

        // Depth is bounded in practice at three or four. A styles.xml with a
        // hundred-deep chain is not something to spend time on.
        if (chain.size() > 32) break;
    }
    return chain;
}

doc::ParagraphStyle StyleResolver::ResolveParagraph(std::string_view styleId) const {
    doc::ParagraphStyle resolved;

    // Nearest style first, then each basedOn ancestor, then docDefaults. Every
    // Underlay only fills gaps, so the first writer of a value wins and the
    // order below IS the precedence order.
    for (const RawStyle* style : Chain(styleId)) {
        resolved.Underlay(style->paragraph);
        resolved.text.Underlay(style->run.style);
    }
    resolved.Underlay(defaults_);
    return resolved;
}

RunFormat StyleResolver::ResolveRun(std::string_view paragraphStyleId,
                                    std::string_view characterStyleId) const {
    RunFormat resolved;

    // A character style is nearer than the paragraph style it sits inside.
    for (const RawStyle* style : Chain(characterStyleId)) resolved.Underlay(style->run);
    for (const RawStyle* style : Chain(paragraphStyleId)) resolved.Underlay(style->run);
    resolved.Underlay(runDefaults_);
    return resolved;
}

std::optional<int> StyleResolver::HeadingLevel(std::string_view styleId) const {
    for (const RawStyle* style : Chain(styleId)) {
        // An explicit outline level is the author's own statement of intent and
        // beats any name-matching heuristic.
        if (style->outlineLevel && *style->outlineLevel >= 1 && *style->outlineLevel <= 6) {
            return style->outlineLevel;
        }
        if (const auto fromId = HeadingLevelFromName(style->id)) return fromId;
        if (const auto fromName = HeadingLevelFromName(style->name)) return fromName;
    }
    return std::nullopt;
}

doc::StyleTable StyleResolver::BuildTable() const {
    doc::StyleTable table;
    table.SetDefault(ResolveParagraph(defaultParagraphStyleId_));

    for (const auto& [id, style] : styles_) {
        if (style.type != "paragraph" && style.type != "character") continue;

        doc::NamedStyle named;
        named.name = style.name.empty() ? style.id : style.name;
        named.paragraph = ResolveParagraph(id);
        named.headingLevel = HeadingLevel(id);
        table.Set(id, std::move(named));
    }
    return table;
}

}  // namespace morph::docooxml
