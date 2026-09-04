// Turning the graph into the Explorer menu (architecture section 9.2).
//
// The broker is the only process that holds the graph and the only one allowed
// to publish, so this is where a planner result becomes a menu. What it does is
// run the planner once per source extension, ahead of any right-click, and store
// the answers where a reader can get at them without asking anyone anything.
#pragma once

#include <cstdint>

#include "morph/core/caps.h"
#include "morph/core/graph.h"

namespace morph::broker {

// Every extension Morph reads, with everything it can become.
//
// Two rules decide what is in the list, and both are section 12.2's "never offer
// what cannot be done": a target with no extension of its own is intermediate
// (application/morph-ir is the only one today) and is left out, and converting a
// format to itself is not a conversion and is left out too.
[[nodiscard]] CapabilitySet CapabilitiesOf(const FormatGraph& graph);

}  // namespace morph::broker
