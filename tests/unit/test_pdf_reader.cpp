// PDF text extraction (architecture section 8.7).
//
// These tests build their fixtures by RENDERING with WebView2 and then reading
// the result back with PDFium. That is a real round trip through two independent
// implementations -- Chromium's typesetter puts glyphs on a page, PDFium reports
// where they landed, and the reader has to infer paragraphs from nothing but
// those positions. A hand-written fixture PDF would test far less.
//
// What is asserted is deliberately limited to what section 8.7 claims: text
// survives, paragraphs are inferred from vertical gaps, headings from relative
// font size. Columns, tables and lists are explicitly not attempted, so there
// are no tests pretending otherwise.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include "morph/core/temp_store.h"
#include "morph/providers/html.h"
#include "morph/providers/markdown.h"
#include "morph/providers/pdf.h"

using namespace morph;
using namespace morph::doc;

namespace {

// Renders HTML to PDF bytes, or an empty vector when there is no runtime.
std::vector<std::byte> RenderPdf(std::string_view html) {
    ScopedTempStore temp("pdfread");
    std::stop_source stop;
    MemorySink sink;
    const Result result = pdf::RenderHtmlToPdf(html, {}, temp, sink, stop.get_token());
    INFO("renderer said: " << result.message());
    REQUIRE(result.ok());
    return sink.bytes();
}

Document ReadBack(std::span<const std::byte> bytes, Result& result) {
    Document document;
    result = pdf::ReadPdf(bytes, {}, document);
    return document;
}

Document ReadBack(std::span<const std::byte> bytes) {
    Result result = Result::Ok();
    Document document = ReadBack(bytes, result);
    INFO("reader said: " << result.message());
    REQUIRE(result.ok());
    return document;
}

bool Mentions(const Document& document, std::string_view needle) {
    return PlainText(document.blocks).find(needle) != std::string::npos;
}

std::string PageHtml(std::string_view body, std::string_view title = "Test") {
    return std::string("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>") +
           std::string(title) +
           "</title><style>body{font-family:Arial,sans-serif;font-size:12pt;}"
           "h1{font-size:24pt;}h2{font-size:18pt;}p{margin:0 0 1em;}</style></head><body>" +
           std::string(body) + "</body></html>";
}

}  // namespace

TEST_CASE("text survives a render and extraction round trip", "[pdfread]") {
    if (!pdf::WebView2Available()) SKIP("WebView2 runtime not available");

    const Document d = ReadBack(RenderPdf(PageHtml(
        "<p>The quick brown fox jumps over the lazy dog.</p>"
        "<p>Pack my box with five dozen liquor jugs.</p>")));

    CHECK(Mentions(d, "quick brown fox"));
    CHECK(Mentions(d, "five dozen liquor jugs"));
}

TEST_CASE("paragraph breaks are inferred from vertical gaps", "[pdfread]") {
    // PDF has no paragraphs, only positioned glyphs. This is the inference the
    // whole reader exists to make.
    if (!pdf::WebView2Available()) SKIP("WebView2 runtime not available");

    const Document d = ReadBack(RenderPdf(PageHtml(
        "<p>First paragraph, which is written long enough that it will certainly wrap onto a "
        "second line when it is laid out on an A4 page with normal margins.</p>"
        "<p>Second paragraph, also long enough to wrap, so that the reader has to distinguish a "
        "wrapped line from a genuine paragraph break rather than getting it right by luck.</p>")));

    std::size_t paragraphs = 0;
    ForEachBlock(d.blocks, [&](const Block& block) { paragraphs += block.Is<Paragraph>() ? 1 : 0; });

    INFO("extracted:\n" << PlainText(d.blocks));
    CHECK(paragraphs == 2);
    // A wrapped line must not have become its own paragraph, and the two halves
    // of a wrapped sentence must be joined by a space.
    CHECK(Mentions(d, "written long enough that it will certainly wrap"));
}

TEST_CASE("headings are recognised from relative font size", "[pdfread]") {
    if (!pdf::WebView2Available()) SKIP("WebView2 runtime not available");

    const Document d = ReadBack(RenderPdf(PageHtml(
        "<h1>Annual Report</h1>"
        "<p>Some body text that establishes what the normal font size on this page is, so that "
        "the heading above it stands out by comparison.</p>"
        "<h2>Second Section</h2>"
        "<p>More body text following the second heading, again long enough to be unambiguous "
        "about the size of ordinary prose.</p>")));

    std::vector<std::pair<int, std::string>> headings;
    ForEachBlock(d.blocks, [&](const Block& block) {
        if (const auto* h = block.As<Heading>()) {
            headings.emplace_back(h->level, PlainText({block}));
        }
    });

    INFO("extracted:\n" << PlainText(d.blocks));
    REQUIRE(headings.size() == 2);
    CHECK(headings[0].second.find("Annual Report") != std::string::npos);
    CHECK(headings[1].second.find("Second Section") != std::string::npos);
    // The h1 was set larger than the h2, so it must come back at a lower level.
    CHECK(headings[0].first < headings[1].first);
}

TEST_CASE("a heading gets a usable anchor", "[pdfread]") {
    if (!pdf::WebView2Available()) SKIP("WebView2 runtime not available");

    const Document d = ReadBack(RenderPdf(PageHtml(
        "<h1>Introduction</h1><p>Body text long enough to set the baseline font size here.</p>")));

    const Heading* heading = nullptr;
    ForEachBlock(d.blocks, [&](const Block& block) {
        if (const auto* h = block.As<Heading>(); h != nullptr && heading == nullptr) heading = h;
    });
    REQUIRE(heading != nullptr);
    CHECK(heading->anchorId == "introduction");
}

TEST_CASE("document metadata is carried through", "[pdfread]") {
    if (!pdf::WebView2Available()) SKIP("WebView2 runtime not available");

    const Document d = ReadBack(RenderPdf(PageHtml("<p>Body text.</p>", "Quarterly Results")));
    CHECK(d.meta.title == "Quarterly Results");
    CHECK(d.meta.extra.count("pdf-page-count") == 1);
}

// --- The fidelity contract ---------------------------------------------------

TEST_CASE("extraction is always reported as structural", "[pdfread][fidelity]") {
    // Section 4.3: "Layout is not recoverable from PDF in the general case; we
    // say so rather than pretending." The rating and the note are not optional
    // extras, they are the product difference.
    if (!pdf::WebView2Available()) SKIP("WebView2 runtime not available");

    Result result = Result::Ok();
    const Document d = ReadBack(RenderPdf(PageHtml("<p>Anything at all.</p>")), result);

    REQUIRE(result.ok());
    CHECK(result.incurred() == Fidelity::Structural);

    bool explained = false;
    for (const FidelityNote& note : result.notes()) {
        explained |= note.code == "pdf-structure-inferred";
    }
    CHECK(explained);

    // And the note has to say something a person can act on, not just name a code.
    for (const FidelityNote& note : result.notes()) {
        if (note.code != "pdf-structure-inferred") continue;
        CHECK(note.detail.size() > 60);
        CHECK(note.detail.find("best guess") != std::string::npos);
    }
    (void)d;
}

TEST_CASE("the declared read cost matches what is delivered", "[pdfread][fidelity]") {
    // The planner publishes ReadCost BEFORE the conversion runs, and the badge
    // the user sees comes from it. If it disagreed with what Read() actually
    // incurs, the fidelity contract would be decorative.
    const pdf::PdfReader reader;
    CHECK(reader.ReadCost(*MediaProfile::Parse("application/pdf")).fidelity ==
          Fidelity::Structural);
    CHECK(reader.Reads().size() == 1);
    CHECK(reader.Reads()[0].Id() == "application/pdf");
}

// --- Malformed and unsupported input -----------------------------------------

TEST_CASE("a file that is not a PDF is refused with a real reason", "[pdfread][security]") {
    const std::string junk = "This is not a PDF, it is just some text in a file.";
    const auto* begin = reinterpret_cast<const std::byte*>(junk.data());

    Document d;
    const Result result = pdf::ReadPdf(std::span(begin, junk.size()), {}, d);

    CHECK_FALSE(result.ok());
    CHECK(result.status() == Status::kMalformedInput);
    CHECK_FALSE(result.message().empty());
}

TEST_CASE("a truncated PDF is refused rather than crashing", "[pdfread][security]") {
    if (!pdf::WebView2Available()) SKIP("WebView2 runtime not available");

    std::vector<std::byte> bytes = RenderPdf(PageHtml("<p>Body.</p>"));
    REQUIRE(bytes.size() > 2000);
    // Chop off the cross-reference table at the end, which is what a partial
    // download or a damaged file looks like.
    bytes.resize(bytes.size() / 2);

    Document d;
    const Result result = pdf::ReadPdf(bytes, {}, d);
    // PDFium may recover a truncated file by rebuilding the xref, so either
    // outcome is legitimate. What is not legitimate is a crash or a silent empty
    // document presented as success.
    if (result.ok()) {
        CHECK_FALSE(d.blocks.empty());
    } else {
        CHECK_FALSE(result.message().empty());
    }
}

TEST_CASE("an empty span is refused", "[pdfread][security]") {
    Document d;
    const Result result = pdf::ReadPdf({}, {}, d);
    CHECK_FALSE(result.ok());
    CHECK(result.status() == Status::kMalformedInput);
}

TEST_CASE("a PDF with no extractable text says so instead of returning nothing",
          "[pdfread]") {
    // A scan. Returning an empty document would look like a successful
    // conversion that lost everything; OCR is out of scope for v1 (section 1),
    // so the honest answer is to name the problem.
    if (!pdf::WebView2Available()) SKIP("WebView2 runtime not available");

    // A page containing only an image and no text at all.
    const std::vector<std::byte> bytes = RenderPdf(PageHtml(
        "<img alt=\"\" src=\"data:image/png;base64,"
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==\""
        " style=\"width:200px;height:200px\">"));

    Document d;
    const Result result = pdf::ReadPdf(bytes, {}, d);

    CHECK_FALSE(result.ok());
    CHECK(result.status() == Status::kUnsupportedInput);
    CHECK(result.message().find("OCR") != std::string::npos);
}

// --- What the graph gets for free --------------------------------------------

TEST_CASE("PDF reaches Markdown through the IR", "[pdfread][roundtrip]") {
    // Nobody wrote a PDF-to-Markdown converter. The reader produces the IR and
    // the Markdown writer consumes it.
    if (!pdf::WebView2Available()) SKIP("WebView2 runtime not available");

    const Document d = ReadBack(RenderPdf(PageHtml(
        "<h1>Extracted Title</h1>"
        "<p>A paragraph of prose long enough to establish the ordinary font size used on this "
        "page, so the heading above is distinguishable from it.</p>")));

    std::string markdown;
    REQUIRE(docmd::WriteMarkdown(d, {}, markdown).ok());

    INFO(markdown);
    CHECK(markdown.find("Extracted Title") != std::string::npos);
    CHECK(markdown.find("# ") != std::string::npos);
    CHECK(markdown.find("ordinary font size") != std::string::npos);
}

TEST_CASE("a full document survives markdown to pdf and back", "[pdfread][roundtrip]") {
    // md -> IR -> html -> pdf -> IR -> md. Six format boundaries. What survives
    // is the words and the headings, which is exactly what STRUCTURAL promises
    // and no more.
    if (!pdf::WebView2Available()) SKIP("WebView2 runtime not available");

    Document source;
    REQUIRE(docmd::ReadMarkdown(
                "# Morph Report\n"
                "\n"
                "An opening paragraph with enough words in it to wrap across more than one line "
                "when it is laid out on a page.\n"
                "\n"
                "## Findings\n"
                "\n"
                "A second paragraph under the second heading, also comfortably long.\n",
                {}, source)
                .ok());

    std::string html;
    REQUIRE(dochtml::WriteHtml(source, {}, html).ok());

    const Document extracted = ReadBack(RenderPdf(html));

    std::string markdown;
    REQUIRE(docmd::WriteMarkdown(extracted, {}, markdown).ok());

    INFO(markdown);
    for (const std::string_view phrase :
         {"Morph Report", "opening paragraph", "Findings", "second heading"}) {
        INFO("looking for: " << phrase);
        CHECK(markdown.find(phrase) != std::string::npos);
    }
}
