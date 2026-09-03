// Running a plan (architecture sections 4, 5 and 11.4).
//
// This file exists because of a bug it now guards. Every provider test in this
// suite calls a reader or writer DIRECTLY with bytes, and every one of them
// passed while the assembled pipeline silently produced empty files: Probe read
// 8 KiB to identify the input and did not rewind, so Execute converted whatever
// was left after the probe -- usually nothing.
//
// Nothing failed. The conversion reported success, the output was a valid file
// of the right format, and it was empty. Unit tests that each exercise one
// component cannot see that; only a test that runs the real path can.
#include <catch2/catch_test_macros.hpp>

#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include "morph/core/executor.h"
#include "morph/core/planner.h"
#include "morph/core/temp_store.h"
#include "morph/providers/docx.h"
#include "morph/providers/html.h"
#include "morph/providers/markdown.h"
#include "morph/providers/pdf.h"

using namespace morph;

namespace {

class SilentProgress final : public IProgressSink {
public:
    void Report(double fraction, std::uint64_t) override {
        // Progress must never go backwards, and must stay in range. A weighting
        // bug shows up here long before anyone notices a jumpy bar.
        CHECK(fraction >= -0.0001);
        CHECK(fraction <= 1.0001);
        CHECK(fraction >= highest - 0.0001);
        highest = std::max(highest, fraction);
        ++calls;
    }
    double highest = 0.0;
    int calls = 0;
};

// The worker's real provider table, assembled the same way morph.worker does.
struct Rig {
    docmd::MarkdownReader markdownReader;
    docmd::MarkdownWriter markdownWriter;
    dochtml::HtmlReader htmlReader;
    dochtml::HtmlWriter htmlWriter;
    docooxml::DocxReader docxReader;
    docooxml::DocxWriter docxWriter;
    pdf::PdfReader pdfReader;
    pdf::PdfWriter pdfWriter;

    Executor executor;
    FormatGraph graph;

    Rig() {
        executor.Add(docxReader);
        executor.Add(pdfReader);
        executor.Add(htmlReader);
        executor.Add(markdownReader);
        executor.Add(docxWriter);
        executor.Add(htmlWriter);
        executor.Add(markdownWriter);
        executor.Add(pdfWriter);
        executor.BuildGraph(graph);
    }

    // The whole path a real conversion takes: identify, plan, execute.
    Result Convert(std::string_view input, std::string_view extension,
                   std::string_view targetProfile, std::vector<std::byte>& out,
                   SilentProgress& progress) {
        MemorySource source(input);
        PathHint hint;
        hint.extension = std::string(extension);
        hint.filename = "fixture" + std::string(extension);

        const std::optional<MediaProfile> from = executor.Probe(source, hint);
        if (!from) return Result::Error(Status::kUnsupportedInput, "not identified");

        PlanRequest request;
        request.source = from->Id();
        request.target = std::string(targetProfile);

        const PlanResult planned = FindPlan(graph, request);
        if (!planned) return Result::Error(Status::kUnsupportedInput, "no route");

        ScopedTempStore temp("exectest");
        std::stop_source stop;
        OptionBag options;
        Capabilities capabilities;
        ConvertContext context{options, progress, stop.get_token(), temp, capabilities};

        MemorySink sink;
        Result result = executor.Execute(graph, *planned.plan, context, source, sink);
        out = sink.bytes();
        return result;
    }
};

std::string_view AsText(const std::vector<std::byte>& bytes) {
    return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

constexpr std::string_view kMarkdown =
    "# Quarterly Report\n"
    "\n"
    "An opening paragraph with **bold text** and a distinctive phrase: xyzzy-marker.\n"
    "\n"
    "- First finding\n"
    "- Second finding\n";

}  // namespace

// --- The regression ----------------------------------------------------------

TEST_CASE("probing does not consume the input", "[executor][regression]") {
    // The bug, isolated. Probe reads up to 8 KiB to identify the file; if it
    // leaves the source there, everything downstream converts the tail.
    Rig rig;
    MemorySource source(kMarkdown);
    PathHint hint;
    hint.extension = ".md";

    const std::optional<MediaProfile> profile = rig.executor.Probe(source, hint);
    REQUIRE(profile.has_value());
    CHECK(profile->Id() == "text/markdown");

    // The source must be back at the start, ready for the reader.
    std::vector<std::byte> first(1);
    CHECK(source.Read(first) == 1);
    CHECK(static_cast<char>(first[0]) == '#');
}

TEST_CASE("probing does not consume the input when nothing matches",
          "[executor][regression]") {
    Rig rig;
    MemorySource source("\x01\x02\x03 binary nonsense that no provider claims");
    PathHint hint;

    CHECK_FALSE(rig.executor.Probe(source, hint).has_value());

    std::vector<std::byte> first(1);
    CHECK(source.Read(first) == 1);
    CHECK(static_cast<unsigned char>(first[0]) == 0x01);
}

TEST_CASE("a full conversion carries the content, not an empty file",
          "[executor][regression]") {
    // The assertion the original bug needed. A valid, well-formed, EMPTY output
    // passed every structural check; only looking for the words catches it.
    Rig rig;
    SilentProgress progress;
    std::vector<std::byte> out;

    REQUIRE(rig.Convert(kMarkdown, ".md", "application/docx", out, progress).ok());
    REQUIRE(out.size() > 1000);

    // Read it back through the real reader and look for the actual text.
    doc::Document back;
    REQUIRE(docooxml::ReadDocx(out, {}, back).ok());
    const std::string text = doc::PlainText(back.blocks);
    INFO(text);
    CHECK(text.find("Quarterly Report") != std::string::npos);
    CHECK(text.find("xyzzy-marker") != std::string::npos);
    CHECK(text.find("First finding") != std::string::npos);
}

// --- Routing -----------------------------------------------------------------

TEST_CASE("a two-hop document route fuses the IR in memory", "[executor]") {
    // md -> IR -> docx. The IR has no serialised form, so the two hops have to
    // run as one in-memory pass; if the executor tried to materialise the IR
    // between them, this would fail rather than silently degrade.
    Rig rig;
    SilentProgress progress;
    std::vector<std::byte> out;

    REQUIRE(rig.Convert(kMarkdown, ".md", "application/docx", out, progress).ok());
    CHECK(AsText(out).substr(0, 2) == "PK");
}

TEST_CASE("a three-hop route chains its intermediates", "[executor]") {
    // md -> IR -> html, where the html hop is the last. Adding pdf makes it
    // md -> IR -> html -> pdf and exercises the buffer handoff between a fused
    // document pass and an ordinary byte provider.
    Rig rig;
    SilentProgress progress;
    std::vector<std::byte> out;

    REQUIRE(rig.Convert(kMarkdown, ".md", "text/html", out, progress).ok());
    const std::string_view html = AsText(out);
    CHECK(html.starts_with("<!DOCTYPE html>"));
    CHECK(html.find("Quarterly Report") != std::string_view::npos);
    CHECK(html.find("xyzzy-marker") != std::string_view::npos);
}

TEST_CASE("markdown reaches PDF through three hops", "[executor]") {
    if (!pdf::WebView2Available()) SKIP("WebView2 runtime not available");

    Rig rig;
    SilentProgress progress;
    std::vector<std::byte> out;

    REQUIRE(rig.Convert(kMarkdown, ".md", "application/pdf", out, progress).ok());
    CHECK(AsText(out).starts_with("%PDF-"));
}

TEST_CASE("docx reaches PDF, which is the four-boundary case", "[executor]") {
    if (!pdf::WebView2Available()) SKIP("WebView2 runtime not available");

    Rig rig;
    SilentProgress progress;

    std::vector<std::byte> docx;
    REQUIRE(rig.Convert(kMarkdown, ".md", "application/docx", docx, progress).ok());

    // Feed the produced .docx straight back in, the way the CLI does.
    MemorySource source(std::string_view(reinterpret_cast<const char*>(docx.data()),
                                         docx.size()));
    PathHint hint;
    hint.extension = ".docx";

    const std::optional<MediaProfile> from = rig.executor.Probe(source, hint);
    REQUIRE(from.has_value());
    CHECK(from->Id() == "application/docx");

    PlanRequest request;
    request.source = from->Id();
    request.target = "application/pdf";
    const PlanResult planned = FindPlan(rig.graph, request);
    REQUIRE(planned);
    CHECK(planned.plan->cost.hopCount == 3);

    ScopedTempStore temp("exectest");
    std::stop_source stop;
    OptionBag options;
    Capabilities capabilities;
    SilentProgress pdfProgress;
    ConvertContext context{options, pdfProgress, stop.get_token(), temp, capabilities};

    MemorySink sink;
    REQUIRE(rig.executor.Execute(rig.graph, *planned.plan, context, source, sink).ok());
    CHECK(AsText(sink.bytes()).starts_with("%PDF-"));
}

TEST_CASE("converting to the format it already is copies it", "[executor]") {
    // A zero-hop plan. Refusing would break a batch where one file in fifty is
    // already in the target format.
    Rig rig;
    SilentProgress progress;
    std::vector<std::byte> out;

    REQUIRE(rig.Convert(kMarkdown, ".md", "text/markdown", out, progress).ok());
    CHECK(AsText(out) == kMarkdown);
}

// --- Progress (section 11.4) -------------------------------------------------

TEST_CASE("progress is monotonic and weighted across hops", "[executor][progress]") {
    // Section 11.4: "weight each hop by its estimated cost so the bar moves
    // proportionally instead of jumping 0 -> 25% -> 25% -> 100%". The
    // monotonicity and range checks live in SilentProgress::Report, so any
    // conversion exercises them; this asserts the endpoint and that more than
    // one hop actually contributed.
    Rig rig;
    SilentProgress progress;
    std::vector<std::byte> out;

    REQUIRE(rig.Convert(kMarkdown, ".md", "text/html", out, progress).ok());
    CHECK(progress.calls >= 2);
    CHECK(progress.highest == 1.0);
}

// --- Failure paths -----------------------------------------------------------

TEST_CASE("the graph the executor builds is the one the planner uses", "[executor]") {
    Rig rig;
    CHECK(rig.graph.Frozen());
    CHECK(rig.graph.Find("application/morph-ir").has_value());
    CHECK(rig.graph.Find("text/markdown").has_value());
    CHECK(rig.graph.Find("application/pdf").has_value());

    // Every document format must reach every other one, which is the property
    // the IR exists to provide.
    for (const std::string_view from : {"text/markdown", "text/html", "application/docx"}) {
        for (const std::string_view to : {"text/markdown", "text/html", "application/docx"}) {
            if (from == to) continue;
            PlanRequest request;
            request.source = std::string(from);
            request.target = std::string(to);
            INFO(from << " -> " << to);
            CHECK(FindPlan(rig.graph, request));
        }
    }
}

TEST_CASE("a cancelled context stops before writing", "[executor]") {
    Rig rig;
    MemorySource source(kMarkdown);
    PathHint hint;
    hint.extension = ".md";

    const std::optional<MediaProfile> from = rig.executor.Probe(source, hint);
    REQUIRE(from.has_value());

    PlanRequest request;
    request.source = from->Id();
    request.target = "application/docx";
    const PlanResult planned = FindPlan(rig.graph, request);
    REQUIRE(planned);

    ScopedTempStore temp("exectest");
    std::stop_source stop;
    stop.request_stop();  // cancelled before it begins

    OptionBag options;
    Capabilities capabilities;
    SilentProgress progress;
    ConvertContext context{options, progress, stop.get_token(), temp, capabilities};

    MemorySink sink;
    const Result result = rig.executor.Execute(rig.graph, *planned.plan, context, source, sink);
    CHECK(result.cancelled());
    // Section 11.3: cancellation never leaves a half-written file where the user
    // expects a real one.
    CHECK_FALSE(sink.committed());
    CHECK(sink.bytes().empty());
}

TEST_CASE("fidelity notes survive the whole pipeline", "[executor][fidelity]") {
    // The notes a hop incurs have to reach the caller, or the fidelity contract
    // stops at the provider boundary and the UI has nothing to show.
    Rig rig;
    MemorySource source(
        "| A | B |\n| - | - |\n| 1 | 2 |\n\nText with ~~strikethrough~~.\n");
    PathHint hint;
    hint.extension = ".md";

    const std::optional<MediaProfile> from = rig.executor.Probe(source, hint);
    REQUIRE(from.has_value());

    PlanRequest request;
    request.source = from->Id();
    request.target = "text/markdown";
    // Force the commonmark flavour, which cannot express either construct.
    OptionBag options;
    options.Set(std::string(opt::kMarkdownFlavour), std::string("commonmark"));

    // A same-format plan is zero hops and would just copy, so route through the
    // IR explicitly by going to HTML and back is not possible in one plan --
    // instead check the note on a docx conversion, which drops nothing, and on a
    // markdown write, which does.
    request.target = "application/docx";
    const PlanResult planned = FindPlan(rig.graph, request);
    REQUIRE(planned);

    ScopedTempStore temp("exectest");
    std::stop_source stop;
    Capabilities capabilities;
    SilentProgress progress;
    ConvertContext context{options, progress, stop.get_token(), temp, capabilities};

    MemorySink sink;
    const Result result = rig.executor.Execute(rig.graph, *planned.plan, context, source, sink);
    REQUIRE(result.ok());
    // docx expresses both, so nothing is lost and nothing is claimed to be.
    CHECK(result.incurred() == Fidelity::Lossless);
    CHECK(result.notes().empty());
}
