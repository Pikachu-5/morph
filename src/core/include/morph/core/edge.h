// Morph — the graph edge and its cost vector (architecture section 4.1).
//
// An edge is a transform advertised by a provider. Each edge declares a cost
// VECTOR rather than a scalar, because the thing users care about (what am I
// about to lose?) is not the thing a scalar cost would optimise (how fast is
// it?). See planner.h for how the vector is ordered.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "morph/core/fidelity.h"
#include "morph/core/media_profile.h"

namespace morph {

// Stable, human-readable provider identifier: "wic", "mf", "ffmpeg", "docooxml".
// Matches IConversionProvider::Id() (section 5).
using ProviderId = std::string;

struct EdgeCost {
    // What this single hop costs the user. Folded with Worst() along a path.
    Fidelity fidelity = Fidelity::Lossless;

    // 0 = no recompression, 1 = one lossy generation, and so on. Summed along a
    // path. This is what distinguishes "decode and re-encode once" from a
    // three-hop route that re-encodes at every step.
    std::uint8_t generational = 0;

    // Measured, calibrated at first run (section 11.4). Milliseconds of CPU per
    // MiB of input. Seeded from a table, overwritten by real measurements.
    float cpuPerMiB = 0.0f;

    // Peak RSS as a multiple of input size. Feeds the job object memory limit
    // in section 11.2: a task whose budget exceeds the cap is rejected at PLAN
    // time with a clear message, not started and then killed halfway.
    float memPeakFactor = 1.0f;

    // Needs a temp spill file. Hops are otherwise connected by the streaming
    // ISource/ISink pair in section 5 and never touch the disk; without this
    // flag a four-hop conversion of a 2 GB video would need 8 GB of scratch.
    bool requiresDisk = false;

    // Filled in by the graph when the edge is registered, from the registering
    // provider's Id(). Providers do not repeat their own name on every edge.
    ProviderId provider;
};

// What a provider returns from EnumerateEdges(). Declared once at registration;
// the broker builds the graph from these (section 5).
struct EdgeDecl {
    MediaProfile from;
    MediaProfile to;
    EdgeCost cost;
};

}  // namespace morph
