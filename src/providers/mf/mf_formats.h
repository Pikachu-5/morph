// The format table for the Media Foundation backend (architecture sections 4.1
// and 7.1).
//
// A node is a container PLUS a codec, because section 4.1 is explicit about it:
// "video/mp4;codec=h264 and video/mp4;codec=hevc are different nodes because
// converting between them costs a full re-encode, while video/mp4;codec=h264 to
// video/mkv;codec=h264 is a remux and costs almost nothing. Collapsing both to
// 'MP4' would make the planner blind to a 100x cost difference."
//
// Splitting this out of the provider keeps the remux decision -- which is a
// question about two profiles and nothing else -- testable without a media file
// or a codec.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace morph::mf {

enum class Media : std::uint8_t { kVideo, kAudio };

struct Format {
    std::string_view profile;
    std::string_view codec;      // "h264", "aac", "pcm"; the identity for remuxing
    Media media = Media::kAudio;
    bool lossy = true;

    // Media Foundation reads it. Everything here does.
    bool decode = true;
    // Media Foundation has a sink and an encoder for it. Far fewer things do:
    // there is no QuickTime sink and no FLAC encoder on Windows, which is
    // section 7.3's honest reason for a GitHub SKU to exist at all.
    bool encode = false;

    // Which encoder must be present for `encode` to be true on this machine.
    // Empty means "no encoder needed" -- PCM is written by the sink itself.
    std::string_view encoder;

    float cpuPerMiB = 0.0f;
    float memPeakFactor = 1.0f;
};

// Every format this backend knows. Order is irrelevant; lookups are by profile.
[[nodiscard]] std::span<const Format> Formats() noexcept;

[[nodiscard]] const Format* FindFormat(std::string_view profile) noexcept;

}  // namespace morph::mf
