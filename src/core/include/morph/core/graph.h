// Morph — the format graph (architecture section 4).
//
// "A format graph, not a format list." Conversions are edges in a directed
// weighted graph. Adding one new format wires it to everything else
// automatically instead of requiring N new converters.
//
// The graph is built once by the broker at startup from the providers'
// EnumerateEdges() declarations, then read concurrently by the planner and by
// the capability-table publisher. It is immutable after Freeze().
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "morph/core/edge.h"
#include "morph/core/media_profile.h"

namespace morph {

// Index into FormatGraph's node table. Stable for the lifetime of the graph.
enum class NodeId : std::uint32_t { kInvalid = 0xFFFFFFFFu };

// Index into FormatGraph's edge table. Stable for the lifetime of the graph.
enum class EdgeId : std::uint32_t { kInvalid = 0xFFFFFFFFu };

class FormatGraph {
public:
    FormatGraph() = default;

    // Interns the profile, returning the existing id if it is already present.
    // Rejects empty profiles.
    NodeId AddNode(const MediaProfile& profile);

    // Adds a directed edge and interns both endpoints. `provider` is stamped
    // into the stored cost. Parallel edges between the same pair are allowed and
    // expected: WIC and FFmpeg may both offer PNG -> JPEG at different costs,
    // and the planner picks per path.
    //
    // Must not be called after Freeze().
    EdgeId AddEdge(const MediaProfile& from, const MediaProfile& to,
                   EdgeCost cost, std::string_view provider);

    // Convenience for provider registration tables.
    EdgeId AddEdge(const EdgeDecl& decl, std::string_view provider);

    // Sorts adjacency into a deterministic order and blocks further mutation.
    // The planner requires this: iteration order over parallel edges decides
    // tie-breaks, and section 16 asserts the planner is deterministic for a
    // fixed graph.
    void Freeze();
    [[nodiscard]] bool Frozen() const noexcept { return frozen_; }

    [[nodiscard]] std::size_t NodeCount() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::size_t EdgeCount() const noexcept { return edges_.size(); }

    [[nodiscard]] std::optional<NodeId> Find(const MediaProfile& profile) const noexcept;
    [[nodiscard]] std::optional<NodeId> Find(std::string_view profile_id) const noexcept;

    [[nodiscard]] const MediaProfile& Node(NodeId id) const;
    [[nodiscard]] const EdgeCost& Cost(EdgeId id) const;
    [[nodiscard]] NodeId From(EdgeId id) const;
    [[nodiscard]] NodeId To(EdgeId id) const;

    // Outgoing edges, in deterministic order once frozen.
    [[nodiscard]] const std::vector<EdgeId>& Outgoing(NodeId id) const;

    // All nodes, in insertion order. Used by the capability-table publisher.
    [[nodiscard]] const std::vector<MediaProfile>& Nodes() const noexcept { return nodes_; }

    // Drops every edge whose provider is in `unavailable`, then re-freezes.
    // This is the codec-probe path from section 6.2: HEIF and AVIF depend on
    // Store-delivered codec packages, so those edges are marked unavailable at
    // startup rather than failing mid-job.
    void RemoveProviderEdges(const std::vector<ProviderId>& unavailable);

private:
    [[nodiscard]] std::size_t Index(NodeId id) const;

    struct Edge {
        NodeId from = NodeId::kInvalid;
        NodeId to = NodeId::kInvalid;
        EdgeCost cost;
    };

    std::vector<MediaProfile> nodes_;
    std::unordered_map<std::string, NodeId> by_id_;
    std::vector<Edge> edges_;
    std::vector<std::vector<EdgeId>> outgoing_;
    bool frozen_ = false;
};

}  // namespace morph
