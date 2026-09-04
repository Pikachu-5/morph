// The image pipeline (architecture section 6).
//
// The fixtures are built in the test rather than committed as files. A BMP is
// simple enough to write by hand, which gives an input whose exact pixels are
// known, and everything else is produced from it by the provider under test --
// so a round trip that comes back wrong is the provider's fault and not a
// fixture's.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include "morph/core/stream.h"
#include "morph/core/temp_store.h"
#include "morph/providers/wic.h"

using namespace morph;
using Microsoft::WRL::ComPtr;

namespace {

struct Pixel {
    unsigned char b = 0;
    unsigned char g = 0;
    unsigned char r = 0;
    unsigned char a = 255;

    friend bool operator==(const Pixel& x, const Pixel& y) noexcept {
        return x.b == y.b && x.g == y.g && x.r == y.r;
    }
};

void PutU32(std::vector<std::byte>& into, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        into.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFF));
    }
}

void PutU16(std::vector<std::byte>& into, std::uint16_t value) {
    into.push_back(static_cast<std::byte>(value & 0xFF));
    into.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
}

// A 24-bit uncompressed BMP, bottom-up, with each row padded to four bytes --
// which is the format's one real trap and worth writing out rather than
// generating with a library that would hide it.
std::vector<std::byte> MakeBmp(int width, int height, const std::vector<Pixel>& pixels) {
    REQUIRE(pixels.size() == static_cast<std::size_t>(width * height));

    const int rowBytes = ((width * 3 + 3) / 4) * 4;
    const std::uint32_t pixelBytes = static_cast<std::uint32_t>(rowBytes * height);
    const std::uint32_t offset = 14 + 40;

    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>('B'));
    out.push_back(static_cast<std::byte>('M'));
    PutU32(out, offset + pixelBytes);
    PutU32(out, 0);
    PutU32(out, offset);

    PutU32(out, 40);  // BITMAPINFOHEADER
    PutU32(out, static_cast<std::uint32_t>(width));
    PutU32(out, static_cast<std::uint32_t>(height));
    PutU16(out, 1);
    PutU16(out, 24);
    PutU32(out, 0);  // BI_RGB
    PutU32(out, pixelBytes);
    PutU32(out, 2835);
    PutU32(out, 2835);
    PutU32(out, 0);
    PutU32(out, 0);

    for (int y = height - 1; y >= 0; --y) {
        int written = 0;
        for (int x = 0; x < width; ++x) {
            const Pixel& pixel = pixels[static_cast<std::size_t>(y * width + x)];
            out.push_back(static_cast<std::byte>(pixel.b));
            out.push_back(static_cast<std::byte>(pixel.g));
            out.push_back(static_cast<std::byte>(pixel.r));
            written += 3;
        }
        while (written < rowBytes) {
            out.push_back(std::byte{0});
            ++written;
        }
    }
    return out;
}

// Decodes with WIC directly, so the assertions are about pixels rather than
// about whatever the provider says it did.
struct Decoded {
    unsigned width = 0;
    unsigned height = 0;
    std::vector<Pixel> pixels;
    unsigned frames = 0;
};

class Apartment {
public:
    Apartment() { ok_ = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~Apartment() { if (ok_) ::CoUninitialize(); }
    Apartment(const Apartment&) = delete;
    Apartment& operator=(const Apartment&) = delete;

private:
    bool ok_ = false;
};

Decoded Decode(const std::vector<std::byte>& bytes) {
    Decoded out;
    const Apartment apartment;

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory)))) {
        return out;
    }

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return out;
    if (FAILED(stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<std::byte*>(bytes.data())),
            static_cast<DWORD>(bytes.size())))) {
        return out;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                WICDecodeMetadataCacheOnDemand, &decoder))) {
        return out;
    }
    (void)decoder->GetFrameCount(&out.frames);

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return out;
    if (FAILED(frame->GetSize(&out.width, &out.height))) return out;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) return out;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return out;
    }

    out.pixels.resize(static_cast<std::size_t>(out.width) * out.height);
    const UINT stride = out.width * 4;
    if (FAILED(converter->CopyPixels(nullptr, stride,
                                     static_cast<UINT>(out.pixels.size() * 4),
                                     reinterpret_cast<BYTE*>(out.pixels.data())))) {
        out.pixels.clear();
    }
    return out;
}

// A two-frame GIF, written with WIC because hand-assembling LZW would be
// testing the fixture rather than the provider.
std::vector<std::byte> MakeAnimatedGif() {
    const Apartment apartment;
    std::vector<std::byte> out;

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory)))) {
        return out;
    }

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return out;

    // A growable in-memory stream is not something WIC offers, so this writes to
    // a temp file and reads it back.
    ScopedTempStore temp("gif");
    const std::filesystem::path path = temp.Acquire("animated.gif");
    if (FAILED(stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE))) return out;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatGif, nullptr, &encoder))) return out;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return out;

    for (int i = 0; i < 2; ++i) {
        ComPtr<IWICBitmap> bitmap;
        if (FAILED(factory->CreateBitmap(2, 2, GUID_WICPixelFormat32bppBGRA,
                                         WICBitmapCacheOnLoad, &bitmap))) {
            return out;
        }
        {
            ComPtr<IWICBitmapLock> lock;
            WICRect rect{0, 0, 2, 2};
            if (FAILED(bitmap->Lock(&rect, WICBitmapLockWrite, &lock))) return out;
            UINT size = 0;
            BYTE* data = nullptr;
            if (FAILED(lock->GetDataPointer(&size, &data))) return out;
            std::memset(data, i == 0 ? 0x20 : 0xD0, size);
        }

        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> bag;
        if (FAILED(encoder->CreateNewFrame(&frame, &bag))) return out;
        if (FAILED(frame->Initialize(bag.Get()))) return out;
        if (FAILED(frame->SetSize(2, 2))) return out;
        WICPixelFormatGUID format = GUID_WICPixelFormat8bppIndexed;
        if (FAILED(frame->SetPixelFormat(&format))) return out;

        ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(&converter))) return out;
        if (FAILED(converter->Initialize(bitmap.Get(), format, WICBitmapDitherTypeNone, nullptr,
                                         0.0, WICBitmapPaletteTypeMedianCut))) {
            return out;
        }
        if (FAILED(frame->WriteSource(converter.Get(), nullptr))) return out;
        if (FAILED(frame->Commit())) return out;
    }
    if (FAILED(encoder->Commit())) return out;
    stream.Reset();

    FileSource source(path);
    return ReadAtMost(source, 1024 * 1024);
}

// A TIFF carrying an EXIF orientation tag. TIFF rather than JPEG because the tag
// sits at the top level of the IFD, which WIC's metadata writer will set without
// an app1 block having to be assembled first.
std::vector<std::byte> MakeOrientedTiff(int width, int height, unsigned short orientation) {
    const Apartment apartment;
    std::vector<std::byte> out;

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory)))) {
        return out;
    }

    ScopedTempStore temp("tiff");
    const std::filesystem::path path = temp.Acquire("oriented.tiff");

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return out;
    if (FAILED(stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE))) return out;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatTiff, nullptr, &encoder))) return out;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return out;

    ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmap(static_cast<UINT>(width), static_cast<UINT>(height),
                                     GUID_WICPixelFormat24bppBGR, WICBitmapCacheOnLoad,
                                     &bitmap))) {
        return out;
    }
    {
        ComPtr<IWICBitmapLock> lock;
        WICRect rect{0, 0, width, height};
        if (FAILED(bitmap->Lock(&rect, WICBitmapLockWrite, &lock))) return out;
        UINT size = 0;
        BYTE* data = nullptr;
        if (FAILED(lock->GetDataPointer(&size, &data))) return out;
        std::memset(data, 0x7F, size);
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> bag;
    if (FAILED(encoder->CreateNewFrame(&frame, &bag))) return out;
    if (FAILED(frame->Initialize(bag.Get()))) return out;
    if (FAILED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)))) return out;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&format))) return out;

    ComPtr<IWICMetadataQueryWriter> metadata;
    if (SUCCEEDED(frame->GetMetadataQueryWriter(&metadata))) {
        PROPVARIANT value{};
        ::PropVariantInit(&value);
        value.vt = VT_UI2;
        value.uiVal = orientation;
        (void)metadata->SetMetadataByName(L"/ifd/{ushort=274}", &value);
        ::PropVariantClear(&value);
    }

    if (FAILED(frame->WriteSource(bitmap.Get(), nullptr))) return out;
    if (FAILED(frame->Commit())) return out;
    if (FAILED(encoder->Commit())) return out;
    stream.Reset();

    FileSource source(path);
    return ReadAtMost(source, 4 * 1024 * 1024);
}

class NoProgress final : public IProgressSink {
public:
    void Report(double fraction, std::uint64_t) override { last = fraction; }
    double last = -1.0;
};

// Runs one edge and hands back both the result and the bytes.
struct Conversion {
    Result result = Result::Ok();
    std::vector<std::byte> bytes;
};

Conversion Convert(const std::vector<std::byte>& input, const char* from, const char* to,
                   const OptionBag& options = {}) {
    wic::WicProvider provider;

    MemorySource source(std::string_view(reinterpret_cast<const char*>(input.data()),
                                         input.size()));
    MemorySink sink;
    NoProgress progress;
    ScopedTempStore temp("wic-test");
    Capabilities capabilities;
    std::stop_source stop;

    ConvertContext context{options, progress, stop.get_token(), temp, capabilities};

    Conversion out;
    out.result = provider.Convert(context, source, sink, *MediaProfile::Parse(from),
                                  *MediaProfile::Parse(to));
    out.bytes = sink.bytes();
    return out;
}

std::vector<Pixel> Checkerboard() {
    return {Pixel{0, 0, 255}, Pixel{0, 255, 0}, Pixel{255, 0, 0}, Pixel{255, 255, 255}};
}

}  // namespace

TEST_CASE("An image is identified by its bytes, never its name", "[wic]") {
    // Section 5: Probe reads content. Every image format has a magic number, so
    // this provider does not even look at the hint.
    const auto identify = [](std::initializer_list<unsigned char> bytes) {
        std::vector<std::byte> data;
        for (const unsigned char c : bytes) data.push_back(static_cast<std::byte>(c));
        return std::string(wic::IdentifyImage(data));
    };

    CHECK(identify({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}) == "image/png");
    CHECK(identify({0xFF, 0xD8, 0xFF, 0xE0}) == "image/jpeg");
    CHECK(identify({'B', 'M', 0, 0}) == "image/bmp");
    CHECK(identify({'G', 'I', 'F', '8', '9', 'a'}) == "image/gif");
    CHECK(identify({'I', 'I', 0x2A, 0x00}) == "image/tiff");
    CHECK(identify({'M', 'M', 0x00, 0x2A}) == "image/tiff");
    CHECK(identify({'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'}) == "image/webp");
    CHECK(identify({0, 0, 0, 0x20, 'f', 't', 'y', 'p', 'a', 'v', 'i', 'f'}) == "image/avif");
    CHECK(identify({0, 0, 0, 0x20, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c'}) == "image/heif");

    CHECK(identify({}).empty());
    CHECK(identify({0x89, 'P', 'N'}).empty());          // truncated magic
    CHECK(identify({'h', 'e', 'l', 'l', 'o'}).empty());  // a text file
    // A PNG magic that is one byte wrong is not a PNG. Guessing here would send
    // a damaged file to a decoder that would then be blamed for the crash.
    CHECK(identify({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0B}).empty());
}

TEST_CASE("A renamed image is still probed correctly", "[wic]") {
    const std::vector<std::byte> bmp = MakeBmp(2, 2, Checkerboard());

    wic::WicProvider provider;
    MemorySource source(std::string_view(reinterpret_cast<const char*>(bmp.data()), bmp.size()));

    PathHint lying;
    lying.filename = "notes.txt";
    lying.extension = ".txt";

    const std::optional<MediaProfile> profile = provider.Probe(source, lying);
    REQUIRE(profile);
    CHECK(profile->Id() == "image/bmp");
}

TEST_CASE("A text file is not claimed by the image provider", "[wic]") {
    wic::WicProvider provider;
    MemorySource source("# A heading\n\nSome prose.\n");

    PathHint hint;
    hint.filename = "notes.png";  // and the name says otherwise
    hint.extension = ".png";

    CHECK_FALSE(provider.Probe(source, hint).has_value());
}

TEST_CASE("A lossless round trip preserves every pixel", "[wic]") {
    const std::vector<Pixel> original = Checkerboard();
    const std::vector<std::byte> bmp = MakeBmp(2, 2, original);

    const Conversion png = Convert(bmp, "image/bmp", "image/png");
    REQUIRE(png.result);
    REQUIRE_FALSE(png.bytes.empty());
    CHECK(wic::IdentifyImage(png.bytes) == "image/png");

    const Decoded decoded = Decode(png.bytes);
    REQUIRE(decoded.width == 2u);
    REQUIRE(decoded.height == 2u);
    REQUIRE(decoded.pixels.size() == original.size());
    CHECK(decoded.pixels == original);

    // ... and back again, through a different codec each way.
    const Conversion tiff = Convert(png.bytes, "image/png", "image/tiff");
    REQUIRE(tiff.result);
    const Decoded again = Decode(tiff.bytes);
    CHECK(again.pixels == original);
}

TEST_CASE("A lossy target reports the loss it takes", "[wic]") {
    const std::vector<std::byte> bmp = MakeBmp(2, 2, Checkerboard());

    const Conversion jpeg = Convert(bmp, "image/bmp", "image/jpeg");
    REQUIRE(jpeg.result);
    CHECK(wic::IdentifyImage(jpeg.bytes) == "image/jpeg");

    // The image survives as an image of the same shape. What it must NOT do is
    // succeed while producing something that will not open -- which is the
    // failure the CLI found in the document pipeline a milestone ago.
    const Decoded decoded = Decode(jpeg.bytes);
    CHECK(decoded.width == 2u);
    CHECK(decoded.height == 2u);
    CHECK(decoded.frames == 1u);

    // No pixel-format note here on purpose: WIC's JPEG encoder takes 24bpp BGR
    // as it stands, so nothing was converted and there is nothing to report. A
    // note in that case would be noise, and the case where one IS warranted is
    // covered by the palettised source below.
    const bool mentioned =
        std::any_of(jpeg.result.notes().begin(), jpeg.result.notes().end(),
                    [](const FidelityNote& note) { return note.code == "image.pixel-format"; });
    CHECK_FALSE(mentioned);
}

TEST_CASE("A rotated photo comes out upright, with the tag gone", "[wic]") {
    // Section 6.1, step 1: "Normalise orientation from EXIF before anything else
    // touches pixels. Skipping this is the #1 source of 'my photos are sideways'
    // bug reports." Orientation 6 is a 90 degree rotation, so a 4x2 image has to
    // come out 2x4 -- the dimensions are the assertion because they cannot be
    // right by accident.
    const std::vector<std::byte> tiff = MakeOrientedTiff(4, 2, 6);
    if (tiff.empty()) SKIP("this machine's TIFF encoder did not produce a fixture");

    const Decoded before = Decode(tiff);
    REQUIRE(before.width == 4u);
    REQUIRE(before.height == 2u);

    const Conversion png = Convert(tiff, "image/tiff", "image/png");
    INFO(png.result.message());
    REQUIRE(png.result);

    const Decoded after = Decode(png.bytes);
    CHECK(after.width == 2u);
    CHECK(after.height == 4u);

    // And the tag must not survive: section 6.2's "never pass EXIF orientation
    // through unchanged after a rotation". A viewer that honoured it would turn
    // the picture a second time.
    const Conversion round = Convert(png.bytes, "image/png", "image/tiff");
    REQUIRE(round.result);
    const Decoded final = Decode(round.bytes);
    CHECK(final.width == 2u);
    CHECK(final.height == 4u);
}

TEST_CASE("An upright photo is left alone", "[wic]") {
    const std::vector<std::byte> tiff = MakeOrientedTiff(4, 2, 1);
    if (tiff.empty()) SKIP("this machine's TIFF encoder did not produce a fixture");

    const Conversion png = Convert(tiff, "image/tiff", "image/png");
    REQUIRE(png.result);

    const Decoded after = Decode(png.bytes);
    CHECK(after.width == 4u);
    CHECK(after.height == 2u);
}

TEST_CASE("The quality option reaches the encoder", "[wic]") {
    // 64x64 rather than 2x2: a two-pixel JPEG is all header, and the header does
    // not change with quality.
    std::vector<Pixel> pixels;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            pixels.push_back(Pixel{static_cast<unsigned char>(x * 4),
                                   static_cast<unsigned char>(y * 4),
                                   static_cast<unsigned char>((x ^ y) * 4)});
        }
    }
    const std::vector<std::byte> bmp = MakeBmp(64, 64, pixels);

    OptionBag low;
    low.Set("quality", std::int64_t{5});
    OptionBag high;
    high.Set("quality", std::int64_t{98});

    // Not named `small`: rpcndr.h, which arrives with windows.h, defines that as
    // a macro for char.
    const Conversion coarse = Convert(bmp, "image/bmp", "image/jpeg", low);
    const Conversion fine = Convert(bmp, "image/bmp", "image/jpeg", high);
    REQUIRE(coarse.result);
    REQUIRE(fine.result);
    CHECK(coarse.bytes.size() < fine.bytes.size());
}

TEST_CASE("A format Morph cannot write on this machine says so", "[wic]") {
    const std::vector<std::byte> bmp = MakeBmp(2, 2, Checkerboard());

    // image/avif is only encodable where a codec package provides it. Whichever
    // machine this runs on, the answer must be a clear one: either it converts,
    // or it reports a missing capability -- never a crash and never a silent
    // empty file.
    const Conversion avif = Convert(bmp, "image/bmp", "image/avif");
    if (!avif.result) {
        CHECK(avif.result.status() == Status::kMissingCapability);
        CHECK_FALSE(avif.result.message().empty());
    } else {
        CHECK_FALSE(avif.bytes.empty());
    }
}

TEST_CASE("An animation converted to a still frame says what it dropped", "[wic]") {
    const std::vector<std::byte> gif = MakeAnimatedGif();
    if (gif.empty() || Decode(gif).frames < 2) {
        SKIP("this machine's GIF encoder did not produce a two-frame fixture");
    }

    const Conversion png = Convert(gif, "image/gif", "image/png");
    REQUIRE(png.result);
    REQUIRE_FALSE(png.bytes.empty());

    // Section 6.2: "Silently dropping frames is the worst option."
    const bool mentioned =
        std::any_of(png.result.notes().begin(), png.result.notes().end(),
                    [](const FidelityNote& note) {
                        return note.code == "image.animation-dropped";
                    });
    CHECK(mentioned);
    // And the rating actually incurred is worse than the edge predicted.
    CHECK(png.result.incurred() >= Fidelity::Structural);
}

TEST_CASE("A palettised source converted to a full-colour target says so", "[wic]") {
    const std::vector<std::byte> gif = MakeAnimatedGif();
    if (gif.empty()) SKIP("this machine's GIF encoder did not produce a fixture");

    // GIF is 8bpp indexed and JPEG is not, so the pixel format is negotiated --
    // which is exactly the case section 8.8 wants the user told about.
    const Conversion jpeg = Convert(gif, "image/gif", "image/jpeg");
    REQUIRE(jpeg.result);

    const bool mentioned =
        std::any_of(jpeg.result.notes().begin(), jpeg.result.notes().end(),
                    [](const FidelityNote& note) { return note.code == "image.pixel-format"; });
    CHECK(mentioned);
}

TEST_CASE("The declared edges are consistent with what they promise", "[wic]") {
    const wic::WicProvider provider;
    const std::vector<EdgeDecl> edges = provider.EnumerateEdges();
    REQUIRE_FALSE(edges.empty());

    for (const EdgeDecl& edge : edges) {
        INFO(edge.from.Id() + " -> " + edge.to.Id());
        CHECK(edge.from.Id() != edge.to.Id());
        CHECK(edge.from.Family() == "image");
        CHECK(edge.to.Family() == "image");

        // A lossy target costs exactly one generation and cannot claim lossless;
        // a lossless one costs none. Getting this backwards would make the
        // planner prefer a route that degrades the file.
        const bool lossy = edge.cost.fidelity != Fidelity::Lossless;
        CHECK(edge.cost.generational == (lossy ? 1u : 0u));

        // Section 11.2 derives the memory budget from this, and an image that
        // decodes to less than its compressed size does not exist.
        CHECK(edge.cost.memPeakFactor > 1.0f);
        CHECK_FALSE(edge.cost.requiresDisk);
    }

    const bool hasPngToJpeg =
        std::any_of(edges.begin(), edges.end(), [](const EdgeDecl& edge) {
            return edge.from.Id() == "image/png" && edge.to.Id() == "image/jpeg";
        });
    CHECK(hasPngToJpeg);

    // GIF is a source and not a target until quantisation has a rating it
    // deserves (see the provider's own note on it).
    const bool writesGif = std::any_of(edges.begin(), edges.end(), [](const EdgeDecl& edge) {
        return edge.to.Id() == "image/gif";
    });
    CHECK_FALSE(writesGif);
}

TEST_CASE("A damaged image fails as malformed input, not as a crash", "[wic]") {
    // Section 15: hostile input is "the expected outcome ... and must never be a
    // crash". A PNG header with nothing behind it is the cheapest version of it.
    constexpr unsigned char kPngMagic[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::byte> broken;
    for (const unsigned char c : kPngMagic) broken.push_back(static_cast<std::byte>(c));
    broken.resize(64, std::byte{0x41});

    const Conversion out = Convert(broken, "image/png", "image/bmp");
    CHECK_FALSE(out.result);
    CHECK(out.result.status() == Status::kMalformedInput);
    CHECK(out.bytes.empty());  // nothing was committed
}
