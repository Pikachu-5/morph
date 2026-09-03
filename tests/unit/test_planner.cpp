#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <string>
#include <vector>

#include "graph_fixture.h"
#include "morph/core/planner.h"

using namespace morph;
using morph::test::Cost;
using morph::test::P;
namespace ids = morph::test::ids;

namespace {

// The route as a readable list of profile ids, so a failing assertion names the
// path it actually chose rather than a vector of integers.
std::vector<std::string> Route(const FormatGraph& g, const Plan& plan) {
    std::vector<std::string> out;
    if (plan.hops.empty()) return out;
    out.push_back(g.Node(plan.hops.front().from).Id());
    for (const PlanHop& hop : plan.hops) out.push_back(g.Node(hop.to).Id());
    return out;
}

PlanRequest Request(std::string_view from, std::string_view to) {
    PlanRequest r;
    r.source = std::string(from);
    r.target = std::string(to);
    return r;
}

}  // namespace

// --- The worked examples from section 4.3 -----------------------------------

TEST_CASE("HEIF to WebP takes the direct edge", "[planner][worked]") {
    const FormatGraph g = test::MakeTestGraph();
    const PlanResult result = FindPlan(g, Request(ids::kHeif, ids::kWebp));

    REQUIRE(result);
    CHECK(result.plan->direct());
    CHECK(result.plan->fidelity() == Fidelity::High);
    CHECK(result.plan->cost.generationalSum == 1);
    CHECK(Route(g, *result.plan) == std::vector<std::string>{"image/heif", "image/webp"});
}

TEST_CASE("DOCX to PNG routes through the IR and rasterises only at the end",
          "[planner][worked]") {
    const FormatGraph g = test::MakeTestGraph();
    const PlanResult result = FindPlan(g, Request(ids::kDocx, ids::kPng));

    REQUIRE(result);
    CHECK(Route(g, *result.plan) ==
          std::vector<std::string>{"application/docx", "application/morph-ir", "text/html",
                                   "application/pdf", "image/png"});
    CHECK(result.plan->cost.hopCount == 4);
    // The only hop that loses anything irreversible is the final rasterisation,
    // which is what the user explicitly asked for.
    CHECK(result.plan->fidelity() == Fidelity::Rasterized);
}

TEST_CASE("MP4 to MKV is detected as a remux, not a re-encode", "[planner][worked]") {
    const FormatGraph g = test::MakeTestGraph();
    const PlanResult result = FindPlan(g, Request(ids::kMp4H264, ids::kMkvH264));

    REQUIRE(result);
    CHECK(result.plan->fidelity() == Fidelity::Lossless);
    CHECK(result.plan->cost.generationalSum == 0);
    CHECK(result.plan->direct());
    // Section 17 budgets this path as IO-bound. The estimate must reflect that.
    CHECK(result.plan->cost.estimatedMs < 1.0);
}

TEST_CASE("MP4 to WebM costs one generation and says so", "[planner][worked]") {
    const FormatGraph g = test::MakeTestGraph();
    const PlanResult result = FindPlan(g, Request(ids::kMp4H264, ids::kWebmVp9));

    REQUIRE(result);
    CHECK(result.plan->fidelity() == Fidelity::High);
    CHECK(result.plan->cost.generationalSum == 1);
}

TEST_CASE("PDF to DOCX is structural, not high", "[planner][worked]") {
    // Section 4.3: layout is not recoverable from PDF in the general case, and
    // we say so rather than pretending.
    const FormatGraph g = test::MakeTestGraph();
    const PlanResult result = FindPlan(g, Request(ids::kPdf, ids::kDocx));

    REQUIRE(result);
    CHECK(result.plan->fidelity() == Fidelity::Structural);
    CHECK(Route(g, *result.plan) ==
          std::vector<std::string>{"application/pdf", "application/morph-ir",
                                   "application/docx"});
}

// --- What lexicographic comparison is actually for ---------------------------

TEST_CASE("a slower lossless route beats a fast lossy one", "[planner]") {
    // "A route that takes twice as long but avoids a second JPEG recompression
    // always wins" (section 4.2).
    FormatGraph g;
    g.AddEdge(P("a/x"), P("a/z"), Cost(Fidelity::High, 1, /*cpu=*/1.0f), "fast");
    g.AddEdge(P("a/x"), P("a/y"), Cost(Fidelity::Lossless, 0, /*cpu=*/500.0f), "slow");
    g.AddEdge(P("a/y"), P("a/z"), Cost(Fidelity::Lossless, 0, /*cpu=*/500.0f), "slow");
    g.Freeze();

    const PlanResult result = FindPlan(g, Request("a/x", "a/z"));
    REQUIRE(result);
    CHECK(result.plan->fidelity() == Fidelity::Lossless);
    CHECK(result.plan->cost.hopCount == 2);
    CHECK(result.plan->cost.estimatedMs > 900.0);
}

TEST_CASE("time breaks a tie only when loss and hops are equal", "[planner]") {
    FormatGraph g;
    g.AddEdge(P("a/x"), P("a/z"), Cost(Fidelity::High, 1, 90.0f), "slow");
    g.AddEdge(P("a/x"), P("a/z"), Cost(Fidelity::High, 1, 10.0f), "quick");
    g.Freeze();

    const PlanResult result = FindPlan(g, Request("a/x", "a/z"));
    REQUIRE(result);
    CHECK(g.Cost(result.plan->hops.front().edge).provider == "quick");
}

TEST_CASE("the planner does not fall for the non-monotone fidelity trap", "[planner][regression]") {
    // This is the case a single lexicographic Dijkstra gets WRONG, and it is
    // silent when it does. Two parallel edges reach the middle node:
    //
    //   trap: Lossless but 5 generations   <- wins the comparison at the middle
    //   good: High but 0 generations
    //
    // Both must then pass through a Rasterized edge, which collapses the
    // fidelity component and leaves generations deciding. The trap label wins
    // early, the good label is discarded as inferior, and the returned path
    // carries five generations of loss instead of zero.
    FormatGraph g;
    g.AddEdge(P("a/src"), P("a/mid"), Cost(Fidelity::Lossless, 5, 1.0f), "trap");
    g.AddEdge(P("a/src"), P("a/mid"), Cost(Fidelity::High, 0, 1.0f), "good");
    g.AddEdge(P("a/mid"), P("a/dst"), Cost(Fidelity::Rasterized, 0, 1.0f), "raster");
    g.Freeze();

    const PlanResult result = FindPlan(g, Request("a/src", "a/dst"));
    REQUIRE(result);
    CHECK(result.plan->fidelity() == Fidelity::Rasterized);
    CHECK(result.plan->cost.generationalSum == 0);
    CHECK(g.Cost(result.plan->hops.front().edge).provider == "good");
}

// --- Constraints -------------------------------------------------------------

TEST_CASE("the hop cap is a hard constraint, not a post-hoc check", "[planner]") {
    const FormatGraph g = test::MakeTestGraph();
    PlanRequest request = Request(ids::kDocx, ids::kPng);  // needs four hops
    request.maxHops = 3;

    const PlanResult result = FindPlan(g, request);
    CHECK_FALSE(result);
    CHECK(result.error == PlanError::kNoRoute);

    request.maxHops = 4;
    CHECK(FindPlan(g, request));
}

TEST_CASE("a fidelity ceiling refuses a route rather than degrading it", "[planner]") {
    const FormatGraph g = test::MakeTestGraph();
    PlanRequest request = Request(ids::kDocx, ids::kPng);
    request.maxFidelity = Fidelity::Structural;

    CHECK_FALSE(FindPlan(g, request));

    request.maxFidelity = Fidelity::Rasterized;
    CHECK(FindPlan(g, request));
}

TEST_CASE("an unknown endpoint is reported precisely", "[planner]") {
    const FormatGraph g = test::MakeTestGraph();

    CHECK(FindPlan(g, Request("image/nonexistent", ids::kPng)).error ==
          PlanError::kUnknownSource);
    CHECK(FindPlan(g, Request(ids::kPng, "image/nonexistent")).error ==
          PlanError::kUnknownTarget);
}

TEST_CASE("converting a file to what it already is yields a zero-hop plan", "[planner]") {
    const FormatGraph g = test::MakeTestGraph();
    const PlanResult result = FindPlan(g, Request(ids::kPng, ids::kPng));

    REQUIRE(result);
    CHECK(result.plan->hops.empty());
    CHECK(result.plan->cost.hopCount == 0);
    CHECK(result.plan->fidelity() == Fidelity::Lossless);
}

TEST_CASE("an unreachable target is a clean no-route", "[planner]") {
    const FormatGraph g = test::MakeTestGraph();
    // Nothing in the fixture converts plain text back into anything.
    CHECK_FALSE(FindPlan(g, Request(ids::kTxt, ids::kPng)));
}

TEST_CASE("planning an unfrozen graph is a programming error", "[planner]") {
    FormatGraph g;
    g.AddEdge(P("a/x"), P("a/y"), Cost(Fidelity::Lossless, 0, 1.0f), "p");
    CHECK_THROWS_AS(FindPlan(g, Request("a/x", "a/y")), std::logic_error);
}

// --- Aggregates the rest of the system depends on ---------------------------

TEST_CASE("requiresDisk is true only when a hop actually spills", "[planner]") {
    // Section 4.3: hops stream into one another. Only the WebView2 PDF hop
    // spills, and a four-hop video conversion must not need 8 GB of scratch.
    const FormatGraph g = test::MakeTestGraph();

    const PlanResult remux = FindPlan(g, Request(ids::kMp4H264, ids::kMkvH264));
    REQUIRE(remux);
    CHECK_FALSE(remux.plan->requiresDisk);

    const PlanResult to_pdf = FindPlan(g, Request(ids::kMd, ids::kPdf));
    REQUIRE(to_pdf);
    CHECK(to_pdf.plan->requiresDisk);
}

TEST_CASE("peak memory is the worst hop, not the sum of hops", "[planner]") {
    // Feeds the job-object budget in section 11.2. Summing would reject jobs
    // that stream perfectly well.
    const FormatGraph g = test::MakeTestGraph();
    const PlanResult result = FindPlan(g, Request(ids::kDocx, ids::kPng));

    REQUIRE(result);
    float worst = 1.0f;
    for (const PlanHop& hop : result.plan->hops) {
        worst = std::max(worst, g.Cost(hop.edge).memPeakFactor);
    }
    CHECK(result.plan->memPeakFactor == worst);
}

TEST_CASE("FindAllTargets agrees with FindPlan for every target", "[planner]") {
    // The shell extension's capability table is built from one sweep rather than
    // one plan per target (section 9.2). The two must not disagree.
    const FormatGraph g = test::MakeTestGraph();
    const auto all = FindAllTargets(g, Request(ids::kDocx, ""));

    CHECK_FALSE(all.empty());
    for (const auto& [node, swept] : all) {
        const PlanResult single = FindPlan(g, Request(ids::kDocx, g.Node(node).Id()));
        REQUIRE(single);
        CHECK(single.plan->cost.worstFidelity == swept.cost.worstFidelity);
        CHECK(single.plan->cost.generationalSum == swept.cost.generationalSum);
        CHECK(single.plan->cost.hopCount == swept.cost.hopCount);
        CHECK(Route(g, *single.plan) == Route(g, swept));
    }
}

TEST_CASE("FindAllTargets never lists the source as a target", "[planner]") {
    const FormatGraph g = test::MakeTestGraph();
    for (const auto& [node, plan] : FindAllTargets(g, Request(ids::kPng, ""))) {
        CHECK(g.Node(node).Id() != std::string(ids::kPng));
        CHECK(plan.cost.hopCount >= 1);
    }
}

// --- The plan cache ----------------------------------------------------------

TEST_CASE("the cache reuses a route and rescales only the estimate", "[planner][cache]") {
    // Section 4.2 caches by (source, target, option hash). The route is
    // size-invariant because every hop's estimate scales by the same factor, so
    // the estimate is the only field that may differ between calls.
    const FormatGraph g = test::MakeTestGraph();
    PlanCache cache(g);

    PlanRequest small = Request(ids::kDocx, ids::kPdf);
    small.inputSizeMiB = 1.0;
    PlanRequest large = small;
    large.inputSizeMiB = 40.0;

    const PlanResult a = cache.Get(small);
    const PlanResult b = cache.Get(large);
    REQUIRE(a);
    REQUIRE(b);

    CHECK(cache.Size() == 1);
    CHECK(Route(g, *a.plan) == Route(g, *b.plan));
    CHECK_THAT(b.plan->cost.estimatedMs,
               Catch::Matchers::WithinRel(a.plan->cost.estimatedMs * 40.0, 1e-9));
}

TEST_CASE("a different option hash is a different cache entry", "[planner][cache]") {
    const FormatGraph g = test::MakeTestGraph();
    PlanCache cache(g);
    const PlanRequest request = Request(ids::kDocx, ids::kPdf);

    CHECK(cache.Get(request, /*optionHash=*/1));
    CHECK(cache.Get(request, /*optionHash=*/2));
    CHECK(cache.Size() == 2);

    cache.Clear();
    CHECK(cache.Size() == 0);
}

TEST_CASE("the cache stores misses too", "[planner][cache]") {
    const FormatGraph g = test::MakeTestGraph();
    PlanCache cache(g);

    CHECK_FALSE(cache.Get(Request(ids::kTxt, ids::kPng)));
    CHECK_FALSE(cache.Get(Request(ids::kTxt, ids::kPng)));
    CHECK(cache.Size() == 1);
}
