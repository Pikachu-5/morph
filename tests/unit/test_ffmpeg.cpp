// The FFmpeg backend (architecture section 7.2).
//
// Two halves, tested differently. The decisions -- what the argument list says,
// what a progress block means, what ffprobe's answer adds up to -- are pure
// functions and are tested exhaustively. The plumbing is tested by spawning a
// stand-in binary (tests/tools/fake_ffmpeg.cpp) through the real CreateProcess,
// reading the real pipes, and cancelling it by terminating a real job object.
//
// What is NOT tested here is whether libvpx produces a valid WebM, because that
// is FFmpeg's test suite's job and not ours. What is ours is everything between
// the graph and the process, and that is what these cover.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>

#include "morph/core/stream.h"
#include "morph/core/temp_store.h"
#include "morph/providers/ffmpeg.h"
#include "providers/ffmpeg/ffmpeg_command.h"
#include "providers/ffmpeg/ffmpeg_process.h"

using namespace morph;
using namespace morph::ffmpeg;

namespace {

std::filesystem::path BinaryDirectory() {
    wchar_t self[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    return std::filesystem::path(self).parent_path();
}

// Points the provider at the stand-in binaries BEFORE anything can ask where
// FFmpeg is: InstalledTools caches its answer in a function-local static, so the
// first caller decides for the whole process. A static initialiser runs before
// main, which is earlier than any test can.
struct UseFakeTools {
    UseFakeTools() {
        const std::filesystem::path directory = BinaryDirectory() / "fake-ffmpeg";
        ::SetEnvironmentVariableW(L"MORPH_FFMPEG_DIR", directory.wstring().c_str());
    }
};
const UseFakeTools gUseFakeTools;

bool FakeToolsPresent() {
    return std::filesystem::exists(BinaryDirectory() / "fake-ffmpeg" / "ffmpeg.exe");
}

// Every argument this backend builds is ASCII by construction except the paths,
// and the paths in these tests are ASCII too, so a narrowing cast is honest
// here in a way it would not be in the provider.
std::string Narrow(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (const wchar_t c : text) out.push_back(static_cast<char>(c));
    return out;
}

std::string Joined(const std::vector<std::wstring>& args) {
    std::string out;
    for (const std::wstring& arg : args) {
        if (!out.empty()) out.push_back(' ');
        out += Narrow(arg);
    }
    return out;
}

bool Follows(const std::vector<std::wstring>& args, std::wstring_view flag,
             std::wstring_view value) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == flag) return args[i + 1] == value;
    }
    return false;
}

struct Conversion {
    Result result = Result::Ok();
    std::vector<std::byte> bytes;
};

Conversion Convert(std::string_view inputBytes, const char* from, const char* to,
                   const OptionBag& options = {}, bool cancelled = false) {
    FfmpegProvider provider;

    MemorySource source(inputBytes);
    MemorySink sink;
    struct Sink final : IProgressSink {
        void Report(double fraction, std::uint64_t bytes) override {
            CHECK(fraction >= last);
            last = fraction;
            written = bytes;
        }
        double last = 0.0;
        std::uint64_t written = 0;
    } progress;

    ScopedTempStore temp("ffmpeg-test");
    Capabilities capabilities;
    std::stop_source stop;
    if (cancelled) stop.request_stop();

    ConvertContext context{options, progress, stop.get_token(), temp, capabilities};

    Conversion out;
    out.result = provider.Convert(context, source, sink, *MediaProfile::Parse(from),
                                  *MediaProfile::Parse(to));
    out.bytes = sink.bytes();
    return out;
}

std::string Text(const std::vector<std::byte>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace

TEST_CASE("A container is identified by its magic", "[ffmpeg]") {
    const auto identify = [](std::string_view text) {
        std::vector<std::byte> bytes;
        for (const char c : text) bytes.push_back(static_cast<std::byte>(c));
        return std::string(IdentifyContainer(bytes));
    };

    // EBML, with the DocType deciding between the two.
    CHECK(identify("\x1A\x45\xDF\xA3....webm....") == "video/webm");
    CHECK(identify("\x1A\x45\xDF\xA3....matroska") == "video/x-matroska");
    CHECK(identify("OggS....") == "audio/ogg");
    CHECK(identify("fLaC....") == "audio/flac");

    // Only AV1 in an MP4 belongs here. Everything else in an MP4 is Media
    // Foundation's, and two providers claiming one node would make the probe
    // order decide which -- a tie with no reason to break it either way.
    CHECK(identify("....ftypav01") == "video/mp4");
    CHECK(identify("....ftypisom").empty());
    CHECK(identify("").empty());
    CHECK(identify("not media at all").empty());
}

TEST_CASE("The remux decision is about codecs, not containers", "[ffmpeg][remux]") {
    // Section 7.2: "check whether every stream's codec node already matches the
    // target container's accepted set. If so, emit -c copy."
    CHECK(CanRemux("video/webm;codec=vp9", "video/x-matroska;codec=vp9"));
    CHECK(CanRemux("video/mp4;codec=av1", "video/webm;codec=av1"));

    CHECK_FALSE(CanRemux("video/webm;codec=vp9", "video/webm;codec=vp9"));
    CHECK_FALSE(CanRemux("video/webm;codec=vp9", "video/webm;codec=av1"));
    CHECK_FALSE(CanRemux("video/webm;codec=vp9", "audio/ogg;codec=opus"));
    // A read-only target is not a remux target however well the codecs line up.
    CHECK_FALSE(CanRemux("video/webm;codec=av1", "video/mp4;codec=av1"));
}

TEST_CASE("The argument list is section 7.2's, and says copy when it can",
          "[ffmpeg][command]") {
    const Format* webm = FindFormat("video/webm;codec=vp9");
    REQUIRE(webm != nullptr);

    const OptionBag options;
    const std::vector<std::wstring> transcode =
        BuildArguments(*webm, L"in.mkv", L"out.webm", options, /*remux=*/false);
    const std::string joined = Joined(transcode);
    INFO(joined);

    // -nostdin is not decoration: without it a child that reads the console
    // steals the parent's stdin and hangs a batch conversion forever.
    CHECK(joined.find("-nostdin") != std::string::npos);
    CHECK(Follows(transcode, L"-loglevel", L"error"));
    CHECK(Follows(transcode, L"-progress", L"pipe:1"));
    CHECK(Follows(transcode, L"-stats_period", L"0.25"));
    CHECK(Follows(transcode, L"-i", L"in.mkv"));
    CHECK(Follows(transcode, L"-c:v", L"libvpx-vp9"));
    // -crf with -b:v 0 is how libvpx and SVT-AV1 are meant to be driven: a
    // consistent result across clips rather than a consistent file size.
    CHECK(Follows(transcode, L"-b:v", L"0"));
    CHECK(transcode.back() == L"out.webm");

    const std::vector<std::wstring> remux =
        BuildArguments(*webm, L"in.mkv", L"out.webm", options, /*remux=*/true);
    CHECK(Follows(remux, L"-c", L"copy"));
    // A copy takes no encoder and no quality: setting either would be ignored at
    // best and would silently start a re-encode at worst.
    CHECK(Joined(remux).find("libvpx") == std::string::npos);
    CHECK(Joined(remux).find("-crf") == std::string::npos);
}

TEST_CASE("Quality and bitrate options reach the command line", "[ffmpeg][command]") {
    OptionBag options;
    options.Set("quality", std::int64_t{18});
    options.Set("audioBitrate", std::int64_t{96});

    const Format* webm = FindFormat("video/webm;codec=vp9");
    const Format* opus = FindFormat("audio/ogg;codec=opus");
    REQUIRE(webm != nullptr);
    REQUIRE(opus != nullptr);

    CHECK(Follows(BuildArguments(*webm, L"a", L"b", options, false), L"-crf", L"18"));
    CHECK(Follows(BuildArguments(*opus, L"a", L"b", options, false), L"-b:a", L"96k"));
    // An audio target keeps no picture, and says so rather than relying on the
    // muxer to drop it.
    CHECK(Joined(BuildArguments(*opus, L"a", L"b", options, false)).find("-vn") !=
          std::string::npos);

    // Out of range values are clamped rather than passed through: -crf 900 is an
    // error from FFmpeg and a puzzle for the user.
    OptionBag absurd;
    absurd.Set("quality", std::int64_t{900});
    CHECK(Follows(BuildArguments(*webm, L"a", L"b", absurd, false), L"-crf", L"63"));
}

TEST_CASE("A progress block is read, and out_time_ms is microseconds",
          "[ffmpeg][progress]") {
    ProgressState state;

    CHECK(ApplyProgressLine("frame=125", state));
    CHECK(ApplyProgressLine("total_size=40960", state));
    CHECK(ApplyProgressLine("out_time_us=2500000", state));
    CHECK(ApplyProgressLine("progress=continue", state));

    CHECK(state.frames == 125u);
    CHECK(state.bytes == 40960u);
    CHECK(state.microseconds == 2'500'000u);
    CHECK_FALSE(state.finished);

    // FFmpeg's out_time_ms has always been microseconds despite the name.
    // Treating it as milliseconds finishes the bar a thousand times too early,
    // which looks like a rendering glitch rather than a parsing bug.
    ProgressState other;
    CHECK(ApplyProgressLine("out_time_ms=2500000", other));
    CHECK(other.microseconds == 2'500'000u);

    CHECK(ApplyProgressLine("progress=end", state));
    CHECK(state.finished);

    // Noise is ignored rather than misread.
    CHECK_FALSE(ApplyProgressLine("", state));
    CHECK_FALSE(ApplyProgressLine("no equals sign here", state));
    CHECK_FALSE(ApplyProgressLine("bitrate=N/A", state));
}

TEST_CASE("ffprobe's CSV becomes a profile", "[ffmpeg][probe]") {
    const std::vector<StreamInfo> streams = ParseProbeOutput("video,vp9\naudio,opus\n");
    REQUIRE(streams.size() == 2u);
    CHECK(streams[0].type == "video");
    CHECK(streams[0].codec == "vp9");

    // The video stream names the node. The audio codec is not part of it, and
    // making it part would double the node count for nothing.
    CHECK(ProfileFor("video/webm", streams) == "video/webm;codec=vp9");
    CHECK(ProfileFor("audio/ogg", ParseProbeOutput("audio,opus")) == "audio/ogg;codec=opus");
    CHECK(ProfileFor("audio/flac", ParseProbeOutput("audio,flac")) == "audio/flac");

    // A codec this build does not handle is not a node, and saying so is better
    // than routing a file down an edge that does not exist.
    CHECK(ProfileFor("video/webm", ParseProbeOutput("video,theora")).empty());
    CHECK(ProfileFor("video/webm", {}).empty());
    CHECK(ProfileFor("", streams).empty());

    // Ragged output does not produce ragged answers.
    CHECK(ParseProbeOutput("").empty());
    CHECK(ParseProbeOutput("video\naudio\n").empty());
    CHECK(ParseProbeOutput("  video , vp9  \n\n").size() == 1u);
}

TEST_CASE("An argument survives being quoted for the command line", "[ffmpeg][command]") {
    // The rule CommandLineToArgvW implements is not the obvious one, and a path
    // with a quote in it is a thing a user can create.
    CHECK(QuoteArgument(L"plain") == L"plain");
    CHECK(QuoteArgument(L"has space") == L"\"has space\"");
    CHECK(QuoteArgument(L"C:\\dir\\file.mkv") == L"C:\\dir\\file.mkv");
    CHECK(QuoteArgument(L"C:\\my dir\\file.mkv") == L"\"C:\\my dir\\file.mkv\"");
    CHECK(QuoteArgument(L"say \"hi\"") == L"\"say \\\"hi\\\"\"");
    // A trailing backslash inside quotes has to double, or it escapes the
    // closing quote and swallows the next argument.
    CHECK(QuoteArgument(L"C:\\my dir\\") == L"\"C:\\my dir\\\\\"");
}

TEST_CASE("No FFmpeg means no edges at all", "[ffmpeg]") {
    // Section 7.3's Store SKU is this build without the binary beside it. The
    // route does not exist rather than existing and failing.
    if (!FakeToolsPresent()) {
        const FfmpegProvider provider;
        CHECK(provider.EnumerateEdges().empty());
        SKIP("the stand-in binaries were not built");
    }
    SUCCEED();
}

TEST_CASE("With FFmpeg present the edges are consistent", "[ffmpeg]") {
    if (!FakeToolsPresent()) SKIP("the stand-in binaries were not built");

    const FfmpegProvider provider;
    const std::vector<EdgeDecl> edges = provider.EnumerateEdges();
    REQUIRE_FALSE(edges.empty());

    for (const EdgeDecl& edge : edges) {
        INFO(edge.from.Id() + " -> " + edge.to.Id());
        CHECK(edge.from.Id() != edge.to.Id());
        // FFmpeg takes a path and writes a path, so both ends spill to the temp
        // store and every edge declares it.
        CHECK(edge.cost.requiresDisk);

        const bool audioToVideo =
            edge.from.Family() == "audio" && edge.to.Family() == "video";
        CHECK_FALSE(audioToVideo);

        if (CanRemux(edge.from.Id(), edge.to.Id())) {
            CHECK(edge.cost.fidelity == Fidelity::Lossless);
            CHECK(edge.cost.cpuPerMiB < 50.0f);
        }
    }

    // FLAC out is the whole reason section 7.3 says a GitHub SKU exists: Media
    // Foundation decodes it and has never encoded it.
    const bool flacOut = std::any_of(edges.begin(), edges.end(), [](const EdgeDecl& edge) {
        return edge.to.Id() == "audio/flac" && edge.cost.fidelity == Fidelity::Lossless;
    });
    CHECK(flacOut);
}

TEST_CASE("A conversion runs the binary and keeps what it produced", "[ffmpeg]") {
    if (!FakeToolsPresent()) SKIP("the stand-in binaries were not built");

    const Conversion out = Convert("\x1A\x45\xDF\xA3 webm payload",
                                   "video/webm;codec=vp9", "audio/ogg;codec=opus");
    INFO(out.result.message());
    REQUIRE(out.result);
    REQUIRE_FALSE(out.bytes.empty());

    // The stand-in writes back the arguments it was given, so this asserts on
    // what the provider actually asked for rather than on what it meant to.
    const std::string produced = Text(out.bytes);
    INFO(produced);
    CHECK(produced.find("-c:a libopus") != std::string::npos);
    CHECK(produced.find("-vn") != std::string::npos);

    // The picture was dropped and the sound re-encoded; both are worth saying.
    CHECK(std::any_of(out.result.notes().begin(), out.result.notes().end(),
                      [](const FidelityNote& note) { return note.code == "av.video-dropped"; }));
    CHECK(out.result.incurred() >= Fidelity::Structural);
}

TEST_CASE("A remux says so and asks for a copy", "[ffmpeg][remux]") {
    if (!FakeToolsPresent()) SKIP("the stand-in binaries were not built");

    const Conversion out = Convert("\x1A\x45\xDF\xA3 webm payload",
                                   "video/webm;codec=vp9", "video/x-matroska;codec=vp9");
    REQUIRE(out.result);

    const std::string produced = Text(out.bytes);
    INFO(produced);
    CHECK(produced.find("-c copy") != std::string::npos);
    CHECK(produced.find("libvpx") == std::string::npos);

    // Nothing was decoded, so nothing was lost.
    CHECK(out.result.incurred() == Fidelity::Lossless);
    CHECK(std::any_of(out.result.notes().begin(), out.result.notes().end(),
                      [](const FidelityNote& note) { return note.code == "av.remuxed"; }));
}

TEST_CASE("A failure carries FFmpeg's own reason", "[ffmpeg]") {
    if (!FakeToolsPresent()) SKIP("the stand-in binaries were not built");

    // The stand-in fails when the input is called "broken", and says why on
    // stderr exactly as the real one does.
    FfmpegProvider provider;
    MemorySource source("\x1A\x45\xDF\xA3 broken");
    MemorySink sink;
    struct Silent final : IProgressSink {
        void Report(double, std::uint64_t) override {}
    } progress;

    // The temp store's label becomes part of the path, which is how the
    // stand-in is told which case this is.
    ScopedTempStore temp("broken");
    Capabilities capabilities;
    OptionBag options;
    std::stop_source stop;
    ConvertContext context{options, progress, stop.get_token(), temp, capabilities};

    const Result result =
        provider.Convert(context, source, sink, *MediaProfile::Parse("video/webm;codec=vp9"),
                         *MediaProfile::Parse("audio/ogg;codec=opus"));

    REQUIRE_FALSE(result);
    INFO(result.message());
    // Section 12.2: "Failures listed with a real reason, never 'conversion
    // failed'." The reason here is FFmpeg's own first line of stderr.
    CHECK(result.message().find("Invalid") != std::string::npos);
    CHECK_FALSE(sink.committed());
}

TEST_CASE("A cancelled conversion kills the child and commits nothing", "[ffmpeg]") {
    if (!FakeToolsPresent()) SKIP("the stand-in binaries were not built");

    const Conversion out = Convert("\x1A\x45\xDF\xA3 webm payload",
                                   "video/webm;codec=vp9", "audio/ogg;codec=opus", {},
                                   /*cancelled=*/true);

    // Section 11.3: cancellation never leaves a half-written file where the user
    // expects a real one, and section 7.2's cancellation is TerminateJobObject
    // on the child's job -- which is what happened here, to a real process.
    CHECK(out.result.cancelled());
    CHECK(out.bytes.empty());
}
