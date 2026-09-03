#include "morph/core/media_profile.h"

#include <algorithm>
#include <cctype>

namespace morph {
namespace {

std::string ToLowerAscii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string_view Trim(std::string_view s) {
    const auto is_space = [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    };
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

}  // namespace

MediaProfile::MediaProfile(std::string_view container,
                           std::vector<Attribute> attributes)
    : container_(ToLowerAscii(Trim(container))) {
    if (container_.empty()) return;

    for (auto& [key, value] : attributes) {
        key = ToLowerAscii(Trim(key));
        value = ToLowerAscii(Trim(value));
    }
    // An empty key carries no meaning and would corrupt the canonical form.
    std::erase_if(attributes, [](const Attribute& a) { return a.first.empty(); });

    // Sort by key, then stably keep the LAST value written for a duplicate key.
    std::stable_sort(attributes.begin(), attributes.end(),
                     [](const Attribute& a, const Attribute& b) { return a.first < b.first; });
    for (auto it = attributes.begin(); it != attributes.end();) {
        auto next = std::find_if(it, attributes.end(),
                                 [&](const Attribute& a) { return a.first != it->first; });
        attributes_.push_back(*std::prev(next));
        it = next;
    }

    id_ = container_;
    for (const auto& [key, value] : attributes_) {
        id_ += ';';
        id_ += key;
        id_ += '=';
        id_ += value;
    }
}

std::optional<MediaProfile> MediaProfile::Parse(std::string_view spec) {
    spec = Trim(spec);
    if (spec.empty()) return std::nullopt;

    const auto first = spec.find(';');
    const std::string_view container = Trim(spec.substr(0, first));
    if (container.empty()) return std::nullopt;

    std::vector<Attribute> attributes;
    std::size_t pos = (first == std::string_view::npos) ? spec.size() : first + 1;
    while (pos < spec.size()) {
        const auto end = std::min(spec.find(';', pos), spec.size());
        const std::string_view token = Trim(spec.substr(pos, end - pos));
        pos = end + 1;

        if (token.empty()) continue;  // tolerate ";;" and a trailing ';'
        const auto eq = token.find('=');
        if (eq == std::string_view::npos || eq == 0) {
            return std::nullopt;  // a bare token or "=value" is malformed
        }
        attributes.emplace_back(std::string(token.substr(0, eq)),
                                std::string(token.substr(eq + 1)));
    }

    return MediaProfile(container, std::move(attributes));
}

std::optional<std::string_view> MediaProfile::Attr(std::string_view key) const noexcept {
    const auto it = std::lower_bound(
        attributes_.begin(), attributes_.end(), key,
        [](const Attribute& a, std::string_view k) { return a.first < k; });
    if (it == attributes_.end() || it->first != key) return std::nullopt;
    return std::string_view(it->second);
}

std::string_view MediaProfile::Family() const noexcept {
    const auto slash = container_.find('/');
    if (slash == std::string::npos) return std::string_view(container_);
    return std::string_view(container_).substr(0, slash);
}

}  // namespace morph
