// Morph — the path planner (architecture section 4.2).
//
// Naive Dijkstra over a scalar cost picks fast, ugly routes. Morph compares
// LEXICOGRAPHICALLY instead: minimise irreversible loss first, then generational
// lossy steps, then hop count, then wall-clock estimate. A route that takes
// twice as long but avoids a second JPEG recompression always wins.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "morph/core/fidelity.h"
#include "morph/core/graph.h"

namespace morph {

// The aggregate cost of a complete path. Note the shapes: fidelity FOLDS with
// Worst(), the rest SUM. That difference is why the search is not one Dijkstra
// run — see planner.cpp.
struct PathCost {
    Fidelity worstFidelity = Fidelity::Lossless;
    std::uint32_t generationalSum = 0;
    std::uint32_t hopCount = 0;
    double estimatedMs = 0.0;
};

// Lexicographic edge relaxation. Loss dominates; time is a tiebreak.
// This is the published comparator: it defines what "the best path" means, and
// the search in planner.cpp is required to return the minimum under it.
[[nodiscard]] constexpr bool Better(const PathCost& a, const PathCost& b) noexcept {
    if (a.worstFidelity  != b.worstFidelity)  return a.worstFidelity  < b.worstFidelity;
    if (a.generationalSum != b.generationalSum) return a.generationalSum < b.generationalSum;
    if (a.hopCount       != b.hopCount)       return a.hopCount       < b.hopCount;
    return a.estimatedMs < b.estimatedMs;
}

struct PlanHop {
    EdgeId edge = EdgeId::kInvalid;
    NodeId from = NodeId::kInvalid;
    NodeId to = NodeId::kInvalid;
};

struct Plan {
    std::vector<PlanHop> hops;
    PathCost cost;

    // True if any hop needs a temp spill file. The failure mode in section 4.3:
    // hops are otherwise connected by the streaming ISource/ISink pair and never
    // materialise an intermediate.
    bool requiresDisk = false;

    // Peak memory across the path as a multiple of input size, used to derive
    // the job-object budget in section 11.2. Hops stream, so the peak is the
    // worst single hop rather than the sum.
    float memPeakFactor = 1.0f;

    // The rating shown in the UI before the user commits (section 8.8).
    [[nodiscard]] Fidelity fidelity() const noexcept { return cost.worstFidelity; }
    [[nodiscard]] bool direct() const noexcept { return hops.size() == 1; }
};

struct PlanRequest {
    std::string source;  // canonical MediaProfile id
    std::string target;  // canonical MediaProfile id

    // Input size, used only to turn cpuPerMiB into an estimate. It does not
    // change which route is chosen: every hop's estimate scales by the same
    // positive factor, so the ordering is size-invariant. That is what makes the
    // route cacheable independently of the file (see PlanCache).
    double inputSizeMiB = 1.0;

    // Section 16 asserts no path exceeds 5 hops. Enforced as a hard constraint
    // during the search, not checked afterwards.
    std::uint32_t maxHops = 5;

    // Refuse paths above this rating outright. The UI uses it for "lossless
    // only" mode; the default admits everything and lets the badge do the
    // talking.
    Fidelity maxFidelity = Fidelity::Rasterized;
};

enum class PlanError {
    kUnknownSource,   // the source profile is not a node in the graph
    kUnknownTarget,   // the target profile is not a node in the graph
    kNoRoute,         // no path within maxHops and maxFidelity
};

struct PlanResult {
    std::optional<Plan> plan;
    PlanError error = PlanError::kNoRoute;

    [[nodiscard]] explicit operator bool() const noexcept { return plan.has_value(); }
};

// Stateless with respect to the graph; the graph must outlive the planner and
// must be frozen. Thread-safe for concurrent Plan() calls.
[[nodiscard]] PlanResult FindPlan(const FormatGraph& graph, const PlanRequest& request);

// Every target reachable from one source, with its cost. This is what backs
// QueryTargets (section 10.2) and the capability table the shell extension
// memory-maps (section 9.2). One sweep, not one FindPlan() call per target.
[[nodiscard]] std::vector<std::pair<NodeId, Plan>> FindAllTargets(
    const FormatGraph& graph, const PlanRequest& request);

// Plans are cached by (source, target, option hash) as section 4.2 specifies.
// The cached value is the ROUTE; estimatedMs is recomputed per call because it
// is the only size-dependent field.
class PlanCache {
public:
    explicit PlanCache(const FormatGraph& graph) : graph_(&graph) {}

    [[nodiscard]] PlanResult Get(const PlanRequest& request, std::uint64_t optionHash = 0);
    void Clear();
    [[nodiscard]] std::size_t Size() const;

private:
    struct Key {
        std::string source;
        std::string target;
        std::uint64_t optionHash;
        std::uint32_t maxHops;
        Fidelity maxFidelity;

        friend bool operator==(const Key&, const Key&) noexcept = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept;
    };

    const FormatGraph* graph_;
    std::unordered_map<Key, PlanResult, KeyHash> entries_;
};

}  // namespace morph
