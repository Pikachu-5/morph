// The worker's broker-driven mode (architecture sections 3.1 and 11).
//
// The CLI in main.cpp and this file are two front ends over one engine. Neither
// contains conversion logic: both hand a plan to Executor and differ only in
// where the request comes from and where the answers go.
#pragma once

#include <string>

#include "providers.h"

namespace morph::worker {

// Connects back to the broker on `pipeName` and serves requests until the broker
// closes the pipe or asks for nothing more. Returns a process exit code.
//
// The worker connects OUT to a pipe the broker already created rather than the
// broker connecting in, so that the broker's security descriptor governs the
// channel and the worker -- the low-integrity, hostile-input-facing process --
// never creates a named object anything else could reach (section 15.1).
[[nodiscard]] int Serve(Providers& providers, const std::wstring& pipeName);

}  // namespace morph::worker
