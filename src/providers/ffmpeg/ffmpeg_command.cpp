#include "ffmpeg_command.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>

namespace morph::ffmpeg {
namespace {

// Section 7.2's build configuration decides this table, and the decisions are
// licensing ones as much as technical ones:
//
//   --disable-gpl --disable-nonfree --disable-libx264 --disable-libx265
//   --enable-libvpx --enable-libopus --enable-libvorbis --enable-libdav1d
//   --enable-libsvtav1 --enable-mediafoundation
//
// So there is no x264 here and there never will be: H.264 encode, if it is
// wanted at all through this backend, goes through h264_mf and is Media
// Foundation's job anyway. What FFmpeg is here for is the row of section 7.3's
// table that says NO in the Store column.
constexpr std::array<Format, 8> kFormats{{
    // container/codec          codec    encoder        muxer      ext     video  lossy encode
    {"video/webm;codec=vp9", "vp9", "libvpx-vp9", "webm", "webm", true, true, true,
     2600.0f, 3.0f},
    {"video/webm;codec=av1", "av1", "libsvtav1", "webm", "webm", true, true, true,
     3800.0f, 3.5f},
    // Matroska holds anything; these two are the pairs worth naming, and the
    // remux path is what makes them cheap.
    {"video/x-matroska;codec=h264", "h264", "copy", "matroska", "mkv", true, true, true,
     900.0f, 2.5f},
    {"video/x-matroska;codec=vp9", "vp9", "libvpx-vp9", "matroska", "mkv", true, true, true,
     2600.0f, 3.0f},

    {"audio/ogg;codec=opus", "opus", "libopus", "ogg", "opus", false, true, true,
     150.0f, 2.0f},
    {"audio/ogg;codec=vorbis", "vorbis", "libvorbis", "ogg", "ogg", false, true, true,
     150.0f, 2.0f},
    // The one lossless audio target Morph can write at all: Media Foundation
    // decodes FLAC and has never encoded it (section 7.3).
    {"audio/flac", "flac", "flac", "flac", "flac", false, false, true, 220.0f, 12.0f},
    // Read-only. AV1 in an MP4 is a real thing to be handed and not a thing this
    // backend should produce, because the MP4 nodes belong to Media Foundation
    // and two providers writing the same node is a tie the planner has no reason
    // to break.
    {"video/mp4;codec=av1", "av1", "", "mp4", "mp4", true, true, false, 3800.0f, 3.5f},
}};

std::wstring Widen(std::string_view text) {
    return std::wstring(text.begin(), text.end());
}

void Add(std::vector<std::wstring>& into, std::string_view flag) {
    into.push_back(Widen(flag));
}

void Add(std::vector<std::wstring>& into, std::string_view flag, std::string_view value) {
    into.push_back(Widen(flag));
    into.push_back(Widen(value));
}

std::uint64_t ParseUnsigned(std::string_view text) {
    std::uint64_t value = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc() ? value : 0;
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\r' || text.front() == '\n' ||
                             text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\r' || text.back() == '\n' ||
                             text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

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

bool CanRemux(std::string_view from, std::string_view to) noexcept {
    const Format* source = FindFormat(from);
    const Format* target = FindFormat(to);
    if (source == nullptr || target == nullptr) return false;
    if (source->profile == target->profile) return false;
    if (!target->encode) return false;
    if (source->video != target->video) return false;
    return source->codec == target->codec;
}

std::vector<std::wstring> BuildArguments(const Format& target, const std::wstring& inputPath,
                                         const std::wstring& outputPath,
                                         const OptionBag& options, bool remux) {
    std::vector<std::wstring> args;

    Add(args, "-nostdin");
    Add(args, "-hide_banner");
    Add(args, "-loglevel", "error");
    Add(args, "-progress", "pipe:1");
    Add(args, "-stats_period", "0.25");
    // Overwrite: the file is ours, in the temp store, and was just created.
    Add(args, "-y");

    args.push_back(L"-i");
    args.push_back(inputPath);

    if (remux) {
        // Section 7.2's fast path. Nothing is decoded, so nothing else matters:
        // no bitrate, no quality, no filter.
        Add(args, "-c", "copy");
    } else if (target.video) {
        Add(args, "-c:v", target.encoder);

        // Constant quality rather than a bitrate: with -b:v 0 this is how libvpx
        // and SVT-AV1 are meant to be driven, and it gives a consistent RESULT
        // across clips instead of a consistent file size.
        const std::int64_t quality = std::clamp<std::int64_t>(options.GetInt("quality", 32), 0,
                                                              63);
        Add(args, "-crf", std::to_string(quality));
        Add(args, "-b:v", "0");
        // Sound comes along, in the container's usual codec.
        Add(args, "-c:a", target.muxer == "webm" ? "libopus" : "copy");
    } else {
        Add(args, "-vn");  // an audio target keeps no picture
        Add(args, "-c:a", target.encoder);
        if (target.lossy) {
            const std::int64_t kbps =
                std::clamp<std::int64_t>(options.GetInt("audioBitrate", 192), 32, 320);
            Add(args, "-b:a", std::to_string(kbps) + "k");
        }
    }

    if (!target.muxer.empty()) Add(args, "-f", target.muxer);

    args.push_back(outputPath);
    return args;
}

bool ApplyProgressLine(std::string_view line, ProgressState& state) {
    line = Trim(line);
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) return false;

    const std::string_view key = line.substr(0, equals);
    const std::string_view value = Trim(line.substr(equals + 1));

    if (key == "out_time_us" || key == "out_time_ms") {
        // out_time_ms is a misnomer in FFmpeg: it has always been microseconds.
        // Treating it as milliseconds makes the bar finish a thousand times too
        // early, which is the kind of bug that looks like a rendering glitch.
        state.microseconds = ParseUnsigned(value);
        return true;
    }
    if (key == "total_size") {
        state.bytes = ParseUnsigned(value);
        return true;
    }
    if (key == "frame") {
        state.frames = ParseUnsigned(value);
        return true;
    }
    if (key == "progress") {
        if (value == "end") {
            state.finished = true;
            return true;
        }
        return value == "continue";
    }
    return false;
}

std::vector<StreamInfo> ParseProbeOutput(std::string_view text) {
    std::vector<StreamInfo> streams;

    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::string_view line =
            Trim(text.substr(start, newline == std::string_view::npos ? std::string_view::npos
                                                                      : newline - start));
        start = newline == std::string_view::npos ? text.size() + 1 : newline + 1;
        if (line.empty()) continue;

        const std::size_t comma = line.find(',');
        if (comma == std::string_view::npos) continue;

        StreamInfo info;
        info.type = std::string(Trim(line.substr(0, comma)));
        info.codec = std::string(Trim(line.substr(comma + 1)));
        if (info.type.empty() || info.codec.empty()) continue;
        streams.push_back(std::move(info));
    }
    return streams;
}

std::string ProfileFor(std::string_view container, const std::vector<StreamInfo>& streams) {
    if (container.empty()) return {};

    const auto video = std::find_if(streams.begin(), streams.end(), [](const StreamInfo& info) {
        return info.type == "video";
    });
    const auto audio = std::find_if(streams.begin(), streams.end(), [](const StreamInfo& info) {
        return info.type == "audio";
    });

    // FFmpeg's codec names and ours agree for everything in the table; the two
    // that do not are spelled out rather than guessed at.
    const auto normalise = [](std::string_view name) -> std::string_view {
        if (name == "vp09") return "vp9";
        if (name == "av01") return "av1";
        return name;
    };

    if (video != streams.end()) {
        const std::string candidate =
            std::string(container) + ";codec=" + std::string(normalise(video->codec));
        if (FindFormat(candidate) != nullptr) return candidate;
    }
    if (audio != streams.end()) {
        // An audio-only Matroska is an MKA, and an audio-only WebM is legal too;
        // both are named by their audio codec.
        const std::string candidate =
            std::string(container) + ";codec=" + std::string(normalise(audio->codec));
        if (FindFormat(candidate) != nullptr) return candidate;
        // FLAC's profile carries no codec attribute, because the container holds
        // exactly one thing.
        if (container == "audio/flac" && audio->codec == "flac") return "audio/flac";
    }
    return {};
}

std::string_view IdentifyContainer(std::span<const std::byte> prefix) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(prefix.data());
    const std::size_t size = prefix.size();

    const auto at = [&](std::size_t offset, std::string_view text) {
        if (size < offset + text.size()) return false;
        return std::memcmp(bytes + offset, text.data(), text.size()) == 0;
    };

    // EBML. Matroska and WebM share the magic and differ by DocType, which sits
    // a few bytes in -- close enough to the front that a prefix always has it.
    if (size >= 4 && bytes[0] == 0x1A && bytes[1] == 0x45 && bytes[2] == 0xDF &&
        bytes[3] == 0xA3) {
        const std::string_view head(reinterpret_cast<const char*>(bytes),
                                    std::min<std::size_t>(size, 64));
        if (head.find("webm") != std::string_view::npos) return "video/webm";
        return "video/x-matroska";
    }
    if (at(0, "OggS")) return "audio/ogg";
    if (at(0, "fLaC")) return "audio/flac";
    if (at(4, "ftyp") && size >= 12) {
        const std::string_view brand(reinterpret_cast<const char*>(bytes) + 8, 4);
        // Only AV1 in MP4 belongs to this backend; everything else in an MP4 is
        // Media Foundation's, and two providers claiming one node would make the
        // probe order decide which, which is not a decision worth having.
        if (brand == "av01") return "video/mp4";
    }
    return {};
}

}  // namespace morph::ffmpeg
