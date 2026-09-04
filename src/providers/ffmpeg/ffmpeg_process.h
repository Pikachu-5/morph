// Running FFmpeg as a child process (architecture sections 7.2 and 11.3).
//
// Two things here are not optional.
//
// THE JOB OBJECT. Section 7.2 says cancellation is "TerminateJobObject on the
// child's job", and section 11.2 puts every worker in one already. FFmpeg spawns
// nothing today, but a wedged child that ignores a closed pipe is exactly what
// the job object exists to end -- and KILL_ON_JOB_CLOSE means a worker that dies
// mid-transcode does not leave an encoder running.
//
// READING THE PIPE WHILE THE CHILD RUNS. A child that fills its stdout buffer
// blocks forever, and `-progress pipe:1` writes continuously. So the pipe is
// drained on the calling thread as part of waiting, rather than after.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "morph/core/result.h"

namespace morph::ffmpeg {

struct RunResult {
    // The process's exit code. FFmpeg uses 0 for success and 1 for everything
    // else, with the reason on stderr.
    std::uint32_t exitCode = 0;
    // Everything the child wrote to stderr, capped. With -loglevel error this is
    // the diagnosis when something fails, and empty when nothing did.
    std::string diagnostics;
    bool cancelled = false;
};

// Runs `executable` with `arguments`, feeding every complete stdout line to
// `onLine` as it arrives.
//
// `shouldStop` is polled between reads; returning true terminates the child's
// job object and reports cancelled. Section 11.3 asks for a check at 100 ms
// granularity, which a progress-emitting FFmpeg comfortably provides.
[[nodiscard]] Result RunProcess(const std::wstring& executable,
                                const std::vector<std::wstring>& arguments,
                                const std::function<void(std::string_view)>& onLine,
                                const std::function<bool()>& shouldStop, RunResult& into);

// Runs a process purely for what it prints, with no progress and no
// cancellation. Used for `ffmpeg -version` and for ffprobe, both of which finish
// in milliseconds and produce a few hundred bytes.
[[nodiscard]] Result RunAndCapture(const std::wstring& executable,
                                   const std::vector<std::wstring>& arguments,
                                   std::string& output, std::uint32_t timeoutMs = 10000);

// Quotes one argument the way CommandLineToArgvW will take it apart again.
// Exposed because it is the sort of thing that is wrong for years in a corner
// case nobody hits until a user has a quote in a filename.
[[nodiscard]] std::wstring QuoteArgument(const std::wstring& argument);

}  // namespace morph::ffmpeg
