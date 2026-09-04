// Morph — the Media Foundation backend (architecture section 7.1).
//
// "One interface, two implementations, selected per edge by the graph. This is
//  what makes a Store SKU possible at all."
//
// This is the first of the two. MF uses the OS's own codecs, which matters for
// more than convenience: the encoders are hardware-accelerated where the GPU
// supports it, and the H.264/HEVC patent licensing sits with the platform rather
// than with us (section 7.3's note). Everything MF cannot do -- Matroska, WebM,
// AV1, Opus, FLAC encode -- is the FFmpeg backend's job, and the graph picks
// between them per edge without either knowing the other exists.
//
// Source Reader / Sink Writer rather than the topology API, as section 7.1
// specifies: it is far easier to drive from a worker thread and gives
// frame-level control for progress.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "morph/core/provider.h"

namespace morph::mf {

// What this machine's Media Foundation can actually encode.
//
// Same lesson the image pipeline learned the hard way: a codec that is
// registered is not a codec that works. HEVC needs the HEVC Video Extensions
// from the Store (section 7.3: "Requires the HEVC Video Extensions ... Probe and
// degrade"), and VP9 decodes with an extension while never encoding at all.
struct EncoderSupport {
    bool h264 = false;
    bool hevc = false;
    bool aac = false;
    bool wma = false;
    bool mp3 = false;
    // PCM has no encoder MFT -- the sink writer writes samples straight into a
    // WAV container -- so this is true wherever MF starts up at all.
    bool wav = false;

    [[nodiscard]] std::vector<std::string> Ids() const;
};

// Enumerates the encoders once and caches the answer. Media Foundation is
// started up by the first call and shut down when the process exits.
[[nodiscard]] const EncoderSupport& InstalledEncoders();

// Whether a route from `from` to `to` can copy compressed samples instead of
// decoding and re-encoding them.
//
// Section 7.2 calls this "the highest-value optimisation in the entire AV
// pipeline and it is fifty lines of code": a 4 GB MOV to MP4 becomes an IO-bound
// copy instead of a twenty-minute encode. It is exposed here, separately from
// the conversion, because it is a decision about two media profiles and is worth
// being able to check without a file.
[[nodiscard]] bool CanRemux(std::string_view from, std::string_view to) noexcept;

// The profile a byte prefix identifies, or empty. Container plus codec, because
// section 4.1 makes those different nodes: video/mp4;codec=h264 and
// video/mp4;codec=hevc are a re-encode apart.
//
// The codec cannot be read from a prefix alone for every container, so this
// answers with the container and the provider fills the codec in from the
// stream's media type when it opens it.
[[nodiscard]] std::string_view IdentifyContainer(std::span<const std::byte> prefix) noexcept;

class MediaFoundationProvider final : public IConversionProvider {
public:
    MediaFoundationProvider();

    [[nodiscard]] std::string_view Id() const noexcept override { return "mf"; }

    [[nodiscard]] std::vector<EdgeDecl> EnumerateEdges() const override;

    [[nodiscard]] std::optional<MediaProfile> Probe(ISource& source,
                                                    const PathHint& hint) const override;

    [[nodiscard]] Result Convert(ConvertContext& context, ISource& source, ISink& sink,
                                 const MediaProfile& from, const MediaProfile& to) override;
};

}  // namespace morph::mf
