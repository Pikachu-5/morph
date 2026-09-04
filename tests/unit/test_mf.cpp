// The Media Foundation backend (architecture section 7.1).
//
// The fixtures are built here, as they are for the image tests. A WAV is a RIFF
// header and some samples, which is simple enough to write by hand and gives an
// input whose exact contents are known; everything else is produced from it by
// the provider under test, so a round trip that comes back wrong belongs to the
// provider and not to a fixture.
//
// Every test that needs an encoder skips rather than fails when the machine does
// not have one. Section 7.3's whole point is that the codec set differs per
// machine and per SKU, and a suite that only passes on a developer's laptop is
// not testing that -- it is depending on it.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "morph/core/stream.h"
#include "morph/core/temp_store.h"
#include "morph/providers/mf.h"

using namespace morph;

namespace {

void PutU32(std::vector<std::byte>& into, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        into.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFF));
    }
}

void PutU16(std::vector<std::byte>& into, std::uint16_t value) {
    into.push_back(static_cast<std::byte>(value & 0xFF));
    into.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
}

void PutTag(std::vector<std::byte>& into, std::string_view tag) {
    for (const char c : tag) into.push_back(static_cast<std::byte>(c));
}

// A 16-bit stereo WAV holding a sine tone. Written out rather than generated,
// because the header is the part a decoder has to agree with.
std::vector<std::byte> MakeWav(int milliseconds, std::uint32_t rate = 44100) {
    const std::uint32_t frames = rate * static_cast<std::uint32_t>(milliseconds) / 1000;
    const std::uint32_t dataBytes = frames * 4;

    std::vector<std::byte> out;
    PutTag(out, "RIFF");
    PutU32(out, 36 + dataBytes);
    PutTag(out, "WAVE");

    PutTag(out, "fmt ");
    PutU32(out, 16);
    PutU16(out, 1);  // PCM
    PutU16(out, 2);  // stereo
    PutU32(out, rate);
    PutU32(out, rate * 4);
    PutU16(out, 4);
    PutU16(out, 16);

    PutTag(out, "data");
    PutU32(out, dataBytes);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        const auto value = static_cast<std::int16_t>(12000.0 * std::sin(2.0 * 3.14159265 * 440.0 * t));
        PutU16(out, static_cast<std::uint16_t>(value));
        PutU16(out, static_cast<std::uint16_t>(value));
    }
    return out;
}

class NoProgress final : public IProgressSink {
public:
    void Report(double fraction, std::uint64_t) override {
        // Progress must never go backwards and never exceed one: the UI draws a
        // bar from it, and section 11.4 is specific about the weighting.
        CHECK(fraction >= last);
        CHECK(fraction <= 1.0);
        last = fraction;
    }
    double last = 0.0;
};

struct Conversion {
    Result result = Result::Ok();
    std::vector<std::byte> bytes;
};

Conversion Convert(const std::vector<std::byte>& input, const char* from, const char* to,
                   const OptionBag& options = {}) {
    mf::MediaFoundationProvider provider;

    MemorySource source(std::string_view(reinterpret_cast<const char*>(input.data()),
                                         input.size()));
    MemorySink sink;
    NoProgress progress;
    ScopedTempStore temp("mf-test");
    Capabilities capabilities;
    std::stop_source stop;

    ConvertContext context{options, progress, stop.get_token(), temp, capabilities};

    Conversion out;
    out.result = provider.Convert(context, source, sink, *MediaProfile::Parse(from),
                                  *MediaProfile::Parse(to));
    out.bytes = sink.bytes();
    return out;
}

bool HasNote(const Result& result, std::string_view code) {
    return std::any_of(result.notes().begin(), result.notes().end(),
                       [code](const FidelityNote& note) { return note.code == code; });
}

}  // namespace

TEST_CASE("Media is identified by its container's bytes", "[mf]") {
    const auto identify = [](std::initializer_list<int> bytes) {
        std::vector<std::byte> data;
        for (const int c : bytes) data.push_back(static_cast<std::byte>(c));
        return std::string(mf::IdentifyContainer(data));
    };

    CHECK(identify({0, 0, 0, 0x20, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm'}) == "video/mp4");
    CHECK(identify({0, 0, 0, 0x14, 'f', 't', 'y', 'p', 'q', 't', ' ', ' '}) ==
          "video/quicktime");
    CHECK(identify({0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'M', '4', 'A', ' '}) == "audio/mp4");
    CHECK(identify({'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'}) == "audio/wav");
    CHECK(identify({'f', 'L', 'a', 'C', 0, 0, 0, 0}) == "audio/flac");
    CHECK(identify({'I', 'D', '3', 4, 0, 0, 0, 0}) == "audio/mpeg");
    CHECK(identify({0xFF, 0xFB, 0x90, 0x00}) == "audio/mpeg");
    CHECK(identify({0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11}) == "video/x-ms-asf");

    // A QuickTime file and an MP4 differ only in the brand, and they are
    // different nodes: one can be written and the other cannot.
    CHECK(identify({}).empty());
    CHECK(identify({'R', 'I', 'F', 'F', 0, 0, 0, 0, 'A', 'V', 'I', ' '}).empty());
    CHECK(identify({'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o'}).empty());
}

TEST_CASE("A WAV is recognised by content, not by name", "[mf]") {
    const std::vector<std::byte> wav = MakeWav(50);

    mf::MediaFoundationProvider provider;
    MemorySource source(std::string_view(reinterpret_cast<const char*>(wav.data()), wav.size()));

    PathHint lying;
    lying.filename = "notes.txt";
    lying.extension = ".txt";

    const std::optional<MediaProfile> profile = provider.Probe(source, lying);
    REQUIRE(profile);
    CHECK(profile->Id() == "audio/wav;codec=pcm");
}

TEST_CASE("A text file is not claimed by the media provider", "[mf]") {
    mf::MediaFoundationProvider provider;
    MemorySource source("# A heading\n\nSome prose.\n");

    PathHint hint;
    hint.filename = "song.mp3";
    hint.extension = ".mp3";
    CHECK_FALSE(provider.Probe(source, hint).has_value());
}

TEST_CASE("The remux decision is about codecs, not containers", "[mf][remux]") {
    // Section 7.2: "Before planning a transcode, check whether every stream's
    // codec node already matches the target container's accepted set. If so,
    // emit -c copy. A 4 GB MKV to MP4 becomes an IO-bound file copy instead of a
    // twenty-minute encode."
    CHECK(mf::CanRemux("video/quicktime;codec=h264", "video/mp4;codec=h264"));

    // Same codec, same container: not a conversion at all.
    CHECK_FALSE(mf::CanRemux("video/mp4;codec=h264", "video/mp4;codec=h264"));
    // Different codec in the same container: a full re-encode, which is exactly
    // the 100x difference section 4.1 says the planner must be able to see.
    CHECK_FALSE(mf::CanRemux("video/mp4;codec=h264", "video/mp4;codec=hevc"));
    // Video samples cannot be poured into an audio container.
    CHECK_FALSE(mf::CanRemux("video/mp4;codec=h264", "audio/mp4;codec=aac"));
    // A target Morph cannot write is not a remux target however well the codecs
    // line up.
    CHECK_FALSE(mf::CanRemux("video/mp4;codec=h264", "video/quicktime;codec=h264"));
    CHECK_FALSE(mf::CanRemux("audio/flac", "audio/flac"));
    CHECK_FALSE(mf::CanRemux("nonsense", "video/mp4;codec=h264"));
}

TEST_CASE("The declared edges are consistent with what they promise", "[mf]") {
    const mf::MediaFoundationProvider provider;
    const std::vector<EdgeDecl> edges = provider.EnumerateEdges();

    if (edges.empty()) SKIP("this machine has no Media Foundation encoders at all");

    for (const EdgeDecl& edge : edges) {
        INFO(edge.from.Id() + " -> " + edge.to.Id());
        CHECK(edge.from.Id() != edge.to.Id());

        // Audio can never become video: there is nothing to make a picture from.
        const bool audioToVideo =
            edge.from.Family() == "audio" && edge.to.Family() == "video";
        CHECK_FALSE(audioToVideo);

        // Every A/V edge writes through a temp file, because an MP4 muxer seeks
        // backwards to write its index and ISink is append-only (section 11.3).
        CHECK(edge.cost.requiresDisk);

        // A remuxable pair costs nothing and loses nothing; anything else costs
        // a generation only if the target is lossy.
        if (mf::CanRemux(edge.from.Id(), edge.to.Id())) {
            CHECK(edge.cost.fidelity == Fidelity::Lossless);
            CHECK(edge.cost.generational == 0u);
            // Copying samples is IO, not compute. If this ever creeps up to a
            // transcode's cost the planner stops preferring it.
            CHECK(edge.cost.cpuPerMiB < 50.0f);
        }
    }

    // A MOV holding H.264 must be offered as an MP4, and as a copy.
    const bool movToMp4 = std::any_of(edges.begin(), edges.end(), [](const EdgeDecl& edge) {
        return edge.from.Id() == "video/quicktime;codec=h264" &&
               edge.to.Id() == "video/mp4;codec=h264" &&
               edge.cost.fidelity == Fidelity::Lossless;
    });
    if (mf::InstalledEncoders().h264) CHECK(movToMp4);
}

TEST_CASE("Extracting sound from a video is offered, and says what it drops", "[mf]") {
    const mf::MediaFoundationProvider provider;
    const std::vector<EdgeDecl> edges = provider.EnumerateEdges();
    if (!mf::InstalledEncoders().aac) SKIP("this machine has no AAC encoder");

    const auto extract = std::find_if(edges.begin(), edges.end(), [](const EdgeDecl& edge) {
        return edge.from.Id() == "video/mp4;codec=h264" && edge.to.Id() == "audio/mp4;codec=aac";
    });
    REQUIRE(extract != edges.end());

    // Half the file is gone. Nothing in the scale is a perfect fit for that, and
    // Structural is the honest one -- what survives is one stream's content, not
    // the presentation of the whole.
    CHECK(extract->cost.fidelity == Fidelity::Structural);
}

TEST_CASE("A WAV encodes to whatever this machine can encode", "[mf]") {
    const std::vector<std::byte> wav = MakeWav(200);
    const mf::EncoderSupport& encoders = mf::InstalledEncoders();

    struct Case {
        const char* profile;
        bool available;
    };
    const Case cases[] = {{"audio/mp4;codec=aac", encoders.aac},
                          {"audio/mpeg;codec=mp3", encoders.mp3},
                          {"audio/x-ms-wma;codec=wma", encoders.wma}};

    int ran = 0;
    for (const Case& one : cases) {
        if (!one.available) continue;
        ++ran;
        INFO(one.profile);

        const Conversion out = Convert(wav, "audio/wav;codec=pcm", one.profile);
        INFO(out.result.message());
        REQUIRE(out.result);
        // A compressed second of a sine tone is smaller than the PCM it came
        // from, and is not empty. Both halves matter: the document pipeline's
        // first bug produced a valid, well-formed, empty file.
        CHECK_FALSE(out.bytes.empty());
        CHECK(out.bytes.size() < wav.size());
        CHECK(HasNote(out.result, "av.transcoded"));
    }

    if (ran == 0) SKIP("this machine has no audio encoders");
}

TEST_CASE("Decoding to PCM is lossless, and says nothing about re-encoding", "[mf]") {
    if (!mf::InstalledEncoders().aac) SKIP("this machine has no AAC encoder");

    const std::vector<std::byte> wav = MakeWav(200);
    const Conversion aac = Convert(wav, "audio/wav;codec=pcm", "audio/mp4;codec=aac");
    REQUIRE(aac.result);

    const Conversion back = Convert(aac.bytes, "audio/mp4;codec=aac", "audio/wav;codec=pcm");
    INFO(back.result.message());
    REQUIRE(back.result);
    CHECK_FALSE(back.bytes.empty());

    // The PCM holds exactly what came out of the decoder. Calling that a
    // re-encode would be a note that says the opposite of the truth.
    CHECK_FALSE(HasNote(back.result, "av.transcoded"));
    CHECK(back.result.incurred() == Fidelity::Lossless);

    // It is a WAV, and it is about the right length: a 200 ms stereo 16-bit
    // 44.1 kHz file is roughly 35 KB, and AAC's encoder delay makes it a little
    // longer rather than shorter.
    CHECK(mf::IdentifyContainer(back.bytes) == "audio/wav");
    CHECK(back.bytes.size() > 30000u);
    CHECK(back.bytes.size() < 80000u);
}

TEST_CASE("The bitrate option reaches the encoder", "[mf]") {
    if (!mf::InstalledEncoders().aac) SKIP("this machine has no AAC encoder");

    const std::vector<std::byte> wav = MakeWav(1000);

    OptionBag low;
    low.Set("audioBitrate", std::int64_t{96});
    OptionBag high;
    high.Set("audioBitrate", std::int64_t{192});

    const Conversion coarse = Convert(wav, "audio/wav;codec=pcm", "audio/mp4;codec=aac", low);
    const Conversion fine = Convert(wav, "audio/wav;codec=pcm", "audio/mp4;codec=aac", high);
    REQUIRE(coarse.result);
    REQUIRE(fine.result);
    CHECK(coarse.bytes.size() < fine.bytes.size());
}

TEST_CASE("A damaged file fails with a reason, not a crash", "[mf]") {
    // Section 15: hostile input "is the expected outcome ... and must never be a
    // crash". A WAV header with nothing behind it is the cheapest version.
    std::vector<std::byte> broken = MakeWav(10);
    broken.resize(64);
    std::fill(broken.begin() + 44, broken.end(), std::byte{0x41});

    const Conversion out = Convert(broken, "audio/wav;codec=pcm", "audio/mp4;codec=aac");
    if (out.result) {
        // A truncated WAV that decodes to a very short file is a legitimate
        // outcome; what matters is that it did not crash and did not lie.
        SUCCEED();
    } else {
        CHECK_FALSE(out.result.message().empty());
        CHECK(out.bytes.empty());
    }
}

TEST_CASE("A cancelled conversion leaves nothing behind", "[mf]") {
    if (!mf::InstalledEncoders().aac) SKIP("this machine has no AAC encoder");

    const std::vector<std::byte> wav = MakeWav(3000);

    mf::MediaFoundationProvider provider;
    MemorySource source(std::string_view(reinterpret_cast<const char*>(wav.data()), wav.size()));
    MemorySink sink;
    NoProgress progress;
    ScopedTempStore temp("mf-cancel");
    Capabilities capabilities;
    OptionBag options;

    std::stop_source stop;
    stop.request_stop();  // cancelled before it starts, which is the hardest case

    ConvertContext context{options, progress, stop.get_token(), temp, capabilities};
    const Result result = provider.Convert(context, source, sink,
                                           *MediaProfile::Parse("audio/wav;codec=pcm"),
                                           *MediaProfile::Parse("audio/mp4;codec=aac"));

    // Section 11.3: cancellation never leaves a half-written file where the user
    // expects a real one. The sink is uncommitted, so there is nothing there.
    CHECK(result.cancelled());
    CHECK_FALSE(sink.committed());
}
