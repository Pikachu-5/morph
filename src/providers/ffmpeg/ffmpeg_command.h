// The parts of the FFmpeg backend that are decisions rather than plumbing
// (architecture section 7.2).
//
// Building an argument list, reading a progress block, and reading what ffprobe
// said are all pure functions from strings to strings. Keeping them here means
// they can be tested exhaustively without a binary, a codec, or a machine that
// happens to have FFmpeg installed -- and the process plumbing next door has
// nothing left in it worth arguing about.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "morph/core/edge.h"
#include "morph/core/options.h"

namespace morph::ffmpeg {

// One format this backend handles. Container and codec are separate because
// section 4.1 makes them separate nodes: a WebM holding VP9 and one holding AV1
// are a full re-encode apart.
struct Format {
    std::string_view profile;
    std::string_view codec;      // our name, matching MediaProfile's codec attribute
    std::string_view encoder;    // the ffmpeg encoder to ask for, e.g. "libvpx-vp9"
    std::string_view muxer;      // the -f value; empty means "infer from the name"
    std::string_view extension;  // what the temp file has to be called
    bool video = false;
    bool lossy = true;
    bool encode = true;

    float cpuPerMiB = 0.0f;
    float memPeakFactor = 1.0f;
};

[[nodiscard]] std::span<const Format> Formats() noexcept;
[[nodiscard]] const Format* FindFormat(std::string_view profile) noexcept;

// Whether the samples can be copied rather than re-encoded (section 7.2's remux
// fast path). Same codec, different container, and the target is one we write.
[[nodiscard]] bool CanRemux(std::string_view from, std::string_view to) noexcept;

// The command line, without the executable.
//
// Section 7.2's own invocation is the starting point:
//
//   ffmpeg -nostdin -hide_banner -loglevel error -progress pipe:1
//          -stats_period 0.25 -i "in.mov" -c:v libvpx-vp9 -crf 32 -b:v 0
//          -c:a libopus "out.webm"
//
// -nostdin matters more than it looks: without it a child that decides to read
// the console steals the parent's stdin and hangs a batch conversion forever.
[[nodiscard]] std::vector<std::wstring> BuildArguments(const Format& target,
                                                       const std::wstring& inputPath,
                                                       const std::wstring& outputPath,
                                                       const OptionBag& options, bool remux);

// One line of `-progress pipe:1` output, folded into a running state.
//
// The format is key=value blocks terminated by "progress=continue" or
// "progress=end". Section 7.2: "Parse those, not stderr" -- stderr is a human
// log that changes between releases and says nothing machine-readable about how
// far along it is.
struct ProgressState {
    std::uint64_t microseconds = 0;  // out_time_us
    std::uint64_t bytes = 0;         // total_size
    std::uint64_t frames = 0;
    bool finished = false;           // progress=end
};

// Returns true when the line changed anything, so a caller can report only when
// there is something to report.
bool ApplyProgressLine(std::string_view line, ProgressState& state);

// One `codec_type,codec_name` row from
// `ffprobe -show_entries stream=codec_type,codec_name -of csv=p=0`.
//
// CSV rather than JSON on purpose: this is the only structured output FFmpeg
// gives that can be read without a parser, and a JSON dependency to learn two
// strings per stream would be a poor trade.
struct StreamInfo {
    std::string type;   // "video" or "audio"
    std::string codec;  // ffmpeg's name: "h264", "vp9", "opus"
};

[[nodiscard]] std::vector<StreamInfo> ParseProbeOutput(std::string_view text);

// The media profile those streams add up to, given the container the bytes said
// it was. Empty when nothing in the file is something this backend handles.
//
// The video stream decides when there is one, because a WebM with VP9 video and
// Opus audio is a video/webm;codec=vp9 -- the audio codec is not part of the
// node, and pretending otherwise would double the node count for nothing.
[[nodiscard]] std::string ProfileFor(std::string_view container,
                                     const std::vector<StreamInfo>& streams);

// The container a byte prefix identifies: "video/webm", "audio/ogg", and so on.
// Empty when it is not one of ours.
[[nodiscard]] std::string_view IdentifyContainer(std::span<const std::byte> prefix) noexcept;

}  // namespace morph::ffmpeg
