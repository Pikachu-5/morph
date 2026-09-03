// DOCX style resolution (architecture section 8.2, design rule 2).
//
// "DOCX style inheritance (docDefaults -> style chain via basedOn -> direct
//  rPr/pPr) is flattened into concrete values when reading. Writers never walk
//  an inheritance chain. This eliminates an entire category of bugs."
//
// So this file is where the chain is walked, once, and it is the only place in
// the codebase that knows the chain exists.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <pugixml.hpp>

#include "morph/core/doc/style.h"

namespace morph::docooxml {

// Character formatting as OOXML expresses it, before it is split between
// semantic inline nodes and a TextStyle.
//
// Bold, italic and strikethrough are kept separate from TextStyle on purpose:
// they become Strong, Emphasis and Strikethrough nodes because every target
// format has a construct for them, while colour and font have no semantic
// equivalent and land on the style. See the IR's style.h.
struct RunFormat {
    std::optional<bool> bold;
    std::optional<bool> italic;
    std::optional<bool> strike;
    doc::TextStyle style;

    // Values already set here win; `under` supplies the rest.
    void Underlay(const RunFormat& under);

    [[nodiscard]] bool SameAs(const RunFormat& other) const;
};

// A style as the document declares it, with its inheritance link intact. The
// resolver flattens these; nothing outside this file sees `basedOn`.
struct RawStyle {
    std::string id;
    std::string name;
    std::string type;  // "paragraph", "character", "table", "numbering"
    std::string basedOn;
    doc::ParagraphStyle paragraph;
    RunFormat run;

    std::optional<int> outlineLevel;  // w:pPr/w:outlineLvl, 0-based in the file
    std::optional<std::string> numId;
    std::optional<int> numLevel;
};

class StyleResolver {
public:
    // `stylesXml` is word/styles.xml. A document without one is legal and
    // common; everything then falls back to the built-in defaults.
    void Load(const pugi::xml_node& stylesRoot);

    // Flattened paragraph style for a styleId, including docDefaults. Returns
    // the defaults for an unknown id rather than failing: a document that
    // references a style it does not define is malformed but readable, and Word
    // reads it.
    [[nodiscard]] doc::ParagraphStyle ResolveParagraph(std::string_view styleId) const;

    // Flattened run format for a character styleId.
    //
    // Deliberately NOT layered with the paragraph style's rPr or with
    // docDefaults. A run's IR formatting carries only what is not already
    // implied by the block it sits in: the paragraph's own style is reachable
    // through Paragraph::styleRef and StyleTable, and docDefaults is what
    // StyleTable::Default() is for.
    //
    // Folding those in instead would put the document's default font and size on
    // every single run, so an HTML writer would wrap every word in a span and a
    // Markdown writer would report dropped character formatting for a document
    // that has none. Worse, a heading whose style sets bold would come back as
    // "# **Title**".
    [[nodiscard]] RunFormat ResolveRun(std::string_view characterStyleId) const;

    // True when this style means "this paragraph is code". Word has no code
    // block element, so a styled paragraph IS the representation, and both our
    // own writer and Word's HTML import use recognisable names for it.
    [[nodiscard]] bool IsCodeStyle(std::string_view styleId) const;

    // True when this style means "this paragraph is quoted". Word has no block
    // quote element either, so like code it is a styled paragraph -- and our own
    // writer produces one, which makes reading it back a symmetry requirement
    // rather than a nicety.
    [[nodiscard]] bool IsQuoteStyle(std::string_view styleId) const;

    // The heading level a style implies, if any. Detected once here so that
    // neither the writers nor the body walk has to pattern-match on style names.
    //
    // Three signals, in order of reliability: an explicit w:outlineLvl, the
    // built-in style id ("Heading1"), and the style's display name
    // ("heading 1"). Real documents supply different subsets of the three.
    [[nodiscard]] std::optional<int> HeadingLevel(std::string_view styleId) const;

    // The numbering a style attaches, for list items that carry their numPr on
    // the style rather than on the paragraph.
    [[nodiscard]] const RawStyle* Find(std::string_view styleId) const;

    [[nodiscard]] const doc::ParagraphStyle& Defaults() const noexcept { return defaults_; }
    [[nodiscard]] const RunFormat& RunDefaults() const noexcept { return runDefaults_; }

    // The style table the IR carries, already flattened.
    [[nodiscard]] doc::StyleTable BuildTable() const;

    // The id of the style Word marks as the document default, or "Normal".
    [[nodiscard]] const std::string& DefaultParagraphStyleId() const noexcept {
        return defaultParagraphStyleId_;
    }

private:
    // Walks basedOn from `styleId` up to the root, nearest first. Cycles are
    // truncated rather than followed: a hand-edited or hostile styles.xml can
    // contain one, and Word tolerates it.
    [[nodiscard]] std::vector<const RawStyle*> Chain(std::string_view styleId) const;

    std::map<std::string, RawStyle, std::less<>> styles_;
    doc::ParagraphStyle defaults_;
    RunFormat runDefaults_;
    std::string defaultParagraphStyleId_ = "Normal";
};

// Parses a w:rPr element into a RunFormat. Shared by the style loader and the
// body walk, because direct formatting and style formatting have identical
// syntax -- which is exactly why flattening them into one type works.
[[nodiscard]] RunFormat ParseRunProperties(const pugi::xml_node& rPr);

// Parses a w:pPr element into a ParagraphStyle.
[[nodiscard]] doc::ParagraphStyle ParseParagraphProperties(const pugi::xml_node& pPr);

}  // namespace morph::docooxml
