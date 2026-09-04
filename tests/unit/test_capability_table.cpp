// The capability table (architecture section 9.2).
//
// Two things are being tested here and they are different in kind. The first is
// that a published table says what the graph says. The second is that a
// MALFORMED table cannot make the reader do anything at all -- the mapping is
// writable by the user whose Explorer will read it, so validation is not a
// formality.

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

#include "broker/capabilities.h"
#include "morph/core/caps.h"
#include "morph/core/graph.h"
#include "morph/core/planner.h"

using namespace morph;

namespace {

CapabilitySet TwoSources() {
    CapabilitySet set;

    CapabilitySet::Source docx;
    docx.extension = "docx";
    docx.targets.push_back({"application/pdf", "pdf", static_cast<std::uint8_t>(Fidelity::High),
                            2, false});
    docx.targets.push_back({"text/markdown", "md",
                            static_cast<std::uint8_t>(Fidelity::Structural), 2, false});

    CapabilitySet::Source md;
    md.extension = "md";
    md.targets.push_back({"application/pdf", "pdf", static_cast<std::uint8_t>(Fidelity::High), 3,
                          true});

    // Deliberately out of order: the serialiser sorts, because the reader binary
    // searches.
    set.sources.push_back(std::move(md));
    set.sources.push_back(std::move(docx));
    return set;
}

FormatGraph DocumentGraph() {
    FormatGraph graph;

    EdgeCost lossless;
    lossless.fidelity = Fidelity::Lossless;
    lossless.cpuPerMiB = 2.0f;

    EdgeCost high;
    high.fidelity = Fidelity::High;
    high.cpuPerMiB = 20.0f;

    const MediaProfile md = *MediaProfile::Parse("text/markdown");
    const MediaProfile ir = *MediaProfile::Parse("application/morph-ir");
    const MediaProfile html = *MediaProfile::Parse("text/html");
    const MediaProfile docx = *MediaProfile::Parse("application/docx");
    const MediaProfile pdf = *MediaProfile::Parse("application/pdf");

    (void)graph.AddEdge(md, ir, lossless, "docmd");
    (void)graph.AddEdge(ir, md, lossless, "docmd");
    (void)graph.AddEdge(docx, ir, lossless, "docooxml");
    (void)graph.AddEdge(ir, docx, lossless, "docooxml");
    (void)graph.AddEdge(ir, html, lossless, "dochtml");
    (void)graph.AddEdge(html, pdf, high, "pdf");
    graph.Freeze();
    return graph;
}

}  // namespace

TEST_CASE("A published table answers what went into it", "[caps]") {
    const std::vector<std::byte> bytes = SerialiseCapabilities(TwoSources(), 7);
    REQUIRE_FALSE(bytes.empty());

    const CapabilityTable table = CapabilityTable::Attach(bytes.data(), bytes.size());
    REQUIRE(table.Valid());
    CHECK(table.Generation() == 7u);
    CHECK(table.Extensions().size() == 2u);

    const std::span<const CapsTarget> docx = table.TargetsFor("docx");
    REQUIRE(docx.size() == 2u);
    CHECK(table.String(docx[0].profileOff) == "application/pdf");
    CHECK(table.String(docx[0].extensionOff) == "pdf");
    CHECK(docx[0].hopCount == 2u);

    const std::span<const CapsTarget> md = table.TargetsFor("md");
    REQUIRE(md.size() == 1u);
    CHECK(md[0].requiresDisk == 1u);
}

TEST_CASE("A lookup takes the extension as the user's file spells it", "[caps]") {
    const std::vector<std::byte> bytes = SerialiseCapabilities(TwoSources(), 1);
    const CapabilityTable table = CapabilityTable::Attach(bytes.data(), bytes.size());

    // Explorer hands over ".DOCX" as readily as "docx".
    CHECK(table.TargetsFor(".docx").size() == 2u);
    CHECK(table.TargetsFor("DOCX").size() == 2u);
    CHECK(table.TargetsFor(".DoCx").size() == 2u);

    CHECK(table.TargetsFor("xlsx").empty());
    CHECK(table.TargetsFor("").empty());
    // Longer than any real extension: the lookup normalises into a fixed buffer
    // because it runs with no allocation, and the bound has to hold.
    CHECK(table.TargetsFor(std::string(400, 'z')).empty());
}

TEST_CASE("A source with nothing to offer is left out of the menu", "[caps]") {
    CapabilitySet set;
    set.sources.push_back({"txt", {}});
    set.sources.push_back({"", {}});

    const std::vector<std::byte> bytes = SerialiseCapabilities(set, 2);
    const CapabilityTable table = CapabilityTable::Attach(bytes.data(), bytes.size());
    REQUIRE(table.Valid());
    CHECK(table.Extensions().empty());
    CHECK(table.TargetsFor("txt").empty());
}

TEST_CASE("Nothing published reads as no table, not as an empty one", "[caps]") {
    // A freshly created mapping is zeroes, and zeroes must not look like a
    // table with no formats in it -- the shell extension shows a static entry
    // for the first and a broken menu for the second.
    const std::vector<std::byte> zeroes(kCapsCapacityBytes / 64, std::byte{0});
    CHECK_FALSE(CapabilityTable::Attach(zeroes.data(), zeroes.size()).Valid());

    // Mid-publish: the body is there but the generation has not landed yet.
    std::vector<std::byte> bytes = SerialiseCapabilities(TwoSources(), 5);
    CapsHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    header.generation = 0;
    std::memcpy(bytes.data(), &header, sizeof(header));
    CHECK_FALSE(CapabilityTable::Attach(bytes.data(), bytes.size()).Valid());
}

TEST_CASE("A table the reader does not understand is refused", "[caps]") {
    std::vector<std::byte> bytes = SerialiseCapabilities(TwoSources(), 3);

    auto with = [&bytes](auto mutate) {
        std::vector<std::byte> copy = bytes;
        CapsHeader header{};
        std::memcpy(&header, copy.data(), sizeof(header));
        mutate(header);
        std::memcpy(copy.data(), &header, sizeof(header));
        return CapabilityTable::Attach(copy.data(), copy.size()).Valid();
    };

    CHECK_FALSE(with([](CapsHeader& h) { h.magic = 0; }));
    // Section 9.2: "bump = shellext ignores the map and shows a static menu".
    CHECK_FALSE(with([](CapsHeader& h) { h.layoutVersion = kCapsLayoutVersion + 1; }));

    // Every one of these would be a read past the end of the mapping if the
    // reader believed the header.
    CHECK_FALSE(with([](CapsHeader& h) { h.totalBytes = 0xFFFFFFFFu; }));
    CHECK_FALSE(with([](CapsHeader& h) { h.extCount = 0xFFFFFFFFu; }));
    CHECK_FALSE(with([](CapsHeader& h) { h.targetCount = 0xFFFFFFFFu; }));
    CHECK_FALSE(with([](CapsHeader& h) { h.stringPoolOff = h.totalBytes; }));
    CHECK_FALSE(with([](CapsHeader& h) { h.stringPoolBytes = 0xFFFFFFFFu; }));
    CHECK_FALSE(with([](CapsHeader& h) { h.extIndexOff = 0; }));
}

TEST_CASE("A truncated table is refused rather than read", "[caps]") {
    const std::vector<std::byte> bytes = SerialiseCapabilities(TwoSources(), 4);
    for (const std::size_t size : {std::size_t{0}, sizeof(CapsHeader) - 1, bytes.size() / 2}) {
        CHECK_FALSE(CapabilityTable::Attach(bytes.data(), size).Valid());
    }
}

TEST_CASE("An index entry pointing outside the string pool is refused", "[caps]") {
    std::vector<std::byte> bytes = SerialiseCapabilities(TwoSources(), 6);
    CapsHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));

    CapsExtension entry{};
    std::memcpy(&entry, bytes.data() + header.extIndexOff, sizeof(entry));
    entry.nameOff = header.stringPoolBytes;  // one past the end
    std::memcpy(bytes.data() + header.extIndexOff, &entry, sizeof(entry));

    CHECK_FALSE(CapabilityTable::Attach(bytes.data(), bytes.size()).Valid());
}

TEST_CASE("The menu is derived from the graph, best route first", "[caps][broker]") {
    const FormatGraph graph = DocumentGraph();
    const CapabilitySet set = broker::CapabilitiesOf(graph);

    const auto docx = std::find_if(set.sources.begin(), set.sources.end(),
                                   [](const CapabilitySet::Source& source) {
                                       return source.extension == "docx";
                                   });
    REQUIRE(docx != set.sources.end());
    REQUIRE_FALSE(docx->targets.empty());

    // The IR is reachable from everything and is not a file. Offering it would
    // put "convert to application/morph-ir" in Explorer.
    for (const CapabilitySet::Target& target : docx->targets) {
        CHECK(target.profile != "application/morph-ir");
        CHECK(target.profile != "application/docx");  // nor itself
    }

    // Lossless before lossy, so the menu reads in the order the fidelity
    // contract would rank it.
    CHECK(docx->targets.front().fidelity <= docx->targets.back().fidelity);
    CHECK(docx->targets.front().fidelity == static_cast<std::uint8_t>(Fidelity::Lossless));

    const auto pdf = std::find_if(docx->targets.begin(), docx->targets.end(),
                                  [](const CapabilitySet::Target& target) {
                                      return target.profile == "application/pdf";
                                  });
    REQUIRE(pdf != docx->targets.end());
    CHECK(pdf->extension == "pdf");
    CHECK(pdf->fidelity == static_cast<std::uint8_t>(Fidelity::High));
}

TEST_CASE("The published table and the graph agree", "[caps][broker]") {
    // The whole point of the table is that the menu built without asking anyone
    // matches the answer the planner would have given.
    const FormatGraph graph = DocumentGraph();
    const std::vector<std::byte> bytes = SerialiseCapabilities(broker::CapabilitiesOf(graph), 1);
    const CapabilityTable table = CapabilityTable::Attach(bytes.data(), bytes.size());
    REQUIRE(table.Valid());

    const std::span<const CapsTarget> targets = table.TargetsFor("docx");
    REQUIRE_FALSE(targets.empty());

    for (const CapsTarget& target : targets) {
        PlanRequest request;
        request.source = "application/docx";
        request.target = std::string(table.String(target.profileOff));

        const PlanResult planned = FindPlan(graph, request);
        INFO(request.target);
        REQUIRE(planned);
        CHECK(static_cast<std::uint8_t>(planned.plan->fidelity()) == target.fidelity);
        CHECK(planned.plan->cost.hopCount == target.hopCount);
    }
}
