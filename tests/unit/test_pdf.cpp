// PDF output through WebView2 (architecture section 8.6).
//
// These tests drive the real runtime. They cannot be faked usefully: the whole
// point of the design is that Chromium's print pipeline does the typesetting, so
// a mock would test nothing but our own plumbing.
//
// Section 20 lists "WebView2 dependency in constrained environments" as a real
// risk whose mitigation is to detect at startup and mark PDF edges unavailable
// rather than crashing. So when the runtime is absent these tests assert exactly
// that behaviour instead of failing.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stop_token>
#include <string>
#include <vector>

#include "morph/core/temp_store.h"
#include "morph/providers/docx.h"
#include "morph/providers/html.h"
#include "morph/providers/markdown.h"
#include "morph/providers/pdf.h"

using namespace morph;
using namespace morph::doc;

namespace {

std::string_view AsText(const std::vector<std::byte>& bytes) {
    return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// Renders HTML and returns the PDF bytes, or an empty vector when the runtime is
// unavailable (the caller then skips).
Result RenderToPdf(std::string_view html, MemorySink& sink, const OptionBag& options = {}) {
    ScopedTempStore temp("test");
    std::stop_source stop;
    return pdf::RenderHtmlToPdf(html, options, temp, sink, stop.get_token());
}

std::string HtmlFromMarkdown(std::string_view markdown) {
    Document document;
    REQUIRE(docmd::ReadMarkdown(markdown, {}, document).ok());
    std::string html;
    REQUIRE(dochtml::WriteHtml(document, {}, html).ok());
    return html;
}

// A PDF's page count, read from /Count on the page-tree root.
//
// Counting "/Type /Page" occurrences does not work: Chromium writes the key
// followed by a newline rather than a space, and "/Type /Pages" on the tree root
// matches the same prefix. /Count is the authoritative number, and taking the
// maximum handles a nested tree whose intermediate nodes carry partial counts.
//
// This is a test helper, not a parser. Once PDFium lands (section 8.7) the page
// count comes from FPDF_GetPageCount and this goes away.
std::size_t CountPages(std::string_view pdf) {
    std::size_t highest = 0;
    constexpr std::string_view kKey = "/Count";

    for (std::size_t at = 0; (at = pdf.find(kKey, at)) != std::string_view::npos; ++at) {
        std::size_t digit = at + kKey.size();
        while (digit < pdf.size() && (pdf[digit] == ' ' || pdf[digit] == '\n' ||
                                      pdf[digit] == '\r' || pdf[digit] == '\t')) {
            ++digit;
        }
        std::size_t value = 0;
        bool any = false;
        while (digit < pdf.size() && pdf[digit] >= '0' && pdf[digit] <= '9') {
            value = value * 10 + static_cast<std::size_t>(pdf[digit] - '0');
            ++digit;
            any = true;
        }
        if (any) highest = std::max(highest, value);
    }
    return highest;
}

}  // namespace

TEST_CASE("the WebView2 runtime is detected", "[pdf][capability]") {
    // Not an assertion about the machine: either answer is valid, and what
    // matters is that we can tell which, because the graph drops the PDF edges
    // when the answer is no rather than failing mid-job (section 6.2).
    const bool available = pdf::WebView2Available();
    const std::string version = pdf::WebView2Version();

    INFO("WebView2 available: " << available << ", version: '" << version << "'");
    if (available) {
        CHECK_FALSE(version.empty());
    } else {
        WARN("No WebView2 runtime on this machine; PDF rendering tests will be skipped.");
    }
    SUCCEED();
}

TEST_CASE("HTML renders to a real PDF", "[pdf][render]") {
    if (!pdf::WebView2Available()) {
        SKIP("WebView2 runtime not available");
    }

    MemorySink sink;
    const Result result = RenderToPdf(
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>T</title></head>"
        "<body><h1>Hello</h1><p>A paragraph.</p></body></html>",
        sink);

    INFO("renderer said: " << result.message());
    REQUIRE(result.ok());
    CHECK(sink.committed());

    const std::string_view bytes = AsText(sink.bytes());
    REQUIRE(bytes.size() > 500);
    CHECK(bytes.starts_with("%PDF-"));
    // Every PDF ends with the end-of-file marker; its absence means we truncated
    // the stream while draining it.
    CHECK(bytes.find("%%EOF") != std::string_view::npos);
}

TEST_CASE("Markdown reaches PDF through the IR and the HTML writer",
          "[pdf][render][roundtrip]") {
    // Section 8.6: "This one component means md -> pdf, html -> pdf and
    // docx -> pdf all fall out of the graph for free, because every one of them
    // routes through the HTML writer." Nobody wrote a Markdown-to-PDF converter.
    if (!pdf::WebView2Available()) {
        SKIP("WebView2 runtime not available");
    }

    MemorySink sink;
    const Result result = RenderToPdf(
        HtmlFromMarkdown("# Report\n\nText with **bold**.\n\n- one\n- two\n"), sink);

    INFO("renderer said: " << result.message());
    REQUIRE(result.ok());
    CHECK(AsText(sink.bytes()).starts_with("%PDF-"));
}

TEST_CASE("DOCX reaches PDF through the same route", "[pdf][render][roundtrip]") {
    if (!pdf::WebView2Available()) {
        SKIP("WebView2 runtime not available");
    }

    // md -> IR -> docx, then docx -> IR -> html -> pdf. Four format boundaries,
    // no converter written for any of the pairs.
    Document source;
    REQUIRE(docmd::ReadMarkdown("# Title\n\nBody text.\n", {}, source).ok());

    std::vector<std::byte> docx;
    REQUIRE(docooxml::WriteDocx(source, {}, docx).ok());

    Document reread;
    REQUIRE(docooxml::ReadDocx(docx, {}, reread).ok());

    std::string html;
    REQUIRE(dochtml::WriteHtml(reread, {}, html).ok());

    MemorySink sink;
    const Result result = RenderToPdf(html, sink);
    INFO("renderer said: " << result.message());
    REQUIRE(result.ok());
    CHECK(AsText(sink.bytes()).starts_with("%PDF-"));
}

TEST_CASE("page size is honoured", "[pdf][render]") {
    if (!pdf::WebView2Available()) {
        SKIP("WebView2 runtime not available");
    }

    // A5 is less than half the height of A4, so the same content paginates
    // differently. Comparing page counts is a property that holds regardless of
    // how Chromium lays the text out.
    const std::string html = HtmlFromMarkdown(
        std::string("# Long\n\n") + [] {
            std::string body;
            for (int i = 0; i < 60; ++i) {
                body += "Paragraph " + std::to_string(i) +
                        " with enough text on it to occupy a visible amount of vertical "
                        "space on the page.\n\n";
            }
            return body;
        }());

    OptionBag a4;
    a4.Set(std::string(opt::kPageSize), std::string("a4"));
    MemorySink a4Sink;
    REQUIRE(RenderToPdf(html, a4Sink, a4).ok());

    OptionBag a5;
    a5.Set(std::string(opt::kPageSize), std::string("a5"));
    MemorySink a5Sink;
    REQUIRE(RenderToPdf(html, a5Sink, a5).ok());

    const std::size_t a4Pages = CountPages(AsText(a4Sink.bytes()));
    const std::size_t a5Pages = CountPages(AsText(a5Sink.bytes()));
    INFO("A4 pages: " << a4Pages << ", A5 pages: " << a5Pages);
    REQUIRE(a4Pages > 0);
    CHECK(a5Pages > a4Pages);
}

TEST_CASE("the environment is reused across renders", "[pdf][render]") {
    // Section 8.6: creating the environment costs 200-400 ms, navigation is
    // fast. If the second render were paying for a new environment the host
    // would not be doing its job. Timing is not asserted -- that belongs in the
    // section 17 benchmark suite -- but the reuse path is exercised here, and it
    // is also where a leaked pending request would show up as a failure.
    if (!pdf::WebView2Available()) {
        SKIP("WebView2 runtime not available");
    }

    for (int i = 0; i < 3; ++i) {
        MemorySink sink;
        const Result result =
            RenderToPdf("<!DOCTYPE html><html><body><p>Render " + std::to_string(i) +
                            "</p></body></html>",
                        sink);
        INFO("render " << i << " said: " << result.message());
        REQUIRE(result.ok());
        CHECK(AsText(sink.bytes()).starts_with("%PDF-"));
    }
}

TEST_CASE("a document larger than the NavigateToString limit still renders",
          "[pdf][render]") {
    // This is the requiresDisk path from section 4.3: above the limit the HTML
    // goes through the temp store and a file: URL instead.
    if (!pdf::WebView2Available()) {
        SKIP("WebView2 runtime not available");
    }

    std::string html = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"></head><body>";
    while (html.size() < pdf::kNavigateToStringLimit + 64 * 1024) {
        html += "<p>Filler paragraph to push this document past the string limit.</p>";
    }
    html += "</body></html>";
    REQUIRE(html.size() > pdf::kNavigateToStringLimit);

    MemorySink sink;
    const Result result = RenderToPdf(html, sink);
    INFO("renderer said: " << result.message());
    REQUIRE(result.ok());
    CHECK(AsText(sink.bytes()).starts_with("%PDF-"));
}

// --- The provider and the graph ----------------------------------------------

TEST_CASE("the provider declares one html to pdf edge", "[pdf][graph]") {
    const pdf::PdfWriter provider;
    const std::vector<EdgeDecl> edges = provider.EnumerateEdges();

    REQUIRE(edges.size() == 1);
    CHECK(edges[0].from.Id() == "text/html");
    CHECK(edges[0].to.Id() == "application/pdf");
    CHECK(edges[0].cost.fidelity == Fidelity::High);
    // Section 4.3: the only edge in the graph that spills to disk.
    CHECK(edges[0].cost.requiresDisk);
}

TEST_CASE("the provider recognises a PDF by its magic bytes", "[pdf][probe]") {
    const pdf::PdfWriter provider;
    PathHint hint;

    MemorySource looksRight("%PDF-1.7\n%\xE2\xE3\xCF\xD3\n");
    const auto profile = provider.Probe(looksRight, hint);
    REQUIRE(profile.has_value());
    CHECK(profile->Id() == "application/pdf");

    // The extension is never the decision (section 5).
    hint.extension = ".pdf";
    MemorySource notAPdf("this is plain text pretending to be a PDF");
    CHECK_FALSE(provider.Probe(notAPdf, hint).has_value());
}

TEST_CASE("the temp store cleans up after itself", "[pdf][temp]") {
    std::filesystem::path root;
    std::filesystem::path file;
    {
        ScopedTempStore store("unit");
        root = store.root();
        REQUIRE_FALSE(root.empty());
        CHECK(std::filesystem::exists(root));

        file = store.Acquire("render.html");
        std::ofstream(file) << "content";
        CHECK(std::filesystem::exists(file));
        // The extension survives, because WebView2 and Media Foundation key off it.
        CHECK(file.extension() == ".html");
    }
    CHECK_FALSE(std::filesystem::exists(file));
    CHECK_FALSE(std::filesystem::exists(root));
}

TEST_CASE("a hint cannot smuggle a path into the temp store", "[pdf][temp][security]") {
    // The hint can carry a filename derived from user input, and section 15.2 is
    // explicit that no user-supplied filename reaches the filesystem intact.
    ScopedTempStore store("unit");

    const std::filesystem::path escaped = store.Acquire("../../../evil.exe");
    CHECK(escaped.parent_path() == store.root());
    CHECK(escaped.filename().string().find("evil") == std::string::npos);

    const std::filesystem::path odd = store.Acquire("name.with spaces");
    CHECK(odd.parent_path() == store.root());
    CHECK(odd.extension().empty());
}
