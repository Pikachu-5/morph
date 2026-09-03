#include "morph/core/options.h"

#include <cmath>
#include <cstring>

namespace morph {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void Eat(std::uint64_t& h, std::string_view bytes) {
    for (const char c : bytes) {
        h ^= static_cast<unsigned char>(c);
        h *= kFnvPrime;
    }
}

void EatRaw(std::uint64_t& h, const void* data, std::size_t size) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= kFnvPrime;
    }
}

}  // namespace

void OptionBag::Set(std::string key, OptionValue value) {
    values_.insert_or_assign(std::move(key), std::move(value));
}

bool OptionBag::Has(std::string_view key) const { return values_.find(key) != values_.end(); }

const OptionValue* OptionBag::Find(std::string_view key) const {
    const auto it = values_.find(key);
    return it == values_.end() ? nullptr : &it->second;
}

bool OptionBag::GetBool(std::string_view key, bool fallback) const {
    const OptionValue* v = Find(key);
    if (!v) return fallback;
    if (const auto* b = std::get_if<bool>(v)) return *b;
    return fallback;
}

std::int64_t OptionBag::GetInt(std::string_view key, std::int64_t fallback) const {
    const OptionValue* v = Find(key);
    if (!v) return fallback;
    if (const auto* i = std::get_if<std::int64_t>(v)) return *i;
    // A preset written as 90 and an override arriving as 90.0 must behave the
    // same. Anything non-integral is a different value, not a rounding job.
    if (const auto* d = std::get_if<double>(v)) {
        double integral = 0.0;
        if (std::modf(*d, &integral) == 0.0) return static_cast<std::int64_t>(integral);
    }
    return fallback;
}

double OptionBag::GetDouble(std::string_view key, double fallback) const {
    const OptionValue* v = Find(key);
    if (!v) return fallback;
    if (const auto* d = std::get_if<double>(v)) return *d;
    if (const auto* i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    return fallback;
}

std::string OptionBag::GetString(std::string_view key, std::string_view fallback) const {
    const OptionValue* v = Find(key);
    if (!v) return std::string(fallback);
    if (const auto* s = std::get_if<std::string>(v)) return *s;
    return std::string(fallback);
}

void OptionBag::MergeOver(const OptionBag& overrides) {
    for (const auto& [key, value] : overrides.values_) values_.insert_or_assign(key, value);
}

std::uint64_t OptionBag::Hash() const noexcept {
    std::uint64_t h = kFnvOffset;
    for (const auto& [key, value] : values_) {
        Eat(h, key);
        h ^= 0x1Fu;  // key/value separator
        h *= kFnvPrime;

        // The alternative index is hashed first so that the integer 1, the
        // double 1.0 and the string "1" produce different digests. They are
        // different option values and must not share a plan cache entry.
        h ^= static_cast<std::uint64_t>(value.index() + 1);
        h *= kFnvPrime;

        std::visit(
            [&h](const auto& held) {
                using T = std::decay_t<decltype(held)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    Eat(h, held);
                } else if constexpr (std::is_same_v<T, bool>) {
                    const unsigned char b = held ? 1u : 0u;
                    EatRaw(h, &b, 1);
                } else if constexpr (std::is_same_v<T, double>) {
                    // Hash the bit pattern, but normalise -0.0 to +0.0 so that
                    // two bags that compare equal always hash equal.
                    const double normalised = (held == 0.0) ? 0.0 : held;
                    EatRaw(h, &normalised, sizeof(normalised));
                } else {
                    EatRaw(h, &held, sizeof(held));
                }
            },
            value);

        h ^= 0x1Eu;  // record separator
        h *= kFnvPrime;
    }
    return h;
}

}  // namespace morph
