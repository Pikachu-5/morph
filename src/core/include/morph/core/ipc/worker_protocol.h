// Morph — the broker-to-worker channel (architecture sections 3.1 and 11).
//
// Section 10.2's table is the CLIENT protocol and lives in protocol.h. This is
// the other conversation: the one the broker has with the workers it spawns. It
// is deliberately a separate header, because the two have opposite audiences.
// The client protocol is a published contract that a C# rewrite of the app must
// be able to speak (section 12.3); this one is private plumbing between two
// copies of our own code and can change in any release.
//
// It exists at all because of the process rule in section 3.1. The broker "owns
// the graph, the queue, and worker lifetime" and "must never parse a user file".
// Both halves of that sentence have to be true at once, and they pull against
// each other:
//
//   * The graph is built from what providers DECLARE (see edge.h: "the broker
//     builds the graph from these"), and the declarations live in the worker
//     along with the providers themselves. So the worker describes the graph and
//     the broker builds its own copy from the description. The description also
//     carries which capabilities probed present, so the broker's graph reflects
//     the machine rather than the build (section 6.2).
//
//   * Identifying an input means reading 8 KiB of a file the user did not vouch
//     for, which is precisely what the broker may not do. So probing is a
//     request the broker sends and the worker answers.
//
// The alternative -- linking the provider table into the broker so it could
// build the graph and probe locally -- would put every parser in the one process
// that is supposed to survive. That is the trade this file exists to refuse.
//
// The channel reuses Progress, TaskDone and Cancel from protocol.h unchanged. A
// worker reporting progress and the broker forwarding it to a client are the
// same message with the same meaning, and giving them two shapes would mean a
// translation step whose only possible contribution is a bug.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// codec.h rather than protocol.h: the TypeOf specialisations at the bottom need
// the primary template, and the Encode/Decode overloads below belong to the same
// overload set as the client codec's.
#include "morph/core/ipc/codec.h"

namespace morph::ipc {

// --- Worker to broker --------------------------------------------------------

// Sent once, immediately after the worker connects back. The broker matches
// `protocolVersion` against its own and kills the worker on a mismatch, the same
// rule section 10.2 sets for clients: a version mismatch closes the pipe rather
// than negotiating.
struct WorkerHello {
    std::uint32_t protocolVersion = kProtocolVersion;
    std::string workerVersion;
    std::uint64_t processId = 0;
};

// One declared edge, flattened. Mirrors EdgeDecl plus the EdgeCost fields the
// planner reads; anything the planner does not read is not on the wire.
struct EdgeWire {
    std::string from;
    std::string to;
    std::string provider;
    std::uint8_t fidelity = 0;      // morph::Fidelity
    std::uint8_t generational = 0;
    double cpuPerMiB = 0.0;
    double memPeakFactor = 1.0;
    bool requiresDisk = false;
};

// The worker's answer to DescribeGraph: every edge that survived the capability
// probe, and the capability ids that probed present.
struct GraphDescription {
    std::vector<EdgeWire> edges;
    std::vector<std::string> capabilities;
};

// Answers ProbeFiles, one entry per requested path and in the same order. An
// empty string means the worker could not identify that file -- which is a
// normal answer, not an error.
struct ProbeResult {
    std::vector<std::string> profiles;
};

// --- Broker to worker --------------------------------------------------------

struct DescribeGraph {};

struct ProbeFiles {
    std::vector<std::string> paths;
};

// One task. The broker has already planned the route; it sends the target
// profile rather than the hop list because the worker owns the graph the plan
// was made against and re-planning there is cheaper than marshalling a plan and
// far cheaper than the two copies disagreeing.
struct RunTask {
    std::uint64_t jobId = 0;
    std::uint32_t taskIndex = 0;
    std::string inputPath;
    std::string outputPath;
    std::string targetProfile;
    std::vector<OptionValue> options;
};

[[nodiscard]] std::vector<std::byte> Encode(const WorkerHello& message);
[[nodiscard]] std::vector<std::byte> Encode(const GraphDescription& message);
[[nodiscard]] std::vector<std::byte> Encode(const ProbeResult& message);
[[nodiscard]] std::vector<std::byte> Encode(const DescribeGraph& message);
[[nodiscard]] std::vector<std::byte> Encode(const ProbeFiles& message);
[[nodiscard]] std::vector<std::byte> Encode(const RunTask& message);

[[nodiscard]] bool Decode(std::span<const std::byte> payload, WorkerHello& into);
[[nodiscard]] bool Decode(std::span<const std::byte> payload, GraphDescription& into);
[[nodiscard]] bool Decode(std::span<const std::byte> payload, ProbeResult& into);
[[nodiscard]] bool Decode(std::span<const std::byte> payload, DescribeGraph& into);
[[nodiscard]] bool Decode(std::span<const std::byte> payload, ProbeFiles& into);
[[nodiscard]] bool Decode(std::span<const std::byte> payload, RunTask& into);

template <> constexpr MessageType TypeOf<WorkerHello>() noexcept {
    return MessageType::kWorkerHello;
}
template <> constexpr MessageType TypeOf<DescribeGraph>() noexcept {
    return MessageType::kDescribeGraph;
}
template <> constexpr MessageType TypeOf<GraphDescription>() noexcept {
    return MessageType::kGraphDescription;
}
template <> constexpr MessageType TypeOf<ProbeFiles>() noexcept {
    return MessageType::kProbeFiles;
}
template <> constexpr MessageType TypeOf<ProbeResult>() noexcept {
    return MessageType::kProbeResult;
}
template <> constexpr MessageType TypeOf<RunTask>() noexcept { return MessageType::kRunTask; }

}  // namespace morph::ipc
