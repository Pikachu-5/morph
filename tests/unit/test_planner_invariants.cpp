// The graph invariants from architecture section 16: every reachable node pair
// has a path, no path exceeds five hops, and the planner is deterministic for a
// fixed graph. Plus the one that matters most and is easiest to get wrong: the
// plan the planner returns really is the minimum under Better().
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "graph_fixture.h"
#include "morph/core/planner.h"

using namespace morph;

namespace {

std::vector<std::string> AllProfileIds(const FormatGraph& g) {
    std::vector<std::string> out;
    out.reserve(g.NodeCount());
    for (const MediaProfile& p : g.Nodes()) out.push_back(p.Id());
    return out;
}

// Plain breadth-first reachability, ignoring cost entirely. This is the ground
// truth the planner is checked against: if BFS can get there, the planner must.
std::vector<bool> Reachable(const FormatGraph& g, NodeId source) {
    std::vector<bool> seen(g.NodeCount(), false);
    std::deque<NodeId> queue{source};
    seen[static_cast<std::uint32_t>(source)] = true;

    while (!queue.empty()) {
        const NodeId at = queue.front();
        queue.pop_front();
        for (const EdgeId edge : g.Outgoing(at)) {
            const NodeId next = g.To(edge);
            if (seen[static_cast<std::uint32_t>(next)]) continue;
            seen[static_cast<std::uint32_t>(next)] = true;
            queue.push_back(next);
        }
    }
    return seen;
}

// Exhaustive enumeration of every simple path within the hop cap, folded into a
// PathCost the same way the planner does it. Exponential, and fine on a fixture
// of this size; this is the oracle, not the implementation.
void EnumeratePaths(const FormatGraph& g, NodeId at, NodeId target, std::uint32_t hops_left,
                    std::vector<bool>& on_path, PathCost running,
                    std::optional<PathCost>& best) {
    if (at == target && running.hopCount > 0) {
        if (!best || Better(running, *best)) best = running;
        return;
    }
    if (hops_left == 0) return;

    for (const EdgeId edge : g.Outgoing(at)) {
        const NodeId next = g.To(edge);
        const auto next_index = static_cast<std::uint32_t>(next);
        if (on_path[next_index]) continue;  // a cycle can only make a path worse

        const EdgeCost& cost = g.Cost(edge);
        PathCost extended = running;
        extended.worstFidelity = Worst(extended.worstFidelity, cost.fidelity);
        extended.generationalSum += cost.generational;
        extended.hopCount += 1;
        extended.estimatedMs += static_cast<double>(cost.cpuPerMiB);

        on_path[next_index] = true;
        EnumeratePaths(g, next, target, hops_left - 1, on_path, extended, best);
        on_path[next_index] = false;
    }
}

std::optional<PathCost> BruteForceBest(const FormatGraph& g, NodeId source, NodeId target,
                                       std::uint32_t max_hops) {
    std::vector<bool> on_path(g.NodeCount(), false);
    on_path[static_cast<std::uint32_t>(source)] = true;
    std::optional<PathCost> best;
    EnumeratePaths(g, source, target, max_hops, on_path, PathCost{}, best);
    return best;
}

PlanRequest Request(std::string_view from, std::string_view to) {
    PlanRequest r;
    r.source = std::string(from);
    r.target = std::string(to);
    return r;
}

}  // namespace

TEST_CASE("every BFS-reachable pair is reachable within the five-hop cap",
          "[planner][invariant]") {
    const FormatGraph g = test::MakeTestGraph();
    const std::vector<std::string> ids = AllProfileIds(g);

    for (std::uint32_t s = 0; s < ids.size(); ++s) {
        const std::vector<bool> reachable = Reachable(g, static_cast<NodeId>(s));
        for (std::uint32_t t = 0; t < ids.size(); ++t) {
            if (s == t || !reachable[t]) continue;

            const PlanResult result = FindPlan(g, Request(ids[s], ids[t]));
            INFO(ids[s] << " -> " << ids[t]);
            REQUIRE(result);
            CHECK(result.plan->cost.hopCount <= 5);
        }
    }
}

TEST_CASE("an unreachable pair is never given a plan", "[planner][invariant]") {
    const FormatGraph g = test::MakeTestGraph();
    const std::vector<std::string> ids = AllProfileIds(g);

    for (std::uint32_t s = 0; s < ids.size(); ++s) {
        const std::vector<bool> reachable = Reachable(g, static_cast<NodeId>(s));
        for (std::uint32_t t = 0; t < ids.size(); ++t) {
            if (s == t || reachable[t]) continue;
            INFO(ids[s] << " -> " << ids[t]);
            CHECK_FALSE(FindPlan(g, Request(ids[s], ids[t])));
        }
    }
}

TEST_CASE("the returned plan is the minimum under Better", "[planner][invariant]") {
    // Checked against exhaustive enumeration for every ordered pair. This is the
    // assertion that would have caught the non-monotone fidelity bug described
    // at the top of planner.cpp.
    const FormatGraph g = test::MakeTestGraph();
    const std::vector<std::string> ids = AllProfileIds(g);

    for (std::uint32_t s = 0; s < ids.size(); ++s) {
        for (std::uint32_t t = 0; t < ids.size(); ++t) {
            if (s == t) continue;

            const std::optional<PathCost> oracle =
                BruteForceBest(g, static_cast<NodeId>(s), static_cast<NodeId>(t), 5);
            const PlanResult result = FindPlan(g, Request(ids[s], ids[t]));

            INFO(ids[s] << " -> " << ids[t]);
            REQUIRE(oracle.has_value() == result.plan.has_value());
            if (!oracle) continue;

            // Ties are allowed: a different route with an identical cost vector
            // is equally correct. What is not allowed is a worse one.
            CHECK_FALSE(Better(*oracle, result.plan->cost));
            CHECK_FALSE(Better(result.plan->cost, *oracle));
        }
    }
}

TEST_CASE("the planner is deterministic for a fixed graph", "[planner][invariant]") {
    const FormatGraph g = test::MakeTestGraph();
    const std::vector<std::string> ids = AllProfileIds(g);

    const auto route_of = [&g](const PlanResult& r) {
        std::vector<std::uint32_t> out;
        if (!r.plan) return out;
        for (const PlanHop& hop : r.plan->hops) out.push_back(static_cast<std::uint32_t>(hop.edge));
        return out;
    };

    for (const std::string& source : ids) {
        for (const std::string& target : ids) {
            const auto first = route_of(FindPlan(g, Request(source, target)));
            for (int repeat = 0; repeat < 8; ++repeat) {
                INFO(source << " -> " << target << ", repeat " << repeat);
                CHECK(route_of(FindPlan(g, Request(source, target))) == first);
            }
        }
    }
}

TEST_CASE("a plan's hops form an unbroken chain from source to target",
          "[planner][invariant]") {
    const FormatGraph g = test::MakeTestGraph();
    const std::vector<std::string> ids = AllProfileIds(g);

    for (const std::string& source : ids) {
        for (const std::string& target : ids) {
            const PlanResult result = FindPlan(g, Request(source, target));
            if (!result || result.plan->hops.empty()) continue;

            INFO(source << " -> " << target);
            CHECK(g.Node(result.plan->hops.front().from).Id() == source);
            CHECK(g.Node(result.plan->hops.back().to).Id() == target);

            for (std::size_t i = 0; i + 1 < result.plan->hops.size(); ++i) {
                CHECK(result.plan->hops[i].to == result.plan->hops[i + 1].from);
            }
            // Each hop's endpoints must match the edge it claims to use.
            for (const PlanHop& hop : result.plan->hops) {
                CHECK(g.From(hop.edge) == hop.from);
                CHECK(g.To(hop.edge) == hop.to);
            }
        }
    }
}

TEST_CASE("aggregate cost matches a fold over the plan's own hops",
          "[planner][invariant]") {
    const FormatGraph g = test::MakeTestGraph();
    const std::vector<std::string> ids = AllProfileIds(g);

    for (const std::string& source : ids) {
        for (const std::string& target : ids) {
            const PlanResult result = FindPlan(g, Request(source, target));
            if (!result || result.plan->hops.empty()) continue;

            PathCost recomputed;
            for (const PlanHop& hop : result.plan->hops) {
                const EdgeCost& cost = g.Cost(hop.edge);
                recomputed.worstFidelity = Worst(recomputed.worstFidelity, cost.fidelity);
                recomputed.generationalSum += cost.generational;
                recomputed.hopCount += 1;
                recomputed.estimatedMs += static_cast<double>(cost.cpuPerMiB);
            }

            INFO(source << " -> " << target);
            CHECK(recomputed.worstFidelity == result.plan->cost.worstFidelity);
            CHECK(recomputed.generationalSum == result.plan->cost.generationalSum);
            CHECK(recomputed.hopCount == result.plan->cost.hopCount);
        }
    }
}

TEST_CASE("Better is a strict weak ordering", "[planner][invariant]") {
    const std::vector<PathCost> samples = {
        PathCost{Fidelity::Lossless, 0, 1, 10.0},   PathCost{Fidelity::Lossless, 0, 1, 20.0},
        PathCost{Fidelity::Lossless, 0, 2, 5.0},    PathCost{Fidelity::Lossless, 1, 1, 5.0},
        PathCost{Fidelity::High, 0, 1, 1.0},        PathCost{Fidelity::Rasterized, 5, 4, 900.0},
    };

    for (const PathCost& a : samples) {
        CHECK_FALSE(Better(a, a));  // irreflexive
        for (const PathCost& b : samples) {
            if (Better(a, b)) CHECK_FALSE(Better(b, a));  // asymmetric
            for (const PathCost& c : samples) {
                if (Better(a, b) && Better(b, c)) CHECK(Better(a, c));  // transitive
            }
        }
    }
}
