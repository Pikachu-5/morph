// What the right-click menu says (architecture sections 9.2 and 9.3).
//
// Deliberately free of COM. Everything here is a function from a selection and a
// capability table to a list of entries, which means the decisions that actually
// matter -- what a mixed selection offers, when to give up and show one generic
// item, how a fidelity rating reads in four words -- can be tested without
// Explorer, a surrogate, or a registered class.
//
// The latency budget in section 17 is 8 ms measured around
// IEnumExplorerCommand::Next, and it is the reason this file exists at all: the
// menu is built from a memory-mapped table and a handful of string comparisons,
// with no IPC and no file IO on the path.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "morph/core/caps.h"

namespace morph::shellext {

// Section 9.3: "if a selection spans more than, say, six distinct input
// families, show the generic 'Convert...' entry rather than a submenu with forty
// children."
inline constexpr std::size_t kMaxDistinctExtensions = 6;

// A menu with more entries than this is not a menu. The targets are sorted best
// first, so the ones that fall off the end are the lossiest.
inline constexpr std::size_t kMaxTargets = 8;

struct MenuEntry {
    // "PNG", from the target's extension. Upper-cased because that is how every
    // other Windows menu spells a format.
    std::string label;
    // The media profile to submit, e.g. "image/png".
    std::string profile;
    std::uint8_t fidelity = 0;
    std::uint8_t hopCount = 0;

    // The whole title, as shown: "To PDF" or "To JPEG (re-encoded once)".
    [[nodiscard]] std::string Title() const;
};

struct Menu {
    // Empty when `generic` is set: there is nothing specific to offer.
    std::vector<MenuEntry> entries;

    // Show the single "Convert with Morph..." item that opens the app instead of
    // a submenu. Section 9.2's degraded path -- no table, an unreadable one, a
    // selection too varied to be useful -- and section 9.3's cap.
    bool generic = true;
};

// Builds the menu for a selection, given the extensions of the files in it (with
// or without dots, in any case).
//
// A target is offered only if EVERY file in the selection can become it, and it
// carries the worst rating any of them would incur. Offering the union would put
// entries in the menu that fail for half the selection, which section 12.2
// forbids in as many words.
[[nodiscard]] Menu BuildMenu(const CapabilityTable& table,
                             const std::vector<std::string>& extensions);

}  // namespace morph::shellext
