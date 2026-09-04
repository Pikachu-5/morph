// The fuzzable entry points (architecture section 16).
//
// "libFuzzer over the DOCX reader, the Markdown reader, and the OPC container
//  layer. Seed from the golden corpus. Run continuously, not in PR CI."
//
// Each target takes bytes and returns nothing anyone should look at. What is
// being asserted is not what they produce, it is that they RETURN: section 15's
// threat model says every input file is hostile, and section 3.1 says a
// malformed input "is the expected outcome for hostile input and must never be a
// crash".
//
// They are declared here, rather than each file defining LLVMFuzzerTestOneInput,
// because they are used two ways. A libFuzzer build makes one binary per target
// and runs it for hours; the ordinary test suite runs every one of them over a
// deterministic corpus on every build, which is the part that catches a
// regression before it is committed rather than a week later.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace morph::fuzz {

// One entry per parser that ever sees bytes it did not write.
void FuzzMarkdown(std::span<const std::byte> data);
void FuzzHtml(std::span<const std::byte> data);
void FuzzOpc(std::span<const std::byte> data);
void FuzzDocx(std::span<const std::byte> data);

// Not a file parser, and here for the same reason the others are: the frame
// decoder reads a length off a wire, and the capability table reads offsets out
// of a section any process running as this user can write to.
void FuzzFrame(std::span<const std::byte> data);
void FuzzCapabilityTable(std::span<const std::byte> data);

struct Target {
    const char* name;
    void (*run)(std::span<const std::byte>);
};

[[nodiscard]] std::span<const Target> Targets() noexcept;

}  // namespace morph::fuzz
