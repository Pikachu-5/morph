#include "morph/core/graph.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <utility>

namespace morph {
namespace {

constexpr std::uint32_t kInvalidNode = static_cast<std::uint32_t>(NodeId::kInvalid);

}  // namespace

std::size_t FormatGraph::Index(NodeId id) const {
    const auto raw = static_cast<std::uint32_t>(id);
    if (raw >= nodes_.size()) throw std::out_of_range("morph: NodeId out of range");
    return raw;
}

NodeId FormatGraph::AddNode(const MediaProfile& profile) {
    if (profile.Empty()) throw std::invalid_argument("morph: empty MediaProfile");
    if (const auto it = by_id_.find(profile.Id()); it != by_id_.end()) return it->second;
    if (frozen_) throw std::logic_error("morph: FormatGraph mutated after Freeze()");

    const auto id = static_cast<NodeId>(static_cast<std::uint32_t>(nodes_.size()));
    nodes_.push_back(profile);
    outgoing_.emplace_back();
    by_id_.emplace(profile.Id(), id);
    return id;
}

EdgeId FormatGraph::AddEdge(const MediaProfile& from, const MediaProfile& to,
                            EdgeCost cost, std::string_view provider) {
    if (frozen_) throw std::logic_error("morph: FormatGraph mutated after Freeze()");
    if (provider.empty()) throw std::invalid_argument("morph: edge with no provider");

    const NodeId from_id = AddNode(from);
    const NodeId to_id = AddNode(to);
    if (from_id == to_id) {
        // A self-edge is always either a no-op or a bug in a provider table, and
        // it can only ever make a path worse. Reject it loudly at registration.
        throw std::invalid_argument("morph: self-edge on " + from.Id());
    }

    cost.provider.assign(provider);
    const auto id = static_cast<EdgeId>(static_cast<std::uint32_t>(edges_.size()));
    edges_.push_back(Edge{from_id, to_id, std::move(cost)});
    outgoing_[Index(from_id)].push_back(id);
    return id;
}

EdgeId FormatGraph::AddEdge(const EdgeDecl& decl, std::string_view provider) {
    return AddEdge(decl.from, decl.to, decl.cost, provider);
}

void FormatGraph::Freeze() {
    // Deterministic adjacency order. Providers register in table order and the
    // planner's tie-break falls through to edge order, so without this the plan
    // for a fixed graph could depend on registration order alone.
    for (auto& adjacency : outgoing_) {
        std::sort(adjacency.begin(), adjacency.end(), [this](EdgeId a, EdgeId b) {
            const Edge& ea = edges_[static_cast<std::uint32_t>(a)];
            const Edge& eb = edges_[static_cast<std::uint32_t>(b)];
            if (ea.to != eb.to) {
                return nodes_[Index(ea.to)].Id() < nodes_[Index(eb.to)].Id();
            }
            if (ea.cost.provider != eb.cost.provider) {
                return ea.cost.provider < eb.cost.provider;
            }
            return static_cast<std::uint32_t>(a) < static_cast<std::uint32_t>(b);
        });
    }
    frozen_ = true;
}

std::optional<NodeId> FormatGraph::Find(const MediaProfile& profile) const noexcept {
    return Find(profile.Id());
}

std::optional<NodeId> FormatGraph::Find(std::string_view profile_id) const noexcept {
    const auto it = by_id_.find(std::string(profile_id));
    if (it == by_id_.end()) return std::nullopt;
    return it->second;
}

const MediaProfile& FormatGraph::Node(NodeId id) const { return nodes_[Index(id)]; }

const EdgeCost& FormatGraph::Cost(EdgeId id) const {
    const auto raw = static_cast<std::uint32_t>(id);
    if (raw >= edges_.size()) throw std::out_of_range("morph: EdgeId out of range");
    return edges_[raw].cost;
}

NodeId FormatGraph::From(EdgeId id) const {
    const auto raw = static_cast<std::uint32_t>(id);
    if (raw >= edges_.size()) throw std::out_of_range("morph: EdgeId out of range");
    return edges_[raw].from;
}

NodeId FormatGraph::To(EdgeId id) const {
    const auto raw = static_cast<std::uint32_t>(id);
    if (raw >= edges_.size()) throw std::out_of_range("morph: EdgeId out of range");
    return edges_[raw].to;
}

const std::vector<EdgeId>& FormatGraph::Outgoing(NodeId id) const {
    return outgoing_[Index(id)];
}

void FormatGraph::RemoveProviderEdges(const std::vector<ProviderId>& unavailable) {
    if (unavailable.empty()) return;
    const auto dropped = [&](const ProviderId& p) {
        return std::find(unavailable.begin(), unavailable.end(), p) != unavailable.end();
    };

    // Edges are addressed by index, so compact and remap rather than erasing in
    // place: EdgeIds held by a cached plan are invalidated either way, but the
    // adjacency lists must stay internally consistent.
    std::vector<Edge> kept;
    kept.reserve(edges_.size());
    for (auto& adjacency : outgoing_) adjacency.clear();

    for (const Edge& edge : edges_) {
        if (dropped(edge.cost.provider)) continue;
        const auto id = static_cast<EdgeId>(static_cast<std::uint32_t>(kept.size()));
        outgoing_[Index(edge.from)].push_back(id);
        kept.push_back(edge);
    }
    edges_ = std::move(kept);

    // Nodes are deliberately left in place even when they become unreachable:
    // the UI needs to say "HEIF needs a codec from the Store" rather than
    // pretending the format does not exist (section 6.2).
    frozen_ = false;
    Freeze();
}

}  // namespace morph
