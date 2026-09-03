// DOCX numbering (architecture section 8.4, mappings 2 and 4).
//
// "There is no list element in WordprocessingML. A list is a sequence of
//  paragraphs that happen to share a numId, with nesting expressed as ilvl."
//
// numbering.xml holds the definitions those paragraphs point at. The indirection
// is two deep and both hops matter:
//
//   w:numId -> w:num -> w:abstractNumId -> w:abstractNum -> w:lvl[ilvl]
//
// Two documents can share one abstractNum through different numIds, which is how
// Word expresses "another list, same look, numbering restarted". Collapsing the
// two levels would merge those lists into one and number them continuously.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <pugixml.hpp>

namespace morph::docooxml {

struct LevelDefinition {
    // Ordered unless the format is "bullet" or "none". This is the only thing
    // about a level the IR can actually express; everything else here exists to
    // get that answer right.
    bool ordered = false;

    int start = 1;

    // The raw w:numFmt: "decimal", "bullet", "lowerLetter", "upperRoman", ...
    // Kept so a DOCX writer can reproduce the look rather than guessing.
    std::string format = "decimal";

    // The literal from w:lvlText, e.g. "%1." or a bullet glyph. Also kept for
    // the writer's benefit.
    std::string text;

    // Indent in points, used as a fallback signal when a document nests lists by
    // indentation rather than by ilvl.
    std::optional<float> indentLeftPt;
};

class NumberingTable {
public:
    // `numberingRoot` is the w:numbering element of word/numbering.xml. A
    // document without that part is legal and common.
    void Load(const pugi::xml_node& numberingRoot);

    // The definition for a numId at a level, following both hops and merging
    // any w:lvlOverride over the abstract level. Returns nullopt when the numId
    // is unknown, which a malformed document can produce and which the caller
    // must treat as "not a list" rather than as an error.
    //
    // By value rather than by pointer: the merge of an override onto its
    // abstract base produces a value that exists in no table, and handing back a
    // pointer to a cached one is a lifetime trap for the first caller who holds
    // it across another lookup.
    [[nodiscard]] std::optional<LevelDefinition> Find(std::string_view numId, int level) const;

    // Section 8.4 mapping 4: "w:lvlOverride with w:startOverride means the same
    // numId can restart mid-document. Ignore this and long documents get subtly
    // wrong numbering." True when this numId overrides the start for this level.
    [[nodiscard]] bool RestartsAt(std::string_view numId, int level) const;

    [[nodiscard]] bool Empty() const noexcept { return numbering_.empty(); }

private:
    struct Instance {
        std::string abstractId;
        // ilvl -> the level as overridden for this numId specifically.
        std::map<int, LevelDefinition> overrides;
        std::map<int, bool> restarts;
    };

    // abstractNumId -> ilvl -> definition
    std::map<std::string, std::map<int, LevelDefinition>, std::less<>> abstract_;
    // numId -> instance
    std::map<std::string, Instance, std::less<>> numbering_;
};

}  // namespace morph::docooxml
