#include "menu.h"

#include <algorithm>
#include <cctype>
#include <map>

#include "morph/core/fidelity.h"

namespace morph::shellext {
namespace {

std::string Normalised(std::string_view extension) {
    if (!extension.empty() && extension.front() == '.') extension.remove_prefix(1);
    std::string out(extension);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string Upper(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

// The fidelity contract in four words, because a context menu has room for four
// words. Section 8.8 requires the user to be told what they are about to lose
// BEFORE they commit, and this is the only place they will see it if they never
// open the app.
//
// Lossless says nothing on purpose: an unadorned "To PNG" reads as the normal
// case, and adding "(lossless)" to most entries would make the badge invisible
// on the ones that matter.
std::string_view Caveat(Fidelity fidelity) {
    switch (fidelity) {
        case Fidelity::Lossless: return "";
        case Fidelity::High: return " (re-encoded once)";
        case Fidelity::Structural: return " (structure, not layout)";
        case Fidelity::TextOnly: return " (text only)";
        case Fidelity::Rasterized: return " (as pixels)";
    }
    return "";
}

}  // namespace

std::string MenuEntry::Title() const {
    return "To " + label + std::string(Caveat(static_cast<Fidelity>(fidelity)));
}

Menu BuildMenu(const CapabilityTable& table, const std::vector<std::string>& extensions) {
    Menu menu;
    if (!table.Valid() || extensions.empty()) return menu;

    // Distinct, because a folder of two hundred photographs is one question, not
    // two hundred.
    std::vector<std::string> distinct;
    for (const std::string& extension : extensions) {
        std::string normalised = Normalised(extension);
        if (normalised.empty()) continue;
        if (std::find(distinct.begin(), distinct.end(), normalised) == distinct.end()) {
            distinct.push_back(std::move(normalised));
        }
        if (distinct.size() > kMaxDistinctExtensions) return menu;  // generic, per 9.3
    }
    if (distinct.empty()) return menu;

    // Intersect across the selection, keeping the worst rating and the longest
    // route: what the menu promises has to be true of every file in it.
    std::map<std::string, MenuEntry> common;
    bool first = true;

    for (const std::string& extension : distinct) {
        const std::span<const CapsTarget> targets = table.TargetsFor(extension);
        if (targets.empty()) return menu;  // one unconvertible file makes it generic

        std::map<std::string, MenuEntry> here;
        for (const CapsTarget& target : targets) {
            MenuEntry entry;
            entry.profile = std::string(table.String(target.profileOff));
            entry.label = Upper(table.String(target.extensionOff));
            entry.fidelity = target.fidelity;
            entry.hopCount = target.hopCount;
            if (entry.profile.empty() || entry.label.empty()) continue;
            here.emplace(entry.profile, std::move(entry));
        }

        if (first) {
            common = std::move(here);
            first = false;
            continue;
        }

        for (auto it = common.begin(); it != common.end();) {
            const auto match = here.find(it->first);
            if (match == here.end()) {
                it = common.erase(it);
                continue;
            }
            it->second.fidelity = std::max(it->second.fidelity, match->second.fidelity);
            it->second.hopCount = std::max(it->second.hopCount, match->second.hopCount);
            ++it;
        }
        if (common.empty()) return menu;
    }

    // A selection that is all one format must not offer to convert it to itself.
    // The table already drops the identity target per source, but a mixed
    // selection can still surface a target that IS one of the sources -- and
    // converting five PNGs and a JPEG "to PNG" would silently do nothing to
    // five of them.
    for (const std::string& extension : distinct) {
        for (auto it = common.begin(); it != common.end();) {
            const std::string label = Upper(extension);
            it = it->second.label == label ? common.erase(it) : std::next(it);
        }
    }
    if (common.empty()) return menu;

    for (auto& [profile, entry] : common) menu.entries.push_back(std::move(entry));

    std::sort(menu.entries.begin(), menu.entries.end(),
              [](const MenuEntry& a, const MenuEntry& b) {
                  if (a.fidelity != b.fidelity) return a.fidelity < b.fidelity;
                  if (a.hopCount != b.hopCount) return a.hopCount < b.hopCount;
                  return a.label < b.label;
              });
    if (menu.entries.size() > kMaxTargets) menu.entries.resize(kMaxTargets);

    menu.generic = false;
    return menu;
}

}  // namespace morph::shellext
