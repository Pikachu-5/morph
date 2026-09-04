// The broker-to-worker channel's codec, and the graph rebuild it feeds
// (architecture sections 3.1 and 10.2).
//
// The rebuild is the part worth testing rather than the encoding: the broker
// plans against a graph it never built from providers, and a description that
// loses an edge cost would produce routes that are subtly worse than the ones
// the worker would have chosen, with nothing failing anywhere.

#include <catch2/catch_test_macros.hpp>

#include "morph/core/graph.h"
#include "morph/core/ipc/worker_protocol.h"
#include "morph/core/planner.h"

using namespace morph;
using namespace morph::ipc;

namespace {

EdgeWire Edge(std::string from, std::string to, Fidelity fidelity, double cpuPerMiB) {
    EdgeWire wire;
    wire.from = std::move(from);
    wire.to = std::move(to);
    wire.provider = "test";
    wire.fidelity = static_cast<std::uint8_t>(fidelity);
    wire.cpuPerMiB = cpuPerMiB;
    wire.memPeakFactor = 2.5;
    return wire;
}

template <typename T>
T RoundTrip(const T& message) {
    T back;
    REQUIRE(Decode(Encode(message), back));
    return back;
}

}  // namespace

TEST_CASE("A worker hello survives the wire", "[ipc][worker]") {
    WorkerHello hello;
    hello.workerVersion = "0.1.0";
    hello.processId = 4321;

    const WorkerHello back = RoundTrip(hello);
    CHECK(back.protocolVersion == kProtocolVersion);
    CHECK(back.workerVersion == "0.1.0");
    CHECK(back.processId == 4321u);
}

TEST_CASE("A graph description survives the wire", "[ipc][worker]") {
    GraphDescription description;
    description.capabilities = {"webview2"};
    description.edges.push_back(Edge("text/markdown", "text/html", Fidelity::High, 3.5));
    description.edges.push_back(Edge("text/html", "application/pdf", Fidelity::Structural, 40.0));

    const GraphDescription back = RoundTrip(description);
    REQUIRE(back.edges.size() == 2);
    CHECK(back.capabilities == std::vector<std::string>{"webview2"});
    CHECK(back.edges[0].from == "text/markdown");
    CHECK(back.edges[1].fidelity == static_cast<std::uint8_t>(Fidelity::Structural));
    CHECK(back.edges[1].cpuPerMiB == 40.0);
    CHECK(back.edges[0].memPeakFactor == 2.5);
}

TEST_CASE("An edge missing an endpoint is dropped, not interned", "[ipc][worker]") {
    GraphDescription description;
    description.edges.push_back(Edge("", "text/html", Fidelity::High, 1.0));
    description.edges.push_back(Edge("text/markdown", "", Fidelity::High, 1.0));
    description.edges.push_back(Edge("text/markdown", "text/html", Fidelity::High, 1.0));

    const GraphDescription back = RoundTrip(description);
    REQUIRE(back.edges.size() == 1);
    CHECK(back.edges[0].from == "text/markdown");
}

TEST_CASE("A described graph plans the same routes as the original", "[ipc][worker]") {
    // The broker's copy is built from the description alone. If it plans
    // differently from the worker's, every conversion goes the wrong way round.
    FormatGraph original;
    const MediaProfile md = *MediaProfile::Parse("text/markdown");
    const MediaProfile html = *MediaProfile::Parse("text/html");
    const MediaProfile pdf = *MediaProfile::Parse("application/pdf");

    EdgeCost cheap;
    cheap.fidelity = Fidelity::High;
    cheap.cpuPerMiB = 3.5f;
    cheap.memPeakFactor = 2.5f;

    EdgeCost lossy;
    lossy.fidelity = Fidelity::Structural;
    lossy.cpuPerMiB = 40.0f;
    lossy.memPeakFactor = 4.0f;

    (void)original.AddEdge(md, html, cheap, "test");
    (void)original.AddEdge(html, pdf, lossy, "test");
    original.Freeze();

    GraphDescription description;
    description.edges.push_back(Edge("text/markdown", "text/html", Fidelity::High, 3.5));
    description.edges.back().memPeakFactor = 2.5;
    description.edges.push_back(Edge("text/html", "application/pdf", Fidelity::Structural, 40.0));
    description.edges.back().memPeakFactor = 4.0;

    FormatGraph rebuilt;
    for (const EdgeWire& wire : RoundTrip(description).edges) {
        EdgeCost cost;
        cost.fidelity = static_cast<Fidelity>(wire.fidelity);
        cost.generational = wire.generational;
        cost.cpuPerMiB = static_cast<float>(wire.cpuPerMiB);
        cost.memPeakFactor = static_cast<float>(wire.memPeakFactor);
        cost.requiresDisk = wire.requiresDisk;
        (void)rebuilt.AddEdge(*MediaProfile::Parse(wire.from), *MediaProfile::Parse(wire.to),
                              cost, wire.provider);
    }
    rebuilt.Freeze();

    PlanRequest request;
    request.source = "text/markdown";
    request.target = "application/pdf";
    request.inputSizeMiB = 4.0;

    const PlanResult expected = FindPlan(original, request);
    const PlanResult actual = FindPlan(rebuilt, request);
    REQUIRE(expected);
    REQUIRE(actual);
    CHECK(actual.plan->cost.hopCount == expected.plan->cost.hopCount);
    CHECK(actual.plan->cost.worstFidelity == expected.plan->cost.worstFidelity);
    CHECK(actual.plan->cost.estimatedMs == expected.plan->cost.estimatedMs);
    CHECK(actual.plan->memPeakFactor == expected.plan->memPeakFactor);
}

TEST_CASE("A run task carries its options with their kinds intact", "[ipc][worker]") {
    RunTask task;
    task.jobId = 9;
    task.taskIndex = 3;
    task.inputPath = "C:\\a\\report.docx";
    task.outputPath = "C:\\a\\report.pdf";
    task.targetProfile = "application/pdf";

    OptionValue flag;
    flag.key = "embedFonts";
    flag.kind = 0;
    flag.boolValue = true;
    OptionValue quality;
    quality.key = "quality";
    quality.kind = 1;
    quality.intValue = 90;
    OptionValue scale;
    scale.key = "scale";
    scale.kind = 2;
    scale.doubleValue = 1.5;
    OptionValue title;
    title.key = "title";
    title.kind = 3;
    title.stringValue = "Q3";
    task.options = {flag, quality, scale, title};

    const RunTask back = RoundTrip(task);
    REQUIRE(back.options.size() == 4);
    CHECK(back.jobId == 9u);
    CHECK(back.taskIndex == 3u);
    CHECK(back.inputPath == task.inputPath);
    CHECK(back.options[0].boolValue);
    CHECK(back.options[1].intValue == 90);
    CHECK(back.options[2].doubleValue == 1.5);
    CHECK(back.options[3].stringValue == "Q3");
}

TEST_CASE("A probe answer is one entry per question", "[ipc][worker]") {
    ProbeFiles request;
    request.paths = {"a.md", "b.bin", "c.docx"};

    ProbeResult result;
    // The middle file could not be identified. That has to survive as an empty
    // string rather than a shorter list, or the broker matches answers to the
    // wrong files.
    result.profiles = {"text/markdown", "", "application/docx"};

    const ProbeResult back = RoundTrip(result);
    REQUIRE(back.profiles.size() == request.paths.size());
    CHECK(back.profiles[1].empty());
    CHECK(back.profiles[2] == "application/docx");
}
