#include <catch2/catch_test_macros.hpp>

#include "graph_fixture.h"
#include "morph/core/graph.h"

using namespace morph;
using morph::test::Cost;
using morph::test::P;

TEST_CASE("adding an edge interns both endpoints", "[graph]") {
    FormatGraph g;
    g.AddEdge(P("image/png"), P("image/jpeg"), Cost(Fidelity::High, 1, 6.0f), "wic");

    CHECK(g.NodeCount() == 2);
    CHECK(g.EdgeCount() == 1);
    CHECK(g.Find("image/png").has_value());
    CHECK(g.Find("image/jpeg").has_value());
    CHECK_FALSE(g.Find("image/webp").has_value());
}

TEST_CASE("a node is added once no matter how it is spelled", "[graph]") {
    FormatGraph g;
    const NodeId a = g.AddNode(P("Video/MP4;Codec=H264"));
    const NodeId b = g.AddNode(P("video/mp4 ; codec = h264"));

    CHECK(a == b);
    CHECK(g.NodeCount() == 1);
}

TEST_CASE("the registering provider is stamped onto the edge", "[graph]") {
    FormatGraph g;
    const EdgeId e = g.AddEdge(P("image/heif"), P("image/webp"),
                               Cost(Fidelity::High, 1, 8.0f), "wic");
    CHECK(g.Cost(e).provider == "wic");
}

TEST_CASE("parallel edges between the same pair are allowed", "[graph]") {
    // WIC and FFmpeg may both offer the same conversion at different costs; the
    // planner picks per path rather than the graph picking at registration.
    FormatGraph g;
    g.AddEdge(P("image/png"), P("image/jpeg"), Cost(Fidelity::High, 1, 6.0f), "wic");
    g.AddEdge(P("image/png"), P("image/jpeg"), Cost(Fidelity::High, 1, 4.0f), "ffmpeg");

    CHECK(g.EdgeCount() == 2);
    CHECK(g.Outgoing(*g.Find("image/png")).size() == 2);
}

TEST_CASE("a self-edge is rejected at registration", "[graph]") {
    FormatGraph g;
    CHECK_THROWS_AS(
        g.AddEdge(P("image/png"), P("image/png"), Cost(Fidelity::Lossless, 0, 1.0f), "wic"),
        std::invalid_argument);
}

TEST_CASE("an edge with no provider is rejected", "[graph]") {
    FormatGraph g;
    CHECK_THROWS_AS(
        g.AddEdge(P("image/png"), P("image/jpeg"), Cost(Fidelity::High, 1, 6.0f), ""),
        std::invalid_argument);
}

TEST_CASE("an empty profile is rejected", "[graph]") {
    FormatGraph g;
    CHECK_THROWS_AS(g.AddNode(MediaProfile{}), std::invalid_argument);
}

TEST_CASE("the graph is immutable after Freeze", "[graph]") {
    FormatGraph g;
    g.AddEdge(P("image/png"), P("image/jpeg"), Cost(Fidelity::High, 1, 6.0f), "wic");
    g.Freeze();

    CHECK(g.Frozen());
    CHECK_THROWS_AS(g.AddNode(P("image/webp")), std::logic_error);
    CHECK_THROWS_AS(
        g.AddEdge(P("image/png"), P("image/webp"), Cost(Fidelity::Lossless, 0, 5.0f), "wic"),
        std::logic_error);
    // Interning a profile that is already present is not a mutation.
    CHECK_NOTHROW(g.AddNode(P("image/png")));
}

TEST_CASE("Freeze produces a registration-order-independent adjacency", "[graph]") {
    // Section 16 asserts the planner is deterministic for a fixed graph, and the
    // planner's last tie-break falls through to adjacency order. Two graphs with
    // the same edges registered in a different order must iterate identically.
    const auto build = [](bool reversed) {
        FormatGraph g;
        if (reversed) {
            g.AddEdge(P("image/png"), P("image/webp"), Cost(Fidelity::Lossless, 0, 14.0f), "wic");
            g.AddEdge(P("image/png"), P("image/jpeg"), Cost(Fidelity::High, 1, 6.0f), "wic");
        } else {
            g.AddEdge(P("image/png"), P("image/jpeg"), Cost(Fidelity::High, 1, 6.0f), "wic");
            g.AddEdge(P("image/png"), P("image/webp"), Cost(Fidelity::Lossless, 0, 14.0f), "wic");
        }
        g.Freeze();
        return g;
    };

    const FormatGraph forward = build(false);
    const FormatGraph reverse = build(true);

    const auto targets = [](const FormatGraph& g) {
        std::vector<std::string> out;
        for (const EdgeId e : g.Outgoing(*g.Find("image/png"))) out.push_back(g.Node(g.To(e)).Id());
        return out;
    };
    CHECK(targets(forward) == targets(reverse));
}

TEST_CASE("an unavailable codec removes its edges but keeps its nodes", "[graph]") {
    // Section 6.2: HEIF and AVIF depend on Store-delivered codec packages. When
    // the startup probe says they are missing we mark the edges unavailable, but
    // the UI still needs the node so it can say "this needs a codec from the
    // Store" instead of pretending the format does not exist.
    FormatGraph g = test::MakeTestGraph();
    const std::size_t nodes_before = g.NodeCount();
    const std::size_t edges_before = g.EdgeCount();

    g.RemoveProviderEdges({"wic"});

    CHECK(g.NodeCount() == nodes_before);
    CHECK(g.EdgeCount() < edges_before);
    CHECK(g.Find(test::ids::kHeif).has_value());
    CHECK(g.Outgoing(*g.Find(test::ids::kHeif)).empty());
    CHECK(g.Frozen());

    // Surviving edges are still internally consistent after the compaction.
    for (const EdgeId e : g.Outgoing(*g.Find(test::ids::kMp4H264))) {
        CHECK(g.From(e) == *g.Find(test::ids::kMp4H264));
        CHECK(g.Cost(e).provider != "wic");
    }
}

TEST_CASE("removing nothing is a no-op", "[graph]") {
    FormatGraph g = test::MakeTestGraph();
    const std::size_t edges = g.EdgeCount();
    g.RemoveProviderEdges({});
    CHECK(g.EdgeCount() == edges);
}

TEST_CASE("out-of-range ids throw rather than reading past the end", "[graph]") {
    const FormatGraph g = test::MakeTestGraph();
    CHECK_THROWS_AS(g.Node(static_cast<NodeId>(9999)), std::out_of_range);
    CHECK_THROWS_AS(g.Cost(static_cast<EdgeId>(9999)), std::out_of_range);
    CHECK_THROWS_AS(g.To(static_cast<EdgeId>(9999)), std::out_of_range);
}
