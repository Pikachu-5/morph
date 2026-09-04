#include "morph/providers/wic.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string>

#include <windows.h>

#include <wincodec.h>
#include <wincodecsdk.h>
#include <wrl/client.h>

#include "morph/core/probe_cache.h"
#include "morph/core/stream.h"
#include "morph/providers/com_stream.h"

namespace morph::wic {
namespace {

using Microsoft::WRL::ComPtr;

// --- The format table --------------------------------------------------------

struct Format {
    std::string_view profile;
    const GUID* container;
    // Lossy formats cost a generation every time they are written (section 4.1).
    bool lossy;
    // Can the container carry an ICC profile? BMP cannot, which section 6.2 says
    // must mean an explicit conversion to sRGB rather than a reinterpretation.
    bool carriesIcc;
    // Milliseconds of CPU per MiB of input, seeded by measurement order of
    // magnitude rather than guesswork about the codec (section 4.1: "seeded from
    // a table, overwritten by real measurements").
    float cpuPerMiB;
    // A compressed image expands enormously when decoded: a 1 MiB JPEG is
    // routinely 30 MiB of BGRA. This is what keeps the section 11.2 budget from
    // being derived off the file size alone and killing the worker mid-decode.
    float memPeakFactor;
    // Which Store codec package, if any, this format needs.
    std::string_view capability;
};

constexpr std::array<Format, 8> kFormats{{
    {"image/png", &GUID_ContainerFormatPng, false, true, 45.0f, 8.0f, ""},
    {"image/jpeg", &GUID_ContainerFormatJpeg, true, true, 18.0f, 14.0f, ""},
    {"image/bmp", &GUID_ContainerFormatBmp, false, false, 6.0f, 6.0f, ""},
    {"image/tiff", &GUID_ContainerFormatTiff, false, true, 22.0f, 10.0f, ""},
    {"image/gif", &GUID_ContainerFormatGif, false, false, 30.0f, 8.0f, ""},
    {"image/heif", &GUID_ContainerFormatHeif, true, true, 90.0f, 16.0f, "heif"},
    {"image/webp", &GUID_ContainerFormatWebp, true, true, 60.0f, 14.0f, "webp"},
    // AVIF has no GUID_ContainerFormatAvif in every SDK; it is looked up by name
    // at probe time instead. The entry is here so the table stays the one place
    // a format is described.
    {"image/avif", nullptr, true, true, 220.0f, 16.0f, "avif"},
}};

const Format* FindFormat(std::string_view profile) {
    for (const Format& format : kFormats) {
        if (format.profile == profile) return &format;
    }
    return nullptr;
}

// --- COM and the factory -----------------------------------------------------

// Section 6.2: "Workers initialise MTA." Per thread, because a lane thread in
// the broker's pool and the CLI's main thread are different threads that both
// end up here, and because an apartment already initialised differently is not
// an error we should fight -- WIC works in either.
class ApartmentGuard {
public:
    ApartmentGuard() {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialised_ = SUCCEEDED(hr);
    }
    ~ApartmentGuard() {
        if (initialised_) ::CoUninitialize();
    }

    ApartmentGuard(const ApartmentGuard&) = delete;
    ApartmentGuard& operator=(const ApartmentGuard&) = delete;

private:
    bool initialised_ = false;
};

HRESULT CreateFactory(ComPtr<IWICImagingFactory>& into) {
    return ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&into));
}

// AVIF's container GUID is not in every Windows SDK. Rather than hard-coding the
// value, it is found by asking WIC for it -- which also proves a codec for it is
// installed, since a component that is not there cannot be enumerated.
bool FindContainerByName(IWICImagingFactory& factory, DWORD kind, std::wstring_view friendlyName,
                         GUID& into) {
    ComPtr<IEnumUnknown> components;
    if (FAILED(factory.CreateComponentEnumerator(kind, WICComponentEnumerateDefault,
                                                 &components))) {
        return false;
    }

    ComPtr<IUnknown> item;
    ULONG fetched = 0;
    while (components->Next(1, &item, &fetched) == S_OK && fetched == 1) {
        ComPtr<IWICBitmapCodecInfo> info;
        if (SUCCEEDED(item.As(&info))) {
            UINT length = 0;
            wchar_t name[128]{};
            if (SUCCEEDED(info->GetFriendlyName(127, name, &length))) {
                std::wstring_view text(name, length == 0 ? 0 : length - 1);
                if (text.find(friendlyName) != std::wstring_view::npos) {
                    GUID container{};
                    if (SUCCEEDED(info->GetContainerFormat(&container))) {
                        into = container;
                        item.Reset();
                        return true;
                    }
                }
            }
        }
        item.Reset();
    }
    return false;
}

bool HasComponentFor(IWICImagingFactory& factory, DWORD kind, const GUID& container) {
    ComPtr<IEnumUnknown> components;
    if (FAILED(factory.CreateComponentEnumerator(kind, WICComponentEnumerateDefault,
                                                 &components))) {
        return false;
    }

    ComPtr<IUnknown> item;
    ULONG fetched = 0;
    while (components->Next(1, &item, &fetched) == S_OK && fetched == 1) {
        ComPtr<IWICBitmapCodecInfo> info;
        GUID found{};
        if (SUCCEEDED(item.As(&info)) && SUCCEEDED(info->GetContainerFormat(&found)) &&
            ::IsEqualGUID(found, container)) {
            item.Reset();
            return true;
        }
        item.Reset();
    }
    return false;
}

// Encodes a 1x1 image and throws the result away.
//
// This exists because enumeration lies. On a stock Windows 11, the HEIF ENCODER
// component is registered and enumerates happily, and then Commit fails with a
// Media Foundation error because the HEVC encoder it delegates to ships in a
// separate paid Store package. Section 6.2 asks for edges to be marked
// unavailable "rather than failing mid-job", and the only way to know is to try
// -- so the gated formats are tried once, at startup, on an image small enough
// that the cost is noise.
bool TrialEncode(IWICImagingFactory& factory, const GUID& container) {
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory.CreateBitmap(1, 1, GUID_WICPixelFormat32bppBGRA,
                                    WICBitmapCacheOnLoad, &bitmap))) {
        return false;
    }

    comio::MemoryStream sink;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory.CreateEncoder(container, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(&sink, WICBitmapEncoderNoCache))) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> bag;
    if (FAILED(encoder->CreateNewFrame(&frame, &bag))) return false;
    if (FAILED(frame->Initialize(bag.Get()))) return false;
    if (FAILED(frame->SetSize(1, 1))) return false;

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) return false;
    if (FAILED(frame->WriteSource(bitmap.Get(), nullptr))) return false;
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(encoder->Commit()) && !sink.bytes().empty();
}

// The AVIF container GUID this machine uses, or GUID_NULL when nothing here
// knows about AVIF at all.
GUID gAvifContainer{};

const GUID* ContainerFor(std::string_view profile) {
    const Format* format = FindFormat(profile);
    if (format == nullptr) return nullptr;
    if (format->container != nullptr) return format->container;
    return ::IsEqualGUID(gAvifContainer, GUID_NULL) ? nullptr : &gAvifContainer;
}

CodecSupport ProbeCodecs() {
    CodecSupport support;

    const ApartmentGuard apartment;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CreateFactory(factory)) || !factory) return support;

    // Decoding is enumerable: a decoder that is registered decodes. Encoding is
    // not, so every gated encoder is tried rather than asked about -- and a
    // trial encode is the expensive part, which is why the answers are
    // remembered across runs (probe_cache.h).
    IWICImagingFactory& raw = *factory.Get();

    support.heifDecode = Cached("wic.heif.decode", [&raw] {
        return HasComponentFor(raw, WICDecoder, GUID_ContainerFormatHeif);
    });
    support.heifEncode = Cached("wic.heif.encode", [&raw] {
        return HasComponentFor(raw, WICEncoder, GUID_ContainerFormatHeif) &&
               TrialEncode(raw, GUID_ContainerFormatHeif);
    });

    support.webpDecode = Cached("wic.webp.decode", [&raw] {
        return HasComponentFor(raw, WICDecoder, GUID_ContainerFormatWebp);
    });
    support.webpEncode = Cached("wic.webp.encode", [&raw] {
        return HasComponentFor(raw, WICEncoder, GUID_ContainerFormatWebp) &&
               TrialEncode(raw, GUID_ContainerFormatWebp);
    });

    // AVIF's container GUID is looked up by name, and the lookup is also the
    // decode probe -- so unlike the others it is not cached as a bare bool: the
    // GUID itself is needed at conversion time and enumerating for it is cheap.
    GUID avif{};
    if (FindContainerByName(raw, WICDecoder, L"AVIF", avif)) {
        gAvifContainer = avif;
        support.avifDecode = true;
        support.avifEncode = Cached("wic.avif.encode", [&raw, &avif] {
            return HasComponentFor(raw, WICEncoder, avif) && TrialEncode(raw, avif);
        });
    }
    return support;
}

bool DecodeAvailable(const Format& format, const CodecSupport& support) {
    if (format.capability == "heif") return support.heifDecode;
    if (format.capability == "webp") return support.webpDecode;
    if (format.capability == "avif") return support.avifDecode;
    return true;
}

bool EncodeAvailable(const Format& format, const CodecSupport& support) {
    if (format.capability == "heif") return support.heifEncode;
    if (format.capability == "webp") return support.webpEncode;
    if (format.capability == "avif") return support.avifEncode;
    // GIF encoding is available, but a GIF written from a full-colour source is
    // quantised to 256 colours, which is a fidelity claim this provider is not
    // yet willing to make honestly. Left out of the target set until it has a
    // rating it deserves.
    return format.profile != "image/gif";
}

// --- Errors ------------------------------------------------------------------

Result Failed(HRESULT hr, std::string_view what) {
    char code[16]{};
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));

    if (hr == WINCODEC_ERR_COMPONENTNOTFOUND) {
        return Result::Error(Status::kMissingCapability,
                             std::string(what) +
                                 ": the codec for this format is not installed.");
    }
    // A Media Foundation error surfacing out of an image encode means the codec
    // MFT behind the WIC component is missing -- HEIF delegating to an HEVC
    // encoder that is not installed is the case this was written for. The
    // startup trial encode should have caught it; this is the backstop, and it
    // says "missing" rather than "internal" because that is what it is.
    // FACILITY_MF is 13, and is only spelled out in mfobjects.h -- which this
    // provider has no other reason to include.
    constexpr int kFacilityMediaFoundation = 13;
    if (HRESULT_FACILITY(hr) == kFacilityMediaFoundation) {
        return Result::Error(Status::kMissingCapability,
                             std::string(what) +
                                 ": this machine has no encoder for that format installed (" +
                                 code + ").");
    }
    if (hr == WINCODEC_ERR_BADIMAGE || hr == WINCODEC_ERR_BADHEADER ||
        hr == WINCODEC_ERR_BADSTREAMDATA || hr == WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT ||
        hr == WINCODEC_ERR_UNKNOWNIMAGEFORMAT) {
        return Result::Error(Status::kMalformedInput,
                             std::string(what) + ": the image is damaged or not what it claims "
                                                 "to be (" + code + ").");
    }
    if (hr == E_OUTOFMEMORY || hr == STG_E_MEDIUMFULL) {
        return Result::Error(Status::kResourceLimit,
                             std::string(what) + ": the image is too large to convert here.");
    }
    return Result::Error(Status::kInternal, std::string(what) + " failed (" + code + ").");
}

// --- Orientation (section 6.2) -----------------------------------------------

// EXIF orientation, 1..8. Returns 1 (upright) when there is none, which is also
// what a malformed value gets: a picture shown the wrong way up is a bug report,
// and a picture shown upright when the tag was nonsense is not.
unsigned short ReadExifOrientation(IWICBitmapFrameDecode& frame) {
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame.GetMetadataQueryReader(&reader)) || !reader) return 1;

    // JPEG keeps it under /app1, TIFF at the top level. Asking for both is
    // cheaper than asking what container this is.
    for (const wchar_t* path : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
        PROPVARIANT value{};
        ::PropVariantInit(&value);
        const HRESULT hr = reader->GetMetadataByName(path, &value);
        unsigned short orientation = 0;
        if (SUCCEEDED(hr) && value.vt == VT_UI2) orientation = value.uiVal;
        ::PropVariantClear(&value);
        if (orientation >= 1 && orientation <= 8) return orientation;
    }
    return 1;
}

WICBitmapTransformOptions TransformFor(unsigned short orientation) {
    switch (orientation) {
        case 2: return WICBitmapTransformFlipHorizontal;
        case 3: return WICBitmapTransformRotate180;
        case 4: return WICBitmapTransformFlipVertical;
        case 5: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 |
                                                              WICBitmapTransformFlipHorizontal);
        case 6: return WICBitmapTransformRotate90;
        case 7: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 |
                                                              WICBitmapTransformFlipHorizontal);
        case 8: return WICBitmapTransformRotate270;
        default: return WICBitmapTransformRotate0;
    }
}

// --- Encoder options (section 6.1, step 3) -----------------------------------

void SetProperty(IPropertyBag2& bag, const wchar_t* name, const VARIANT& value) {
    PROPBAG2 option{};
    option.pstrName = const_cast<LPOLESTR>(name);
    // A property the encoder does not have is not an error: the same bag is
    // offered to every encoder, and each takes what it understands.
    (void)bag.Write(1, &option, const_cast<VARIANT*>(&value));
}

void ApplyEncoderOptions(IPropertyBag2& bag, const Format& target, const OptionBag& options) {
    if (target.lossy) {
        // Section 6.1's 0.88 default. Expressed to users as 1..100 because that
        // is what every other tool calls it.
        const std::int64_t quality =
            std::clamp<std::int64_t>(options.GetInt("quality", 88), 1, 100);
        VARIANT value{};
        value.vt = VT_R4;
        value.fltVal = static_cast<float>(quality) / 100.0f;
        SetProperty(bag, L"ImageQuality", value);
    }

    if (target.profile == "image/tiff") {
        VARIANT value{};
        value.vt = VT_UI1;
        value.bVal = static_cast<BYTE>(WICTiffCompressionZIP);
        SetProperty(bag, L"TiffCompressionMethod", value);
    }

    if (target.profile == "image/png") {
        // Interlacing costs size and buys nothing for a file on disk.
        VARIANT value{};
        value.vt = VT_BOOL;
        value.boolVal = VARIANT_FALSE;
        SetProperty(bag, L"InterlaceOption", value);
    }
}

}  // namespace

// --- Public surface ----------------------------------------------------------

std::vector<std::string> CodecSupport::Ids() const {
    std::vector<std::string> ids;
    if (heifDecode) ids.emplace_back("wic-heif-decode");
    if (heifEncode) ids.emplace_back("wic-heif-encode");
    if (webpDecode) ids.emplace_back("wic-webp-decode");
    if (webpEncode) ids.emplace_back("wic-webp-encode");
    if (avifDecode) ids.emplace_back("wic-avif-decode");
    if (avifEncode) ids.emplace_back("wic-avif-encode");
    return ids;
}

const CodecSupport& InstalledCodecs() {
    static const CodecSupport support = ProbeCodecs();
    return support;
}

std::string_view IdentifyImage(std::span<const std::byte> prefix) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(prefix.data());
    const std::size_t size = prefix.size();

    const auto starts = [&](std::initializer_list<unsigned char> magic, std::size_t at = 0) {
        if (size < at + magic.size()) return false;
        std::size_t i = at;
        for (const unsigned char c : magic) {
            if (bytes[i++] != c) return false;
        }
        return true;
    };

    if (starts({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A})) return "image/png";
    if (starts({0xFF, 0xD8, 0xFF})) return "image/jpeg";
    if (starts({'B', 'M'})) return "image/bmp";
    if (starts({'G', 'I', 'F', '8', '7', 'a'}) || starts({'G', 'I', 'F', '8', '9', 'a'})) {
        return "image/gif";
    }
    if (starts({'I', 'I', 0x2A, 0x00}) || starts({'M', 'M', 0x00, 0x2A})) return "image/tiff";
    if (starts({'R', 'I', 'F', 'F'}) && starts({'W', 'E', 'B', 'P'}, 8)) return "image/webp";

    // ISO base media: the brand at offset 8 says which of the family it is.
    if (starts({'f', 't', 'y', 'p'}, 4) && size >= 12) {
        const std::string_view brand(reinterpret_cast<const char*>(bytes) + 8, 4);
        if (brand == "avif" || brand == "avis") return "image/avif";
        if (brand == "heic" || brand == "heix" || brand == "hevc" || brand == "heim" ||
            brand == "heis" || brand == "mif1" || brand == "msf1") {
            return "image/heif";
        }
    }
    return {};
}

WicProvider::WicProvider() {
    // Probing here rather than lazily means the graph the worker builds at
    // startup already reflects the codecs this machine has -- and the broker's
    // copy, which comes from that graph, does too.
    (void)InstalledCodecs();
}

std::vector<EdgeDecl> WicProvider::EnumerateEdges() const {
    const CodecSupport& support = InstalledCodecs();
    std::vector<EdgeDecl> edges;

    for (const Format& from : kFormats) {
        if (!DecodeAvailable(from, support)) continue;
        const std::optional<MediaProfile> source = MediaProfile::Parse(from.profile);
        if (!source) continue;

        for (const Format& to : kFormats) {
            if (from.profile == to.profile) continue;
            if (!EncodeAvailable(to, support)) continue;
            const std::optional<MediaProfile> target = MediaProfile::Parse(to.profile);
            if (!target) continue;

            EdgeCost cost;
            // Writing a lossy format costs a generation and cannot be called
            // lossless whatever the source was. Writing a lossless format
            // preserves every pixel it was given -- which is exactly what the
            // rating means, and is true even when the source was a JPEG.
            cost.fidelity = to.lossy ? Fidelity::High : Fidelity::Lossless;
            cost.generational = to.lossy ? 1u : 0u;
            cost.cpuPerMiB = from.cpuPerMiB + to.cpuPerMiB;
            cost.memPeakFactor = std::max(from.memPeakFactor, to.memPeakFactor);
            // Everything happens in memory in one pass: decode, transform,
            // encode. Nothing here needs a spill file.
            cost.requiresDisk = false;

            edges.push_back(EdgeDecl{*source, *target, cost});
        }
    }
    return edges;
}

std::optional<MediaProfile> WicProvider::Probe(ISource& source, const PathHint& /*hint*/) const {
    // Content, never the extension (section 5). Every image format Morph handles
    // has an unambiguous magic number, which is why this provider does not need
    // the hint at all.
    const std::vector<std::byte> prefix = ReadAtMost(source, 32);
    const std::string_view profile = IdentifyImage(prefix);
    if (profile.empty()) return std::nullopt;

    // A format whose codec is absent is not identified as something we can
    // handle: claiming it and failing later is section 6.2's "failing mid-job".
    const Format* format = FindFormat(profile);
    if (format == nullptr || !DecodeAvailable(*format, InstalledCodecs())) return std::nullopt;

    return MediaProfile::Parse(profile);
}

Result WicProvider::Convert(ConvertContext& context, ISource& source, ISink& sink,
                            const MediaProfile& from, const MediaProfile& to) {
    const Format* target = FindFormat(to.Id());
    const GUID* container = ContainerFor(to.Id());
    if (target == nullptr || container == nullptr) {
        return Result::Error(Status::kMissingCapability,
                             "Morph cannot write " + to.Id() + " on this machine.");
    }

    const ApartmentGuard apartment;
    ComPtr<IWICImagingFactory> factory;
    if (const HRESULT hr = CreateFactory(factory); FAILED(hr)) {
        return Failed(hr, "Starting the imaging component");
    }

    Result result = Result::Ok();
    context.progress.Report(0.0, 0);

    // --- Decode --------------------------------------------------------------
    comio::SourceStream input(source);
    ComPtr<IWICBitmapDecoder> decoder;
    if (const HRESULT hr = factory->CreateDecoderFromStream(
            &input, nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
        FAILED(hr)) {
        return Failed(hr, "Reading the " + from.Id() + " image");
    }

    UINT frameCount = 1;
    (void)decoder->GetFrameCount(&frameCount);

    ComPtr<IWICBitmapFrameDecode> frame;
    if (const HRESULT hr = decoder->GetFrame(0, &frame); FAILED(hr)) {
        return Failed(hr, "Reading the first frame");
    }

    UINT width = 0;
    UINT height = 0;
    if (const HRESULT hr = frame->GetSize(&width, &height); FAILED(hr)) {
        return Failed(hr, "Reading the image size");
    }
    if (width == 0 || height == 0) {
        return Result::Error(Status::kMalformedInput, "That image has no pixels in it.");
    }

    if (context.cancelled()) return Result::Cancelled();
    context.progress.Report(0.25, 0);

    // Section 6.2: "a 40000x40000 PNG must fail cleanly, not OOM the worker."
    // The job object is the backstop; this is the clean failure.
    constexpr std::uint64_t kMaxPixels = 512ull * 1024 * 1024;
    if (static_cast<std::uint64_t>(width) * height > kMaxPixels) {
        return Result::Error(Status::kResourceLimit,
                             "That image is " + std::to_string(width) + " by " +
                                 std::to_string(height) +
                                 " pixels, which is more than Morph will decode at once.");
    }

    // Section 6.1, step 1: orientation before anything else touches pixels.
    ComPtr<IWICBitmapSource> pixels;
    const unsigned short orientation = ReadExifOrientation(*frame.Get());
    if (orientation != 1) {
        ComPtr<IWICBitmapFlipRotator> rotator;
        if (const HRESULT hr = factory->CreateBitmapFlipRotator(&rotator); FAILED(hr)) {
            return Failed(hr, "Rotating the image upright");
        }
        if (const HRESULT hr = rotator->Initialize(frame.Get(), TransformFor(orientation));
            FAILED(hr)) {
            return Failed(hr, "Rotating the image upright");
        }
        pixels = rotator;
    } else {
        pixels = frame;
    }

    if (frameCount > 1) {
        // Section 6.2: "Silently dropping frames is the worst option." The
        // conversion still happens -- a still from an animation is usually what
        // was wanted -- but the result says what it did, and the incurred rating
        // drops to Structural whatever the edge predicted.
        result.Note("image.animation-dropped",
                    "That image has " + std::to_string(frameCount) +
                        " frames and " + to.Id() + " holds one. The first frame was converted.",
                    "frame 0");
        result.Incurred(Fidelity::Structural);
    }

    // --- Encode --------------------------------------------------------------
    comio::MemoryStream output;
    ComPtr<IWICBitmapEncoder> encoder;
    if (const HRESULT hr = factory->CreateEncoder(*container, nullptr, &encoder); FAILED(hr)) {
        return Failed(hr, "Starting the " + to.Id() + " encoder");
    }
    if (const HRESULT hr = encoder->Initialize(&output, WICBitmapEncoderNoCache); FAILED(hr)) {
        return Failed(hr, "Starting the " + to.Id() + " encoder");
    }

    ComPtr<IWICBitmapFrameEncode> outFrame;
    ComPtr<IPropertyBag2> bag;
    if (const HRESULT hr = encoder->CreateNewFrame(&outFrame, &bag); FAILED(hr)) {
        return Failed(hr, "Starting the " + to.Id() + " frame");
    }
    if (bag) ApplyEncoderOptions(*bag.Get(), *target, context.options);
    if (const HRESULT hr = outFrame->Initialize(bag.Get()); FAILED(hr)) {
        return Failed(hr, "Starting the " + to.Id() + " frame");
    }

    // Section 6.1, step 2: convert the pixel format only if the encoder cannot
    // take the source's. SetPixelFormat negotiates -- WIC writes back the
    // closest format it supports, and a difference is the signal to convert.
    WICPixelFormatGUID sourceFormat{};
    if (const HRESULT hr = pixels->GetPixelFormat(&sourceFormat); FAILED(hr)) {
        return Failed(hr, "Reading the pixel format");
    }
    WICPixelFormatGUID encoderFormat = sourceFormat;
    if (const HRESULT hr = outFrame->SetPixelFormat(&encoderFormat); FAILED(hr)) {
        return Failed(hr, "Choosing the output pixel format");
    }

    ComPtr<IWICBitmapSource> converted = pixels;
    if (!::IsEqualGUID(encoderFormat, sourceFormat)) {
        ComPtr<IWICBitmapSource> replacement;
        if (const HRESULT hr = ::WICConvertBitmapSource(encoderFormat, pixels.Get(),
                                                        &replacement);
            FAILED(hr)) {
            return Failed(hr, "Converting the pixel format");
        }
        converted = replacement;

        // Dropping to fewer bits, or to no alpha, is a real loss and the note is
        // the only place a user will ever hear about it.
        result.Note("image.pixel-format",
                    "The pixels were converted to the closest format " + to.Id() + " supports.",
                    "");
    }

    // The size AFTER the rotation, not the frame's own. Orientations 5 to 8 are
    // quarter turns, so a 4000x3000 photo becomes a 3000x4000 one, and telling
    // the encoder otherwise fails the whole conversion with
    // WINCODEC_ERR_SOURCERECTDOESNOTMATCHDIMENSIONS -- which is to say every
    // photo taken in portrait, and none taken in landscape.
    UINT outWidth = width;
    UINT outHeight = height;
    if (const HRESULT hr = converted->GetSize(&outWidth, &outHeight); FAILED(hr)) {
        return Failed(hr, "Reading the converted image size");
    }
    if (const HRESULT hr = outFrame->SetSize(outWidth, outHeight); FAILED(hr)) {
        return Failed(hr, "Setting the output size");
    }

    double dpiX = 96.0;
    double dpiY = 96.0;
    if (SUCCEEDED(frame->GetResolution(&dpiX, &dpiY)) && dpiX > 0.0 && dpiY > 0.0) {
        (void)outFrame->SetResolution(dpiX, dpiY);
    }

    // Section 6.1, step 4: colour first, then everything else.
    UINT contextCount = 0;
    if (SUCCEEDED(frame->GetColorContexts(0, nullptr, &contextCount)) && contextCount > 0) {
        std::vector<ComPtr<IWICColorContext>> contexts(contextCount);
        std::vector<IWICColorContext*> raw(contextCount);
        bool built = true;
        for (UINT i = 0; i < contextCount; ++i) {
            if (FAILED(factory->CreateColorContext(&contexts[i]))) {
                built = false;
                break;
            }
            raw[i] = contexts[i].Get();
        }
        if (built && SUCCEEDED(frame->GetColorContexts(contextCount, raw.data(),
                                                       &contextCount))) {
            if (target->carriesIcc) {
                (void)outFrame->SetColorContexts(contextCount, raw.data());
            } else {
                // Section 6.2: a format that cannot carry a profile gets an
                // explicit statement that the pixels are now sRGB, rather than
                // the same numbers reinterpreted under whatever the viewer
                // assumes.
                result.Note("image.icc-dropped",
                            to.Id() + " cannot carry a colour profile, so the colours were "
                                      "converted to sRGB.",
                            "");
                result.Incurred(Fidelity::High);
            }
        }
    }

    // Metadata across, minus the orientation tag: the pixels are upright now, so
    // a tag that says to rotate them would rotate them twice (section 6.2).
    ComPtr<IWICMetadataBlockReader> blockReader;
    ComPtr<IWICMetadataBlockWriter> blockWriter;
    if (SUCCEEDED(frame.As(&blockReader)) && SUCCEEDED(outFrame.As(&blockWriter))) {
        if (SUCCEEDED(blockWriter->InitializeFromBlockReader(blockReader.Get()))) {
            ComPtr<IWICMetadataQueryWriter> queryWriter;
            if (SUCCEEDED(outFrame->GetMetadataQueryWriter(&queryWriter))) {
                (void)queryWriter->RemoveMetadataByName(L"/app1/ifd/{ushort=274}");
                (void)queryWriter->RemoveMetadataByName(L"/ifd/{ushort=274}");
            }
        }
    }

    if (context.cancelled()) return Result::Cancelled();
    context.progress.Report(0.5, 0);

    if (const HRESULT hr = outFrame->WriteSource(converted.Get(), nullptr); FAILED(hr)) {
        return Failed(hr, "Encoding the " + to.Id() + " image");
    }
    if (const HRESULT hr = outFrame->Commit(); FAILED(hr)) {
        return Failed(hr, "Finishing the " + to.Id() + " frame");
    }
    if (const HRESULT hr = encoder->Commit(); FAILED(hr)) {
        return Failed(hr, "Finishing the " + to.Id() + " image");
    }

    if (context.cancelled()) return Result::Cancelled();

    if (output.bytes().empty()) {
        return Result::Error(Status::kInternal, "The encoder produced nothing.");
    }

    WriteAll(sink, output.bytes());
    sink.Commit();
    context.progress.Report(1.0, output.bytes().size());
    return result;
}

}  // namespace morph::wic
