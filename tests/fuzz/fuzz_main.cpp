// The libFuzzer entry point (architecture section 16).
//
// One binary per target, selected at compile time by MORPH_FUZZ_TARGET, because
// libFuzzer's corpus, its coverage counters and its crash reproducers all belong
// to one entry point. Building six targets into one binary would give a fuzzer
// that spends its budget deciding which parser to feed rather than exploring one.
//
// This file is only compiled where the toolchain has libFuzzer -- clang-cl on
// Windows -- and section 16 is explicit that these run "continuously, not in PR
// CI". What runs in PR CI is the deterministic replay in the ordinary test
// suite, which is a different job: catching a regression on inputs that already
// broke something once.

#include <cstddef>
#include <cstdint>
#include <span>

#include "targets.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
    MORPH_FUZZ_TARGET(bytes);
    return 0;
}
