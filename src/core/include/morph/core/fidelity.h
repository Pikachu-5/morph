// Morph — the fidelity contract (architecture section 8.8).
//
// Every edge in the format graph declares what it costs. The planner aggregates
// the worst rating along a path and the UI shows that rating BEFORE the user
// commits. This enum is therefore part of the product surface, not an internal
// detail: the ordering below is the order shown to users, and the values are
// serialised over the broker pipe.
#pragma once

#include <cstdint>
#include <string_view>

namespace morph {

// Ordered worst-last. Comparison is meaningful: a > b means a loses more.
// Do not renumber — these values cross the IPC boundary (section 10.2).
enum class Fidelity : std::uint8_t {
    // Round-trips byte-identical or semantically identical.
    // PNG -> WebP (lossless mode); MKV -> MP4 remux.
    Lossless = 0,

    // Visually or semantically equivalent; one lossy generation at most.
    // HEIC -> JPEG q90; MD -> HTML.
    High = 1,

    // Content and hierarchy preserved; visual layout is not.
    // DOCX -> MD; PDF -> DOCX.
    Structural = 2,

    // Text survives, everything else is discarded.
    // PDF -> TXT.
    TextOnly = 3,

    // Output is pixels. Text is no longer text.
    // DOCX -> PNG.
    Rasterized = 4,
};

// The worst of two ratings. Path fidelity is the fold of this over its hops.
[[nodiscard]] constexpr Fidelity Worst(Fidelity a, Fidelity b) noexcept {
    return a > b ? a : b;
}

// Stable identifier. Used in logs, the capability table, and the IPC payload.
[[nodiscard]] constexpr std::string_view Name(Fidelity f) noexcept {
    switch (f) {
        case Fidelity::Lossless:   return "lossless";
        case Fidelity::High:       return "high";
        case Fidelity::Structural: return "structural";
        case Fidelity::TextOnly:   return "text-only";
        case Fidelity::Rasterized: return "rasterized";
    }
    return "unknown";
}

// One sentence, for the badge tooltip in the target picker (section 12.2).
// The UI shows this when the user clicks a badge; it must say what is lost.
[[nodiscard]] constexpr std::string_view Explain(Fidelity f) noexcept {
    switch (f) {
        case Fidelity::Lossless:
            return "Nothing is lost. The result is identical in content.";
        case Fidelity::High:
            return "Equivalent to look at or listen to, but re-encoded once. Repeated "
                   "conversions will degrade.";
        // Media-neutral on purpose. The document wording -- "text, structure and
        // images survive" -- was true until audio arrived, and then extracting a
        // soundtrack from a video reported that the images had survived it.
        case Fidelity::Structural:
            return "The content survives. Its exact presentation does not.";
        case Fidelity::TextOnly:
            return "Only the text survives. Formatting, images and layout are discarded.";
        case Fidelity::Rasterized:
            return "The result is an image. Text will no longer be selectable or searchable.";
    }
    return "Unknown effect on the file.";
}

}  // namespace morph
