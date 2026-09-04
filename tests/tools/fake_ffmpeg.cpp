// A stand-in for ffmpeg.exe and ffprobe.exe, used by the tests.
//
// The FFmpeg backend is a subprocess protocol: an argument list goes out, a
// progress stream comes back on stdout, a diagnosis comes back on stderr, an
// exit code decides, and a file appears. Every one of those is something that
// can be wrong, and none of them needs a codec to be wrong in.
//
// So the tests drive this instead. It is not a mock in the usual sense -- the
// provider spawns it for real, through the real CreateProcess, reads the real
// pipes, and cancels it by terminating a real job object. What it does not do is
// encode anything, which is the only part the tests are not asserting about.
//
// It answers to both names, because the provider looks for two binaries and the
// difference between them is the arguments they are given.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

bool Has(const std::vector<std::string>& args, std::string_view flag) {
    for (const std::string& arg : args) {
        if (arg == flag) return true;
    }
    return false;
}

std::string ValueAfter(const std::vector<std::string>& args, std::string_view flag) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == flag) return args[i + 1];
    }
    return {};
}

bool Contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

// ffprobe: two shapes of question, both answered as CSV.
int Probe(const std::vector<std::string>& args) {
    const std::string entries = ValueAfter(args, "-show_entries");
    const std::string path = args.empty() ? std::string() : args.back();

    if (Contains(entries, "format=duration")) {
        std::printf("2.500000\n");
        return 0;
    }
    if (Contains(entries, "codec_type")) {
        if (Contains(path, "audio-only")) {
            std::printf("audio,opus\n");
        } else if (Contains(path, "unknown-codec")) {
            std::printf("video,theora\n");
        } else if (Contains(path, "empty")) {
            // A file with no streams at all: a legitimate answer, and one the
            // caller has to survive rather than crash on.
        } else {
            std::printf("video,vp9\naudio,opus\n");
        }
        return 0;
    }
    std::fprintf(stderr, "fake ffprobe: nothing to say about %s\n", entries.c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

    if (Has(args, "-version")) {
        std::printf("ffmpeg version 0.0-fake Copyright (c) the Morph test suite\n");
        std::printf("configuration: --disable-gpl --disable-nonfree\n");
        return 0;
    }
    if (Has(args, "-show_entries")) return Probe(args);

    // Otherwise: a conversion. The output path is the last argument, as it is
    // for the real thing.
    const std::string input = ValueAfter(args, "-i");
    const std::string output = args.empty() ? std::string() : args.back();

    // A failure with a real reason on stderr, which is what the provider is
    // supposed to surface instead of "conversion failed".
    if (Contains(input, "broken")) {
        std::fprintf(stderr, "[matroska @ 000001] Invalid EBML header\n");
        std::fprintf(stderr, "%s: Invalid data found when processing input\n", input.c_str());
        return 1;
    }

    // A conversion that never finishes, for the cancellation test. It keeps
    // emitting progress so the parent has something to read while it decides.
    const bool hang = Contains(output, "hang");

    for (int step = 1; step <= 10; ++step) {
        std::printf("frame=%d\n", step * 25);
        std::printf("total_size=%d\n", step * 4096);
        std::printf("out_time_us=%d\n", step * 250000);
        std::printf("speed=1.0x\n");
        std::printf("progress=continue\n");
        std::fflush(stdout);

        if (hang) {
            step = 0;  // forever
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Something has to appear at the output path or the provider is right to
    // call it a failure.
    if (!output.empty()) {
        std::FILE* file = nullptr;
        if (::fopen_s(&file, output.c_str(), "wb") == 0 && file != nullptr) {
            // Enough bytes to look like a file and to carry the arguments the
            // provider chose, so a test can assert on what was asked for without
            // parsing a command line it did not see.
            std::fprintf(file, "FAKE");
            for (const std::string& arg : args) std::fprintf(file, " %s", arg.c_str());
            std::fprintf(file, "\n");
            std::fclose(file);
        }
    }

    std::printf("progress=end\n");
    std::fflush(stdout);
    return 0;
}
