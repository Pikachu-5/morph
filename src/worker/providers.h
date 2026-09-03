// The worker's provider table (architecture section 5.1).
//
// "v1 providers are statically linked into the worker and registered from a
//  table. The interface is designed so that v2 can load out-of-tree providers
//  from an MSIX optional package without changing any provider code -- but do
//  not build the dynamic loading in v1. It is the single easiest way to turn a
//  three-month project into a nine-month project."
//
// So this is the table, and it is deliberately the dullest file in the tree.
#pragma once

#include "morph/core/executor.h"
#include "morph/core/graph.h"

namespace morph::worker {

// Every provider this build contains, and the graph they describe.
//
// Held by value in one place because the providers are stateless singletons and
// their lifetime is the process. Section 3.1: the worker is expendable and lives
// for one job batch, so there is nothing to tear down carefully.
class Providers {
public:
    Providers();

    Providers(const Providers&) = delete;
    Providers& operator=(const Providers&) = delete;

    [[nodiscard]] Executor& executor() noexcept { return executor_; }
    [[nodiscard]] const FormatGraph& graph() const noexcept { return graph_; }

    // Probes the codecs and OS components that are not guaranteed to be present,
    // then removes the edges belonging to anything missing (section 6.2). Called
    // once at startup, before the graph is used for planning.
    void ProbeCapabilities();

    [[nodiscard]] const Capabilities& capabilities() const noexcept { return capabilities_; }

private:
    Executor executor_;
    FormatGraph graph_;
    Capabilities capabilities_;
};

}  // namespace morph::worker
