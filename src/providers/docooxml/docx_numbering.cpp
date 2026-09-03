#include "docx_numbering.h"

#include "docx_xml.h"

namespace morph::docooxml {
namespace {

LevelDefinition ParseLevel(const pugi::xml_node& lvl) {
    LevelDefinition level;

    if (const auto start = IntAttr(Child(lvl, "start"), "val")) {
        level.start = static_cast<int>(*start);
    }
    if (const pugi::xml_node fmt = Child(lvl, "numFmt")) {
        level.format = Attr(fmt, "val");
    }
    level.text = Attr(Child(lvl, "lvlText"), "val");

    // "none" is a list with no marker at all, which is still not an ordered
    // list. Everything else -- decimal, letters, roman, ordinal, chicago -- is.
    level.ordered = !level.format.empty() && level.format != "bullet" && level.format != "none";

    if (const auto indent = IntAttr(Descend(lvl, "pPr", "ind"), "left")) {
        level.indentLeftPt = TwipsToPoints(*indent);
    }
    return level;
}

}  // namespace

void NumberingTable::Load(const pugi::xml_node& numberingRoot) {
    if (!numberingRoot) return;

    for (pugi::xml_node node = numberingRoot.first_child(); node; node = node.next_sibling()) {
        const std::string_view name = LocalName(node);

        if (name == "abstractNum") {
            const std::string id = Attr(node, "abstractNumId");
            if (id.empty()) continue;

            std::map<int, LevelDefinition> levels;
            for (pugi::xml_node lvl = node.first_child(); lvl; lvl = lvl.next_sibling()) {
                if (LocalName(lvl) != "lvl") continue;
                const auto ilvl = IntAttr(lvl, "ilvl");
                if (!ilvl) continue;
                levels.emplace(static_cast<int>(*ilvl), ParseLevel(lvl));
            }
            abstract_.insert_or_assign(id, std::move(levels));
            continue;
        }

        if (name == "num") {
            const auto numId = IntAttr(node, "numId");
            if (!numId) continue;

            Instance instance;
            instance.abstractId = Attr(Child(node, "abstractNumId"), "val");

            for (pugi::xml_node override_node = node.first_child(); override_node;
                 override_node = override_node.next_sibling()) {
                if (LocalName(override_node) != "lvlOverride") continue;
                const auto ilvl = IntAttr(override_node, "ilvl");
                if (!ilvl) continue;
                const int level = static_cast<int>(*ilvl);

                // A full replacement level.
                if (const pugi::xml_node lvl = Child(override_node, "lvl")) {
                    instance.overrides.emplace(level, ParseLevel(lvl));
                }
                // Mapping 4: the same numId restarting mid-document. Ignoring
                // this is how long documents get subtly wrong numbering.
                if (const auto start = IntAttr(Child(override_node, "startOverride"), "val")) {
                    instance.restarts[level] = true;
                    LevelDefinition& definition = instance.overrides[level];
                    definition.start = static_cast<int>(*start);
                    // A startOverride on its own does not describe the format,
                    // so mark it for filling in from the abstract definition.
                    if (definition.format.empty()) definition.format = "decimal";
                }
            }
            numbering_.insert_or_assign(std::to_string(*numId), std::move(instance));
        }
    }
}

std::optional<LevelDefinition> NumberingTable::Find(std::string_view numId, int level) const {
    const auto instance = numbering_.find(numId);
    if (instance == numbering_.end()) return std::nullopt;

    const auto abstract = abstract_.find(instance->second.abstractId);
    std::optional<LevelDefinition> base;
    if (abstract != abstract_.end()) {
        const auto found = abstract->second.find(level);
        if (found != abstract->second.end()) base = found->second;
    }

    const auto overridden = instance->second.overrides.find(level);
    if (overridden == instance->second.overrides.end()) return base;

    // An override wins, but only for the fields it actually carries. A
    // startOverride alone must not erase the abstract level's format, which is
    // what a naive "the override replaces the level" implementation does: an
    // ordinary restarted bullet list then renders as "1. 2. 3.".
    LevelDefinition merged = overridden->second;
    if (base) {
        const bool start_only = merged.text.empty();
        if (start_only) {
            merged.format = base->format;
            merged.text = base->text;
        }
        if (!merged.indentLeftPt) merged.indentLeftPt = base->indentLeftPt;
    }
    merged.ordered =
        !merged.format.empty() && merged.format != "bullet" && merged.format != "none";
    return merged;
}

bool NumberingTable::RestartsAt(std::string_view numId, int level) const {
    const auto instance = numbering_.find(numId);
    if (instance == numbering_.end()) return false;
    const auto found = instance->second.restarts.find(level);
    return found != instance->second.restarts.end() && found->second;
}

}  // namespace morph::docooxml
