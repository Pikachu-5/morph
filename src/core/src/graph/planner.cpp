#include "morph/core/planner.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <queue>
#include <stdexcept>

// WHY THIS IS NOT ONE DIJKSTRA RUN
//
// Section 4.2 specifies lexicographic comparison and then says "plain Dijkstra
// with a binary heap resolves in microseconds". The comparison is right; running
// one Dijkstra over it is not, and the reason is worth stating because it is a
// silent wrong-answer bug rather than a crash.
//
// Dijkstra needs its cost to be MONOTONE: if path a is better than path b, then
// a extended by edge e must still be better than b extended by e. Three of the
// four components are additive and non-negative, so they are monotone. The
// fourth, worstFidelity, folds with max(), and max is not monotone under
// lexicographic tie-breaking:
//
//   a = { Lossless, gen 5 }        b = { High, gen 0 }         a beats b
//   extend both by e = { Rasterized, gen 0 }
//   a+e = { Rasterized, gen 5 }    b+e = { Rasterized, gen 0 }  now b wins
//
// The max component collapsed and the tiebreak reversed the order. A single
// Dijkstra settles the node on a, discards b, and returns a path that is not the
// minimum under Better().
//
// The fix is to make the max component constant. Fidelity has five values, so we
// sweep: for each ceiling F, restrict the graph to edges of fidelity <= F and
// search with a purely additive cost, which IS monotone. The best path under
// Better() is the best across the five sweeps. Five runs over a graph of a few
// hundred nodes is still microseconds, and now it is correct.
//
// The hop cap is handled the same way, as structure rather than as a filter.
// hopCount is only the THIRD key, so a node's winning label can have more hops
// than a label that lost, and pruning after the fact would discard a route that
// was still extensible. Searching over (node, hops) states makes the cap exact.

namespace morph {
namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();

// Additive-only cost, valid inside a single fidelity sweep.
struct Label {
    std::uint32_t generationalSum = 0;
    double cpuPerMiB = 0.0;  // scaled to ms by the caller's input size

    friend bool operator<(const Label& a, const Label& b) noexcept {
        if (a.generationalSum != b.generationalSum) return a.generationalSum < b.generationalSum;
        return a.cpuPerMiB < b.cpuPerMiB;
    }
};

struct State {
    std::uint32_t node = 0;
    std::uint32_t hops = 0;
};

struct QueueEntry {
    Label label;
    State state;

    // Strict weak ordering for the max-heap, made total so that pop order is
    // determined by the graph and not by heap implementation details. Section 16
    // asserts the planner is deterministic for a fixed graph.
    friend bool operator<(const QueueEntry& a, const QueueEntry& b) noexcept {
        if (a.label < b.label) return false;
        if (b.label < a.label) return true;
        if (a.state.node != b.state.node) return a.state.node > b.state.node;
        return a.state.hops > b.state.hops;
    }
};

struct SweepResult {
    bool reached = false;
    Label label;
    std::uint32_t hops = 0;
};

// One sweep: additive Dijkstra over (node, hops) states, restricted to edges no
// worse than ceiling. predecessor is indexed by state and lets the caller walk
// the route back.
std::vector<SweepResult> Sweep(const FormatGraph& graph, std::uint32_t source,
                               Fidelity ceiling, std::uint32_t max_hops,
                               std::vector<EdgeId>& predecessor) {
    const auto node_count = static_cast<std::uint32_t>(graph.NodeCount());
    const std::uint32_t layers = max_hops + 1;
    const auto states = static_cast<std::size_t>(node_count) * layers;

    const auto index = [layers](std::uint32_t node, std::uint32_t hops) {
        return static_cast<std::size_t>(node) * layers + hops;
    };

    std::vector<Label> best(states, Label{std::numeric_limits<std::uint32_t>::max(), kInfinity});
    std::vector<bool> settled(states, false);
    predecessor.assign(states, EdgeId::kInvalid);

    std::priority_queue<QueueEntry> queue;
    best[index(source, 0)] = Label{};
    queue.push(QueueEntry{Label{}, State{source, 0}});

    while (!queue.empty()) {
        const QueueEntry top = queue.top();
        queue.pop();

        const std::size_t here = index(top.state.node, top.state.hops);
        if (settled[here]) continue;      // stale heap entry
        if (best[here] < top.label) continue;
        settled[here] = true;

        if (top.state.hops == max_hops) continue;  // cannot extend further

        for (const EdgeId edge : graph.Outgoing(static_cast<NodeId>(top.state.node))) {
            const EdgeCost& cost = graph.Cost(edge);
            if (cost.fidelity > ceiling) continue;

            const auto next_node = static_cast<std::uint32_t>(graph.To(edge));
            const std::uint32_t next_hops = top.state.hops + 1;
            const std::size_t there = index(next_node, next_hops);
            if (settled[there]) continue;

            const Label candidate{top.label.generationalSum + cost.generational,
                                  top.label.cpuPerMiB + static_cast<double>(cost.cpuPerMiB)};
            if (!(candidate < best[there])) continue;

            best[there] = candidate;
            predecessor[there] = edge;
            queue.push(QueueEntry{candidate, State{next_node, next_hops}});
        }
    }

    // Collapse the layers: per node, the best label over all hop counts, keeping
    // the fewest hops on a tie because hopCount outranks time in Better().
    std::vector<SweepResult> out(node_count);
    for (std::uint32_t node = 0; node < node_count; ++node) {
        for (std::uint32_t hops = (node == source ? 0u : 1u); hops <= max_hops; ++hops) {
            const std::size_t here = index(node, hops);
            if (!settled[here]) continue;
            if (out[node].reached && !(best[here] < out[node].label)) continue;
            out[node] = SweepResult{true, best[here], hops};
        }
    }
    return out;
}

// Walks predecessor back from (node, hops) and fills in the aggregate cost.
Plan Reconstruct(const FormatGraph& graph, const std::vector<EdgeId>& predecessor,
                 std::uint32_t node, std::uint32_t hops, std::uint32_t max_hops,
                 double input_size_mib) {
    const std::uint32_t layers = max_hops + 1;
    Plan plan;
    plan.hops.reserve(hops);

    std::uint32_t at_node = node;
    std::uint32_t at_hops = hops;
    while (at_hops > 0) {
        const EdgeId edge = predecessor[static_cast<std::size_t>(at_node) * layers + at_hops];
        if (edge == EdgeId::kInvalid) throw std::logic_error("morph: broken plan predecessor");

        const NodeId from = graph.From(edge);
        plan.hops.push_back(PlanHop{edge, from, static_cast<NodeId>(at_node)});
        at_node = static_cast<std::uint32_t>(from);
        --at_hops;
    }
    std::reverse(plan.hops.begin(), plan.hops.end());

    double cpu_per_mib = 0.0;
    for (const PlanHop& hop : plan.hops) {
        const EdgeCost& cost = graph.Cost(hop.edge);
        plan.cost.worstFidelity = Worst(plan.cost.worstFidelity, cost.fidelity);
        plan.cost.generationalSum += cost.generational;
        cpu_per_mib += static_cast<double>(cost.cpuPerMiB);
        plan.requiresDisk = plan.requiresDisk || cost.requiresDisk;
        // Hops stream into one another, so peak memory is the worst single hop
        // rather than the sum (section 4.3).
        plan.memPeakFactor = std::max(plan.memPeakFactor, cost.memPeakFactor);
    }
    plan.cost.hopCount = static_cast<std::uint32_t>(plan.hops.size());
    plan.cost.estimatedMs = cpu_per_mib * input_size_mib;
    return plan;
}

constexpr Fidelity kCeilings[] = {Fidelity::Lossless, Fidelity::High, Fidelity::Structural,
                                  Fidelity::TextOnly, Fidelity::Rasterized};

void RequireFrozen(const FormatGraph& graph) {
    if (!graph.Frozen()) {
        throw std::logic_error("morph: FormatGraph must be frozen before planning");
    }
}

}  // namespace

PlanResult FindPlan(const FormatGraph& graph, const PlanRequest& request) {
    RequireFrozen(graph);

    const auto source = graph.Find(request.source);
    if (!source) return PlanResult{std::nullopt, PlanError::kUnknownSource};
    const auto target = graph.Find(request.target);
    if (!target) return PlanResult{std::nullopt, PlanError::kUnknownTarget};

    if (*source == *target) {
        // Converting a file to the format it already is. The planner returns a
        // zero-hop plan and the caller decides whether that is a copy or a
        // no-op; it must not be an error.
        Plan same;
        same.cost = PathCost{Fidelity::Lossless, 0, 0, 0.0};
        return PlanResult{std::move(same), PlanError::kNoRoute};
    }

    const auto target_index = static_cast<std::uint32_t>(*target);
    std::optional<Plan> best;

    for (const Fidelity ceiling : kCeilings) {
        if (ceiling > request.maxFidelity) break;

        std::vector<EdgeId> predecessor;
        const std::vector<SweepResult> results = Sweep(
            graph, static_cast<std::uint32_t>(*source), ceiling, request.maxHops, predecessor);
        if (!results[target_index].reached) continue;

        Plan candidate = Reconstruct(graph, predecessor, target_index, results[target_index].hops,
                                     request.maxHops, request.inputSizeMiB);
        if (!best || Better(candidate.cost, best->cost)) best = std::move(candidate);

        // The sweep at ceiling F found a path whose actual worst rating is F.
        // No higher ceiling can beat it, because worstFidelity is the first key
        // and admitting worse edges cannot lower it.
        if (best->cost.worstFidelity <= ceiling) break;
    }

    if (!best) return PlanResult{std::nullopt, PlanError::kNoRoute};
    return PlanResult{std::move(best), PlanError::kNoRoute};
}

std::vector<std::pair<NodeId, Plan>> FindAllTargets(const FormatGraph& graph,
                                                    const PlanRequest& request) {
    RequireFrozen(graph);

    const auto source = graph.Find(request.source);
    if (!source) return {};
    const auto source_index = static_cast<std::uint32_t>(*source);

    std::vector<std::optional<Plan>> best(graph.NodeCount());

    for (const Fidelity ceiling : kCeilings) {
        if (ceiling > request.maxFidelity) break;

        std::vector<EdgeId> predecessor;
        const std::vector<SweepResult> results =
            Sweep(graph, source_index, ceiling, request.maxHops, predecessor);

        for (std::uint32_t node = 0; node < results.size(); ++node) {
            if (node == source_index || !results[node].reached) continue;
            // Once a node has a plan whose actual rating is at or below this
            // ceiling, no later sweep can improve it.
            if (best[node] && best[node]->cost.worstFidelity <= ceiling) continue;

            Plan candidate = Reconstruct(graph, predecessor, node, results[node].hops,
                                         request.maxHops, request.inputSizeMiB);
            if (!best[node] || Better(candidate.cost, best[node]->cost)) {
                best[node] = std::move(candidate);
            }
        }
    }

    std::vector<std::pair<NodeId, Plan>> out;
    out.reserve(best.size());
    for (std::uint32_t node = 0; node < best.size(); ++node) {
        if (best[node]) out.emplace_back(static_cast<NodeId>(node), std::move(*best[node]));
    }
    // Deterministic order for the capability table and the shell submenu.
    std::sort(out.begin(), out.end(), [&graph](const auto& a, const auto& b) {
        return graph.Node(a.first).Id() < graph.Node(b.first).Id();
    });
    return out;
}

std::size_t PlanCache::KeyHash::operator()(const Key& k) const noexcept {
    // FNV-1a over the two ids, then mixed with the scalar fields.
    std::uint64_t h = 1469598103934665603ull;
    const auto eat = [&h](std::string_view s) {
        for (const char c : s) {
            h ^= static_cast<unsigned char>(c);
            h *= 1099511628211ull;
        }
        h ^= 0xFFu;  // separator, so ("ab","c") and ("a","bc") differ
        h *= 1099511628211ull;
    };
    eat(k.source);
    eat(k.target);
    h ^= k.optionHash;
    h *= 1099511628211ull;
    h ^= k.maxHops;
    h *= 1099511628211ull;
    h ^= static_cast<std::uint8_t>(k.maxFidelity);
    h *= 1099511628211ull;
    return static_cast<std::size_t>(h);
}

PlanResult PlanCache::Get(const PlanRequest& request, std::uint64_t optionHash) {
    const Key key{request.source, request.target, optionHash, request.maxHops,
                  request.maxFidelity};

    auto it = entries_.find(key);
    if (it == entries_.end()) {
        // Cache the route computed at unit size; estimatedMs is rescaled below.
        // Route choice is size-invariant because every hop's estimate scales by
        // the same positive factor (see PlanRequest::inputSizeMiB).
        PlanRequest normalised = request;
        normalised.inputSizeMiB = 1.0;
        it = entries_.emplace(key, FindPlan(*graph_, normalised)).first;
    }

    PlanResult result = it->second;
    if (result.plan) result.plan->cost.estimatedMs *= request.inputSizeMiB;
    return result;
}

void PlanCache::Clear() { entries_.clear(); }

std::size_t PlanCache::Size() const { return entries_.size(); }

}  // namespace morph
