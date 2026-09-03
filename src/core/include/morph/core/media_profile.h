// Morph — the graph node (architecture section 4.1).
//
// A node is not a file extension. It is a media profile: a container plus a
// codec or encoding plus the constraints that matter for planning.
//
//   video/mp4;codec=h264  and  video/mp4;codec=hevc  are different nodes
//   because converting between them costs a full re-encode, while
//   video/mp4;codec=h264 -> video/mkv;codec=h264 is a remux and costs almost
//   nothing. Collapsing both to "MP4" would make the planner blind to a 100x
//   cost difference.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph {

class MediaProfile {
public:
    using Attribute = std::pair<std::string, std::string>;

    MediaProfile() = default;

    // Attributes are canonicalised on construction: lowercased, sorted by key,
    // duplicates resolved last-wins. Two profiles that mean the same thing
    // therefore have the same Id() regardless of how they were written.
    MediaProfile(std::string_view container, std::vector<Attribute> attributes);

    // Parses "container[;key=value]*", e.g. "video/mp4;codec=h264".
    // Returns nullopt on a malformed spec rather than throwing: these strings
    // come from provider registration tables and, later, from the IPC boundary.
    [[nodiscard]] static std::optional<MediaProfile> Parse(std::string_view spec);

    // Canonical form. This is the cache key, the map key and the wire form.
    [[nodiscard]] const std::string& Id() const noexcept { return id_; }

    [[nodiscard]] const std::string& Container() const noexcept { return container_; }
    [[nodiscard]] const std::vector<Attribute>& Attributes() const noexcept { return attributes_; }

    // nullopt when the attribute is absent. An empty value is a distinct thing
    // from an absent one, so callers can tell "no codec" from "codec unknown".
    [[nodiscard]] std::optional<std::string_view> Attr(std::string_view key) const noexcept;

    // Convenience for the overwhelmingly common attribute.
    [[nodiscard]] std::optional<std::string_view> Codec() const noexcept { return Attr("codec"); }

    // The broad family, i.e. the part before '/': "image", "video", "audio",
    // "text", "application". The shell extension groups the submenu by this
    // (section 9.3) and the target picker groups by it too (section 12.2).
    [[nodiscard]] std::string_view Family() const noexcept;

    [[nodiscard]] bool Empty() const noexcept { return id_.empty(); }

    friend bool operator==(const MediaProfile& a, const MediaProfile& b) noexcept {
        return a.id_ == b.id_;
    }
    friend bool operator<(const MediaProfile& a, const MediaProfile& b) noexcept {
        return a.id_ < b.id_;
    }

private:
    std::string id_;
    std::string container_;
    std::vector<Attribute> attributes_;
};

}  // namespace morph

template <>
struct std::hash<morph::MediaProfile> {
    std::size_t operator()(const morph::MediaProfile& p) const noexcept {
        return std::hash<std::string>{}(p.Id());
    }
};
