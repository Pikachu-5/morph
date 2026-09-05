// Where the converted file goes (architecture section 10.2's OutputPolicy).
//
// The policy is its own shape in the protocol because, in the words of that
// section, it is "the part users get wrong and support questions are about". The
// resolution happens here, in the broker, for two reasons: the worker is
// expendable and must not be trusted to decide where a file lands, and the whole
// batch's names have to be decided against one view of the directory or two
// tasks converting a.md and A.MD would race for the same output.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "morph/core/ipc/protocol.h"
// ApplyTemplate used to live here. It moved to the core when the app grew a
// preview of the name a template produces: the broker decides the real one and
// the app predicts it, and two implementations of that would drift silently.
#include "morph/core/naming.h"

namespace morph::broker {

// What resolving one input produced.
struct OutputPlan {
    std::filesystem::path path;
    // The overwrite rule said skip and the file is already there. The task is
    // still reported -- silently doing nothing is how a user loses an afternoon.
    bool skip = false;
    // Empty on success. A policy that cannot produce a path (no extension for
    // the target, a directory that cannot be created) fails the TASK, not the
    // job: the other files in the batch are still convertible.
    std::string error;
};

// Resolves the output path for one input.
//
// `taken` is every path this batch has already claimed, and is updated on
// success. It exists because kRename asks "does this name exist?" and the answer
// has to include names that do not exist YET but will by the time the batch
// finishes.
[[nodiscard]] OutputPlan ResolveOutput(const std::filesystem::path& input,
                                       const std::string& targetProfile,
                                       const ipc::OutputPolicy& policy, std::size_t index,
                                       std::vector<std::filesystem::path>& taken);

}  // namespace morph::broker
