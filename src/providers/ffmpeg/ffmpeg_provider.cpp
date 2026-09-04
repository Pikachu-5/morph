#include "morph/providers/ffmpeg.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>

#include <windows.h>

#include "ffmpeg_command.h"
#include "ffmpeg_process.h"
#include "morph/core/ipc/pipe.h"
#include "morph/core/stream.h"
#include "morph/core/temp_store.h"

namespace morph::ffmpeg {
namespace {

// Where a binary may be. In order: an explicit override, beside the installed
// binaries, and the third_party directory a developer build fetches into.
//
// PATH is deliberately NOT searched. A converter that picks up whichever ffmpeg
// happens to be on a machine has no idea what it was built with -- section 14's
// licensing matrix depends on the GPL-free configuration in 7.2, and a
// user-supplied GPL build would quietly change what this package is.
std::filesystem::path FindTool(std::wstring_view name) {
    std::vector<std::filesystem::path> candidates;

    wchar_t override[MAX_PATH]{};
    if (::GetEnvironmentVariableW(L"MORPH_FFMPEG_DIR", override, MAX_PATH) > 0) {
        candidates.emplace_back(std::filesystem::path(override) / name);
    }

    const std::wstring directory = ipc::InstallDirectory();
    if (!directory.empty()) {
        candidates.emplace_back(std::filesystem::path(directory) / name);
        candidates.emplace_back(std::filesystem::path(directory) / L"ffmpeg" / name);
        // A developer build runs out of out/build/<preset>/bin; the fetched
        // binaries live in the source tree next to it.
        candidates.emplace_back(std::filesystem::path(directory) / L".." / L".." / L".." /
                                L".." / L"third_party" / L"ffmpeg" / L"bin" / name);
    }

    std::error_code ec;
    for (const std::filesystem::path& candidate : candidates) {
        if (std::filesystem::exists(candidate, ec)) {
            return std::filesystem::weakly_canonical(candidate, ec);
        }
    }
    return {};
}

Tools Discover() {
    Tools tools;
    tools.ffmpeg = FindTool(L"ffmpeg.exe");
    if (tools.ffmpeg.empty()) return tools;

    std::string output;
    if (const Result ran = RunAndCapture(tools.ffmpeg.wstring(), {L"-version"}, output);
        !ran || output.empty()) {
        // A file called ffmpeg.exe that cannot say what it is does not get used.
        tools.ffmpeg.clear();
        return tools;
    }

    // "ffmpeg version 7.1 Copyright ..." -- the first line, trimmed to something
    // a log line can carry.
    const std::size_t newline = output.find('\n');
    tools.version = output.substr(0, newline == std::string::npos ? output.size() : newline);
    while (!tools.version.empty() &&
           (tools.version.back() == '\r' || tools.version.back() == ' ')) {
        tools.version.pop_back();
    }
    if (tools.version.size() > 120) tools.version.resize(120);

    tools.ffprobe = FindTool(L"ffprobe.exe");
    return tools;
}

Result Failed(const RunResult& run, std::string_view what) {
    // FFmpeg says why on stderr, and with -loglevel error that is one line and
    // it is the real reason. Section 12.2: "Failures listed with a real reason,
    // never 'conversion failed'."
    std::string detail = run.diagnostics;
    const std::size_t newline = detail.find('\n');
    if (newline != std::string::npos) detail.resize(newline);
    while (!detail.empty() && (detail.back() == '\r' || detail.back() == ' ')) detail.pop_back();

    if (detail.empty()) {
        detail = "FFmpeg exited with code " + std::to_string(run.exitCode) + ".";
    }
    return Result::Error(Status::kMalformedInput, std::string(what) + ": " + detail);
}

// Copies a source into the temp store, because FFmpeg takes a path and Morph
// hands providers a stream.
//
// A pipe would avoid the copy and cost more than it saves: FFmpeg seeks in its
// input for every container worth having (an MP4's index is at the end, and
// Matroska seeks constantly), and a non-seekable input turns a remux into a
// failure or a full decode.
Result Spill(ISource& source, const std::filesystem::path& path) {
    FileSink sink(path);
    std::vector<std::byte> chunk(kStreamChunkBytes);
    while (true) {
        const std::size_t got = source.Read(std::span(chunk.data(), chunk.size()));
        if (got == 0) break;
        sink.Write(std::span(chunk.data(), got));
    }
    sink.Commit();
    return Result::Ok();
}

}  // namespace

const Tools& InstalledTools() {
    static const Tools tools = Discover();
    return tools;
}

FfmpegProvider::FfmpegProvider() { (void)InstalledTools(); }

std::vector<EdgeDecl> FfmpegProvider::EnumerateEdges() const {
    // No binary, no edges. This is the Store SKU (section 7.3), and it is the
    // same build: what differs is whether ffmpeg.exe shipped beside it.
    if (!InstalledTools().Present()) return {};

    std::vector<EdgeDecl> edges;
    for (const Format& from : Formats()) {
        const std::optional<MediaProfile> source = MediaProfile::Parse(from.profile);
        if (!source) continue;

        for (const Format& to : Formats()) {
            if (from.profile == to.profile) continue;
            if (!to.encode) continue;
            // Audio cannot become video; video to audio is a legitimate ask and
            // is offered.
            if (!from.video && to.video) continue;

            const std::optional<MediaProfile> target = MediaProfile::Parse(to.profile);
            if (!target) continue;

            EdgeCost cost;
            if (CanRemux(from.profile, to.profile)) {
                cost.fidelity = Fidelity::Lossless;
                cost.generational = 0;
                cost.cpuPerMiB = 4.0f;
                cost.memPeakFactor = 1.3f;
            } else if (from.video && !to.video) {
                cost.fidelity = Fidelity::Structural;
                cost.generational = to.lossy ? 1 : 0;
                cost.cpuPerMiB = to.cpuPerMiB;
                cost.memPeakFactor = to.memPeakFactor;
            } else {
                cost.fidelity = to.lossy ? Fidelity::High : Fidelity::Lossless;
                cost.generational = to.lossy ? 1 : 0;
                cost.cpuPerMiB = from.cpuPerMiB + to.cpuPerMiB;
                cost.memPeakFactor = std::max(from.memPeakFactor, to.memPeakFactor);
            }
            // FFmpeg takes a path and writes a path. Both ends spill.
            cost.requiresDisk = true;

            edges.push_back(EdgeDecl{*source, *target, cost});
        }
    }
    return edges;
}

std::optional<MediaProfile> FfmpegProvider::Probe(ISource& source,
                                                  const PathHint& /*hint*/) const {
    const Tools& tools = InstalledTools();
    if (!tools.Present()) return std::nullopt;

    const std::vector<std::byte> prefix = ReadAtMost(source, 64);
    const std::string_view container = IdentifyContainer(prefix);
    if (container.empty()) return std::nullopt;

    // The container is in the magic; the codec is not, and a node without one is
    // not a node. Matroska in particular holds anything, so guessing from the
    // container would put the planner on a route the file cannot take.
    //
    // ffprobe answers in a few milliseconds, and this is a probe -- section 5
    // budgets a light structural check, and one process spawn is within it for a
    // format whose alternative is parsing EBML by hand.
    if (tools.ffprobe.empty()) return std::nullopt;
    if (!source.Seek(0)) return std::nullopt;

    ScopedTempStore temp("probe");
    const std::filesystem::path spilled =
        temp.Acquire(std::string("probe.") + std::string(container.substr(
                         container.find('/') + 1)));
    if (const Result copied = Spill(source, spilled); !copied) return std::nullopt;

    std::string output;
    const std::vector<std::wstring> arguments = {
        L"-v", L"error", L"-show_entries", L"stream=codec_type,codec_name",
        L"-of", L"csv=p=0", spilled.wstring()};
    if (const Result ran = RunAndCapture(tools.ffprobe.wstring(), arguments, output); !ran) {
        return std::nullopt;
    }

    const std::string profile = ProfileFor(container, ParseProbeOutput(output));
    if (profile.empty()) return std::nullopt;
    return MediaProfile::Parse(profile);
}

Result FfmpegProvider::Convert(ConvertContext& context, ISource& source, ISink& sink,
                               const MediaProfile& from, const MediaProfile& to) {
    const Tools& tools = InstalledTools();
    if (!tools.Present()) {
        return Result::Error(Status::kMissingCapability,
                             "This build of Morph does not include FFmpeg, so it cannot write " +
                                 to.Id() + ".");
    }

    const Format* source_format = FindFormat(from.Id());
    const Format* target = FindFormat(to.Id());
    if (source_format == nullptr || target == nullptr || !target->encode) {
        return Result::Error(Status::kMissingCapability,
                             "Morph does not write " + to.Id() + " through FFmpeg.");
    }

    Result result = Result::Ok();
    context.progress.Report(0.0, 0);

    const std::filesystem::path input =
        context.temp.Acquire("ffmpeg-in." + std::string(source_format->extension));
    if (const Result spilled = Spill(source, input); !spilled) return spilled;
    if (context.cancelled()) return Result::Cancelled();

    const std::filesystem::path output =
        context.temp.Acquire("ffmpeg-out." + std::string(target->extension));

    const bool remux = CanRemux(from.Id(), to.Id());
    if (remux) {
        result.Note("av.remuxed", "The streams were copied without re-encoding.", "");
    } else if (target->lossy) {
        result.Note("av.transcoded",
                    std::string(target->video ? "The video" : "The audio") +
                        " was re-encoded as " + std::string(target->codec) + ".",
                    "");
        result.Incurred(Fidelity::High);
    }
    if (source_format->video && !target->video) {
        result.Note("av.video-dropped", "The picture was discarded; only the sound was kept.",
                    "");
        result.Incurred(Fidelity::Structural);
    }

    // The duration is not known up front -- ffprobe could say, but that is a
    // second spawn for a number the progress stream itself converges on. So the
    // bar is driven by the input's own duration when FFmpeg reports one, and
    // falls back to bytes written when it does not.
    std::uint64_t durationUs = 0;
    ProgressState state;

    const auto onLine = [&](std::string_view line) {
        if (!ApplyProgressLine(line, state)) return;
        if (durationUs == 0) return;
        const double fraction =
            static_cast<double>(state.microseconds) / static_cast<double>(durationUs);
        context.progress.Report(std::clamp(fraction, 0.0, 0.99), state.bytes);
    };

    // One ffprobe call for the duration, only when there is a progress sink that
    // can use it. Cheap, and the alternative is a bar that never moves.
    if (!tools.ffprobe.empty()) {
        std::string probed;
        const std::vector<std::wstring> arguments = {
            L"-v", L"error", L"-show_entries", L"format=duration",
            L"-of", L"csv=p=0", input.wstring()};
        if (RunAndCapture(tools.ffprobe.wstring(), arguments, probed)) {
            durationUs = static_cast<std::uint64_t>(std::strtod(probed.c_str(), nullptr) *
                                                    1'000'000.0);
        }
    }

    RunResult run;
    const Result ran = RunProcess(
        tools.ffmpeg.wstring(),
        BuildArguments(*target, input.wstring(), output.wstring(), context.options, remux),
        onLine, [&context] { return context.cancelled(); }, run);

    if (run.cancelled) return Result::Cancelled();
    if (!ran) return ran;
    if (run.exitCode != 0) return Failed(run, "Converting to " + to.Id());

    std::error_code ec;
    if (!std::filesystem::exists(output, ec) || std::filesystem::file_size(output, ec) == 0) {
        return Failed(run, "Converting to " + to.Id());
    }

    FileSource produced(output);
    std::vector<std::byte> chunk(kStreamChunkBytes);
    std::uint64_t copied = 0;
    while (true) {
        const std::size_t got = produced.Read(std::span(chunk.data(), chunk.size()));
        if (got == 0) break;
        sink.Write(std::span(chunk.data(), got));
        copied += got;
    }
    sink.Commit();
    context.progress.Report(1.0, copied);
    return result;
}

}  // namespace morph::ffmpeg
