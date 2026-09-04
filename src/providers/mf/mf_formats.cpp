#include "mf_formats.h"

#include <array>

namespace morph::mf {
namespace {

// cpuPerMiB is milliseconds of CPU per MiB of INPUT, and the video numbers are
// large on purpose: a transcode is three orders of magnitude more expensive than
// a remux, and the planner's whole job is to notice that. They are seeds to be
// replaced by measurement (section 11.4), not claims.
constexpr std::array<Format, 9> kFormats{{
    // --- Video ---------------------------------------------------------------
    {"video/mp4;codec=h264", "h264", Media::kVideo, true, true, true, "h264", 900.0f, 2.5f},
    {"video/mp4;codec=hevc", "hevc", Media::kVideo, true, true, true, "hevc", 1400.0f, 2.5f},
    // QuickTime is read and never written: Media Foundation has no .mov sink.
    // The remux path makes this less of a loss than it sounds -- a MOV holding
    // H.264 becomes an MP4 holding the same samples, at copy speed.
    {"video/quicktime;codec=h264", "h264", Media::kVideo, true, true, false, "", 900.0f, 2.5f},
    {"video/x-ms-asf;codec=wmv", "wmv", Media::kVideo, true, true, false, "", 900.0f, 2.5f},

    // --- Audio ---------------------------------------------------------------
    {"audio/mp4;codec=aac", "aac", Media::kAudio, true, true, true, "aac", 140.0f, 2.0f},
    {"audio/mpeg;codec=mp3", "mp3", Media::kAudio, true, true, true, "mp3", 130.0f, 2.0f},
    // PCM in a WAV container. No encoder MFT is involved: the sink writes the
    // samples it is given, which is why this is the one target that is available
    // wherever Media Foundation starts at all.
    {"audio/wav;codec=pcm", "pcm", Media::kAudio, false, true, true, "", 40.0f, 12.0f},
    // FLAC decodes on Windows 10 and later and has never encoded. Section 7.3
    // lists it under the GitHub SKU for exactly this reason.
    {"audio/flac", "flac", Media::kAudio, false, true, false, "", 90.0f, 12.0f},
    {"audio/x-ms-wma;codec=wma", "wma", Media::kAudio, true, true, true, "wma", 140.0f, 2.0f},
}};

}  // namespace

std::span<const Format> Formats() noexcept {
    return std::span<const Format>(kFormats.data(), kFormats.size());
}

const Format* FindFormat(std::string_view profile) noexcept {
    for (const Format& format : kFormats) {
        if (format.profile == profile) return &format;
    }
    return nullptr;
}

}  // namespace morph::mf
