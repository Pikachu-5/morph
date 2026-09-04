// Lane sizing and task classification (architecture section 11.1).

#include <catch2/catch_test_macros.hpp>

#include "broker/job.h"
#include "broker/pool.h"
#include "morph/core/graph.h"
#include "morph/core/planner.h"

using namespace morph;
using namespace morph::broker;

namespace {

FormatGraph DocumentGraph() {
    FormatGraph graph;
    EdgeCost cost;
    cost.fidelity = Fidelity::High;
    cost.cpuPerMiB = 3.0f;

    (void)graph.AddEdge(*MediaProfile::Parse("text/markdown"),
                        *MediaProfile::Parse("text/html"), cost, "docmd");
    (void)graph.AddEdge(*MediaProfile::Parse("text/html"),
                        *MediaProfile::Parse("application/pdf"), cost, "pdf");
    (void)graph.AddEdge(*MediaProfile::Parse("text/html"),
                        *MediaProfile::Parse("image/png"), cost, "wic");
    graph.Freeze();
    return graph;
}

Plan PlanTo(const FormatGraph& graph, const char* target) {
    PlanRequest request;
    request.source = "text/markdown";
    request.target = target;
    const PlanResult planned = FindPlan(graph, request);
    REQUIRE(planned);
    return *planned.plan;
}

}  // namespace

TEST_CASE("Every lane has a limit, and the WebView2 lane has exactly one",
          "[broker][scheduler]") {
    const LaneSizes sizes = SizeLanes();

    // Section 11.1: "PDF render (WebView2): 1. One WebView2 environment,
    // reused. Serialise through it."
    CHECK(sizes[TaskClass::kPdfRender] == 1u);

    // "Video / audio: 1-2."
    CHECK(sizes[TaskClass::kAudioVideo] >= 1u);
    CHECK(sizes[TaskClass::kAudioVideo] <= 2u);

    CHECK(sizes[TaskClass::kDocument] >= 1u);
    CHECK(sizes[TaskClass::kImage] >= 1u);

    // Images run wider than documents on any machine with more than one core,
    // which is the whole point of having separate lanes.
    CHECK(sizes[TaskClass::kImage] >= sizes[TaskClass::kDocument]);
}

TEST_CASE("A route is classified by its most constrained hop", "[broker][scheduler]") {
    const FormatGraph graph = DocumentGraph();

    CHECK(ClassOf(graph, PlanTo(graph, "text/html")) == TaskClass::kDocument);
    // Rendering to PDF is a PDF render even though the route is mostly document
    // work: the WebView2 environment is the binding constraint.
    CHECK(ClassOf(graph, PlanTo(graph, "application/pdf")) == TaskClass::kPdfRender);
    CHECK(ClassOf(graph, PlanTo(graph, "image/png")) == TaskClass::kImage);
}

TEST_CASE("A job counts its tasks and reports the last one", "[broker][scheduler]") {
    Job job;
    job.id = 1;
    job.tasks.resize(3);

    CHECK_FALSE(job.Finish(Status::kOk));
    CHECK_FALSE(job.Finish(Status::kMalformedInput));
    CHECK(job.Finish(Status::kCancelled));
    CHECK(job.Complete());

    const ipc::JobDone summary = job.Summary();
    CHECK(summary.succeeded == 1u);
    CHECK(summary.failed == 1u);
    CHECK(summary.cancelled == 1u);
}
