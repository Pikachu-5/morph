// Morph — the option bag (architecture section 5).
//
// "resolved preset + per-job overrides". Resolution happens once, in the broker,
// before the job is handed to a worker: a provider receives concrete values and
// never has to know what a preset is.
//
// Options also participate in the plan cache key (section 4.2), so the bag must
// hash stably across processes. That rules out std::hash, whose values are not
// guaranteed to agree between runs, let alone between the broker and a worker.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace morph {

using OptionValue = std::variant<bool, std::int64_t, double, std::string>;

class OptionBag {
public:
    OptionBag() = default;

    void Set(std::string key, OptionValue value);

    [[nodiscard]] bool Has(std::string_view key) const;
    [[nodiscard]] const OptionValue* Find(std::string_view key) const;

    // Typed access with a fallback. Deliberately lenient about numeric width so
    // that a preset written as 90 and an override arriving from JSON as 90.0
    // behave the same, but never lenient about kind: asking for a bool and
    // finding a string yields the fallback, not a guess.
    [[nodiscard]] bool GetBool(std::string_view key, bool fallback) const;
    [[nodiscard]] std::int64_t GetInt(std::string_view key, std::int64_t fallback) const;
    [[nodiscard]] double GetDouble(std::string_view key, double fallback) const;
    [[nodiscard]] std::string GetString(std::string_view key, std::string_view fallback) const;

    // Values in `overrides` win. This is exactly how a per-job override is
    // layered onto a resolved preset.
    void MergeOver(const OptionBag& overrides);

    [[nodiscard]] bool Empty() const noexcept { return values_.empty(); }
    [[nodiscard]] std::size_t Size() const noexcept { return values_.size(); }

    [[nodiscard]] const std::map<std::string, OptionValue, std::less<>>& All() const noexcept {
        return values_;
    }

    // FNV-1a over the canonically ordered key/value pairs. Stable across
    // processes and across runs, which is what the plan cache and the IPC
    // boundary both require. Not cryptographic and not meant to be.
    [[nodiscard]] std::uint64_t Hash() const noexcept;

    friend bool operator==(const OptionBag& a, const OptionBag& b) noexcept {
        return a.values_ == b.values_;
    }

private:
    // Ordered, not hashed: Hash() must not depend on insertion order, and
    // std::less<> lets us look up by string_view without allocating.
    std::map<std::string, OptionValue, std::less<>> values_;
};

// Option keys used across more than one provider. Provider-specific keys live
// next to their provider, but these are shared vocabulary and a typo in one of
// them is a silently ignored setting.
namespace opt {

// Image quality, 0..100. Maps to WIC's ImageQuality (0..1) in the WIC provider.
inline constexpr std::string_view kQuality = "quality";

// Prefer a lossless encoding where the target format offers one, e.g. WebP.
inline constexpr std::string_view kLossless = "lossless";

// Strip metadata that is not required for correct rendering. Off by default:
// section 6.2 preserves the ICC profile and EXIF minus orientation.
inline constexpr std::string_view kStripMetadata = "strip-metadata";

// Longest edge in pixels, 0 for no limit.
inline constexpr std::string_view kMaxDimension = "max-dimension";

// Markdown flavour: "commonmark", "gfm", or "portable" (section 8.3).
inline constexpr std::string_view kMarkdownFlavour = "markdown-flavour";

// Emit a single self-contained file, inlining images as data URIs, rather than
// a document plus an assets directory (section 8.5).
inline constexpr std::string_view kSelfContained = "self-contained";

// Page size for the PDF stage: "a4" or "letter".
inline constexpr std::string_view kPageSize = "page-size";

}  // namespace opt

}  // namespace morph
