// Morph — the image pipeline (architecture section 6).
//
// "WIC is the right answer here and it is not a compromise. It ships in the OS,
//  it is COM so it is native to a C++ codebase, it handles colour management and
//  metadata properly, and it is the same component Explorer and Photos use, so
//  codec support tracks the OS for free."
//
// One provider, N x N edges. Images do not route through the document IR: a
// decoded bitmap IS the intermediate, and it lives inside a single Convert call
// rather than between two hops. That is why this is a byte provider (section 5)
// and the document formats are not.
#pragma once

#include <string>
#include <vector>

#include "morph/core/provider.h"

namespace morph::wic {

// Which image codecs this machine actually has.
//
// Section 6.2: "HEIF, AVIF, and to a degree WebP depend on Store-delivered codec
// packages. Probe at startup ... and mark those graph edges unavailable rather
// than failing mid-job." Decode and encode are tracked separately because they
// genuinely differ -- Windows 11 decodes WebP out of the box and, depending on
// the build, cannot encode it.
struct CodecSupport {
    bool heifDecode = false;
    bool heifEncode = false;
    bool webpDecode = false;
    bool webpEncode = false;
    bool avifDecode = false;
    bool avifEncode = false;

    // Capability ids for the Capabilities bag, so a UI can say which codec pack
    // is missing rather than "unavailable".
    [[nodiscard]] std::vector<std::string> Ids() const;
};

// Enumerates the installed WIC codecs. Cheap, and cached after the first call:
// a codec package can only appear or disappear across a process lifetime, and
// the worker's lifetime is one job batch (section 3.1).
[[nodiscard]] const CodecSupport& InstalledCodecs();

// image -> image, every pair the installed codecs support.
class WicProvider final : public IConversionProvider {
public:
    WicProvider();

    [[nodiscard]] std::string_view Id() const noexcept override { return "wic"; }

    [[nodiscard]] std::vector<EdgeDecl> EnumerateEdges() const override;

    [[nodiscard]] std::optional<MediaProfile> Probe(ISource& source,
                                                    const PathHint& hint) const override;

    [[nodiscard]] Result Convert(ConvertContext& context, ISource& source, ISink& sink,
                                 const MediaProfile& from, const MediaProfile& to) override;
};

// The profile a byte prefix identifies, or empty. Exposed for the tests, which
// need to assert that a renamed file is still identified by content (section 5).
[[nodiscard]] std::string_view IdentifyImage(std::span<const std::byte> prefix) noexcept;

}  // namespace morph::wic
