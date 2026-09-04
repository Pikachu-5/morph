// Morph — the FFmpeg backend (architecture section 7.2).
//
// "Everything MF cannot do: Matroska, WebM, AV1, Opus, Vorbis, Theora, ProRes,
//  obscure legacy containers, subtitle streams, and precise filter graphs.
//  Consumed as a subprocess, never linked."
//
// WHY SUBPROCESS AND NOT LIBAVCODEC, in section 7.2's own order of importance:
//
//   1. The LGPL relinking obligation evaporates -- we are not linking, we are
//      executing. Section 14's licensing matrix depends on this being true, and
//      it stops being true the moment anything here includes an FFmpeg header.
//   2. A malformed input that crashes a demuxer kills a child process we already
//      expect to die, not our worker.
//   3. FFmpeg can be updated for a security fix by replacing one file, with no
//      rebuild.
//
// The cost is 3-8 ms of process spawn per job, which is noise next to any real
// transcode.
//
// PRESENT OR ABSENT, NEVER ASSUMED. This provider declares no edges at all when
// no binary is found, which is exactly the Store SKU (section 7.3): the same
// build ships both ways, and what distinguishes them is whether ffmpeg.exe sits
// beside it. Nothing here fails at conversion time because a binary was missing;
// the route simply does not exist.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "morph/core/provider.h"

namespace morph::ffmpeg {

// Where the binaries are, if they are anywhere.
struct Tools {
    std::filesystem::path ffmpeg;
    std::filesystem::path ffprobe;
    // What `ffmpeg -version` reported, for diagnostics and for the capability
    // bag. Empty when nothing was found.
    std::string version;

    [[nodiscard]] bool Present() const noexcept { return !ffmpeg.empty() && !version.empty(); }
};

// Looks beside the installed binaries and in third_party, runs `-version`, and
// caches the answer for the life of the process.
[[nodiscard]] const Tools& InstalledTools();

class FfmpegProvider final : public IConversionProvider {
public:
    FfmpegProvider();

    [[nodiscard]] std::string_view Id() const noexcept override { return "ffmpeg"; }

    [[nodiscard]] std::vector<EdgeDecl> EnumerateEdges() const override;

    [[nodiscard]] std::optional<MediaProfile> Probe(ISource& source,
                                                    const PathHint& hint) const override;

    [[nodiscard]] Result Convert(ConvertContext& context, ISource& source, ISink& sink,
                                 const MediaProfile& from, const MediaProfile& to) override;
};

}  // namespace morph::ffmpeg
