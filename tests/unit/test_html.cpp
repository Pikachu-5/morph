// HTML reader and writer (architecture section 8.5).
//
// Two things are being checked. That the SEMANTIC WHITELIST does what it claims:
// unknown elements are transparent, not dropped, and presentational markup is
// ignored rather than interpreted. And that the writer's output is
// self-contained and deterministic, because it is the PDF stage's input.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "morph/providers/html.h"
#include "morph/providers/markdown.h"

using namespace morph;
using namespace morph::doc;
using morph::dochtml::EscapeHtmlAttribute;
using morph::dochtml::EscapeHtmlText;
using morph::dochtml::ReadHtml;
using morph::dochtml::WriteHtml;

namespace {

Document Read(std::string_view html) {
    Document document;
    const Result result = ReadHtml(html, {}, document);
    REQUIRE(result.ok());
    return document;
}

std::string Write(const Document& document) {
    std::string out;
    const Result result = WriteHtml(document, {}, out);
    REQUIRE(result.ok());
    return out;
}

std::vector<std::byte> Bytes(std::string_view text) {
    return std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                  reinterpret_cast<const std::byte*>(text.data()) + text.size());
}

}  // namespace

// --- Escaping ----------------------------------------------------------------

TEST_CASE("text escaping closes the markup-injection routes", "[html][escape]") {
    CHECK(EscapeHtmlText("a < b & c > d") == "a &lt; b &amp; c &gt; d");
    CHECK(EscapeHtmlText("<script>alert(1)</script>") ==
          "&lt;script&gt;alert(1)&lt;/script&gt;");
}

TEST_CASE("attribute escaping covers both quote styles", "[html][escape]") {
    // We always emit double quotes, but a value that escapes only one style is
    // one refactor away from an injection.
    CHECK(EscapeHtmlAttribute("a\"b'c") == "a&quot;b&#39;c");
    CHECK(EscapeHtmlAttribute("x\" onload=\"evil()") ==
          "x&quot; onload=&quot;evil()");
}

// --- The semantic whitelist --------------------------------------------------

TEST_CASE("unknown elements are transparent, not dropped", "[html][read]") {
    // A wrapper is not content, but what it wraps is. Dropping the wrapper's
    // children instead is the classic naive-reader bug.
    const Document d = Read(
        "<div class=\"wrapper\"><section><article>"
        "<p>survives</p>"
        "</article></section></div>");

    REQUIRE(d.blocks.size() == 1);
    CHECK(PlainText(d.blocks) == "survives\n");
}

TEST_CASE("script and style content never becomes text", "[html][read]") {
    // Keeping these would put JavaScript source into a Word document.
    const Document d = Read(
        "<style>body{color:red}</style>"
        "<script>var x = 1;</script>"
        "<p>only this</p>");

    const std::string text = PlainText(d.blocks);
    CHECK(text.find("color:red") == std::string::npos);
    CHECK(text.find("var x") == std::string::npos);
    CHECK(text.find("only this") != std::string::npos);
}

TEST_CASE("presentational markup is ignored, not interpreted", "[html][read]") {
    // <font> and <center> are how 2003 said "formatting". Guessing at them
    // produces a document that is subtly wrong in ways nobody can explain.
    const Document d = Read("<center><font color=\"red\" size=\"7\">plain</font></center>");

    bool styled = false;
    ForEachBlock(d.blocks, [&](const Block& block) {
        if (const auto* p = block.As<Paragraph>()) {
            ForEachInline(p->inlines, [&](const Inline& node) {
                if (const auto* text = node.As<Text>()) styled |= !text->style.Empty();
            });
        }
    });
    CHECK_FALSE(styled);
    CHECK(PlainText(d.blocks) == "plain\n");
}

TEST_CASE("b and i map to semantic nodes", "[html][read]") {
    // HTML5 gives them meaning and every real document uses them, so unlike
    // <font> they are mapped rather than ignored.
    const Document d = Read("<p><b>bold</b> and <i>italic</i></p>");

    bool strong = false, emphasis = false;
    ForEachInline(d.blocks[0].As<Paragraph>()->inlines, [&](const Inline& node) {
        strong |= node.Is<Strong>();
        emphasis |= node.Is<Emphasis>();
    });
    CHECK(strong);
    CHECK(emphasis);
}

TEST_CASE("malformed nesting is repaired the way a browser would", "[html][read]") {
    // The reason for lexbor rather than a hand-rolled parser: a naive one puts
    // the second paragraph inside the first.
    const Document d = Read("<p>one<p>two<p>three");
    CHECK(CountBlocks(d.blocks) == 3);
}

TEST_CASE("whitespace is collapsed, so source indentation is not content",
          "[html][read]") {
    const Document d = Read("<p>\n    lots\n    of\n    space\n</p>");
    CHECK(PlainText(d.blocks) == "lots of space\n");
}

TEST_CASE("pre keeps its whitespace exactly", "[html][read]") {
    // The one place where collapsing would destroy the content rather than tidy
    // it.
    const Document d = Read("<pre><code>line one\n    indented\n</code></pre>");
    const auto* code = d.blocks[0].As<CodeBlock>();
    REQUIRE(code != nullptr);
    CHECK(code->text == "line one\n    indented\n");
}

TEST_CASE("a code language class is recognised", "[html][read]") {
    const Document d = Read("<pre><code class=\"language-rust\">fn main() {}</code></pre>");
    CHECK(d.blocks[0].As<CodeBlock>()->language == "rust");
}

TEST_CASE("headings keep a source id in preference to a generated slug",
          "[html][read]") {
    // Links elsewhere in the document point at the source id.
    const Document d = Read("<h2 id=\"custom-target\">Some Heading</h2><h2>Some Heading</h2>");
    CHECK(d.blocks[0].As<Heading>()->anchorId == "custom-target");
    CHECK(d.blocks[1].As<Heading>()->anchorId == "some-heading");
}

TEST_CASE("a table's spans become covered placeholders", "[html][read]") {
    // Section 8.4's second pass, applied to HTML. Keeping the covered cells in
    // the grid is what makes indexing by (row, column) safe for every writer.
    const Document d = Read(
        "<table>"
        "<tr><th colspan=\"2\">Wide</th></tr>"
        "<tr><td>a</td><td>b</td></tr>"
        "</table>");

    const auto* table = d.blocks[0].As<Table>();
    REQUIRE(table != nullptr);
    REQUIRE(table->rows.size() == 2);
    CHECK(table->headerRows == 1);
    REQUIRE(table->rows[0].cells.size() == 2);
    CHECK(table->rows[0].cells[0].colSpan == 2);
    CHECK(table->rows[0].cells[1].covered);
    CHECK_FALSE(table->rows[1].cells[0].covered);
}

TEST_CASE("a rowspan marks the cell below as covered", "[html][read]") {
    const Document d = Read(
        "<table>"
        "<tr><td rowspan=\"2\">tall</td><td>a</td></tr>"
        "<tr><td>b</td></tr>"
        "</table>");

    const auto* table = d.blocks[0].As<Table>();
    REQUIRE(table != nullptr);
    REQUIRE(table->rows.size() == 2);
    CHECK(table->rows[0].cells[0].rowSpan == 2);
    REQUIRE(table->rows[1].cells.size() == 2);
    CHECK(table->rows[1].cells[0].covered);
    CHECK(PlainText(table->rows[1].cells[1].blocks) == "b\n");
}

TEST_CASE("a task list checkbox is read back", "[html][read]") {
    const Document d = Read(
        "<ul><li><input type=\"checkbox\" disabled checked>done</li>"
        "<li><input type=\"checkbox\" disabled>todo</li></ul>");

    const auto* list = d.blocks[0].As<ListBlock>();
    REQUIRE(list != nullptr);
    REQUIRE(list->items.size() == 2);
    CHECK(list->items[0].checked == true);
    CHECK(list->items[1].checked == false);
}

TEST_CASE("a data URI image is decoded into the resource store", "[html][read]") {
    // Bytes already in hand, so they belong in the store. A remote URL is left
    // alone: section 1 is local only, with no network egress at all.
    Document document;
    const Result result = ReadHtml(
        "<p><img alt=\"dot\" src=\"data:image/png;base64,iVBORw0KGgo=\"></p>", {}, document);
    REQUIRE(result.ok());

    CHECK(document.res.Size() == 1);
    CHECK(ReferencedResources(document).size() == 1);
    CHECK(document.res.All().begin()->second.mime == "image/png");
}

TEST_CASE("a remote image URL is preserved and never fetched", "[html][read]") {
    const Document d = Read("<p><img alt=\"x\" src=\"https://example.com/a.png\"></p>");
    CHECK(d.res.Empty());

    bool found = false;
    ForEachInline(d.blocks[0].As<Paragraph>()->inlines, [&](const Inline& node) {
        if (const auto* image = node.As<InlineImage>()) {
            found = true;
            CHECK(image->resource == "https://example.com/a.png");
        }
    });
    CHECK(found);
}

TEST_CASE("title and language are read from the head", "[html][read]") {
    const Document d = Read(
        "<html lang=\"en-GB\"><head><title>My Title</title></head><body><p>x</p></body></html>");
    CHECK(d.meta.title == "My Title");
    CHECK(d.meta.lang == "en-GB");
}

TEST_CASE("deeply nested input is truncated rather than overflowing the stack",
          "[html][security]") {
    std::string bomb;
    for (int i = 0; i < 400; ++i) bomb += "<blockquote>";
    bomb += "boom";
    for (int i = 0; i < 400; ++i) bomb += "</blockquote>";

    Document document;
    const Result result = ReadHtml(bomb, {}, document);

    // HTML has no parse errors, so this succeeds with a note rather than
    // failing. What it must not do is recurse 400 deep and die.
    REQUIRE(result.ok());
    CHECK(MaxDepth(document.blocks) <= kMaxBlockDepth + 1);
    bool noted = false;
    for (const FidelityNote& note : result.notes()) noted |= note.code == "nesting-truncated";
    CHECK(noted);
}

// --- Writing -----------------------------------------------------------------

TEST_CASE("the writer emits a complete self-contained document", "[html][write]") {
    Document document;
    document.meta.title = "Report";
    document.meta.lang = "en";
    document.blocks.push_back(Block::Of(Heading{1, {Inline::Of(Text{"Report", {}})}, "", "report"}));

    const std::string html = Write(document);

    CHECK(html.starts_with("<!DOCTYPE html>\n"));
    CHECK(html.find("<html lang=\"en\">") != std::string::npos);
    CHECK(html.find("<meta charset=\"utf-8\">") != std::string::npos);
    CHECK(html.find("<title>Report</title>") != std::string::npos);
    CHECK(html.find("<h1 id=\"report\">Report</h1>") != std::string::npos);
    CHECK(html.ends_with("</html>\n"));
}

TEST_CASE("the output has no external references at all", "[html][write]") {
    // The PDF host blocks non-data:/file: navigation (section 15.1), so an
    // external reference would not merely be slow, it would silently not load.
    Document document;
    document.res.Add(Bytes("\x89PNG\r\n\x1a\n fake"), "image/png");
    const ResourceId id = document.res.All().begin()->first;
    document.blocks.push_back(Block::Of(ImageBlock{id, "alt", "", 0, 0}));

    const std::string html = Write(document);

    CHECK(html.find("data:image/png;base64,") != std::string::npos);
    CHECK(html.find("http://") == std::string::npos);
    CHECK(html.find("https://") == std::string::npos);
    CHECK(html.find("<link ") == std::string::npos);
    CHECK(html.find("@import") == std::string::npos);
}

TEST_CASE("the writer is deterministic", "[html][write]") {
    // Golden-file testing depends on this, and so does diffing two conversions.
    // It is also why no generator or timestamp meta tag is emitted.
    const Document d = Read("<h1>Title</h1><p>Body with <em>emphasis</em>.</p>");
    CHECK(Write(d) == Write(d));
    CHECK(Write(d).find("generator") == std::string::npos);
}

TEST_CASE("spans become colspan and rowspan, covered cells become nothing",
          "[html][write]") {
    Document document;
    Table table;
    TableCell wide;
    wide.colSpan = 2;
    wide.blocks.push_back(
        Block::Of(Paragraph{{Inline::Of(Text{"wide", {}})}, {}, {}, std::nullopt}));
    TableCell covered;
    covered.covered = true;
    table.rows.push_back(TableRow{{std::move(wide), std::move(covered)}});
    table.headerRows = 1;
    document.blocks.push_back(Block::Of(std::move(table)));

    const std::string html = Write(document);
    CHECK(html.find("colspan=\"2\"") != std::string::npos);
    // One th, not two: the covered placeholder is grid bookkeeping, not content.
    CHECK(html.find("<th") != std::string::npos);
    CHECK(html.find("</th>\n<th") == std::string::npos);
}

TEST_CASE("a tight list item does not gain a paragraph", "[html][write]") {
    // <li><p>x</p></li> renders with a margin that makes every list look wrong.
    const Document d = Read("<ul><li>one</li><li>two</li></ul>");
    const std::string html = Write(d);
    CHECK(html.find("<li>one</li>") != std::string::npos);
}

// --- Round trips -------------------------------------------------------------

TEST_CASE("HTML round-trips through the IR", "[html][roundtrip]") {
    const std::string source =
        "<h1>Title</h1>"
        "<p>Text with <strong>strong</strong>, <em>em</em>, <code>code</code> and a "
        "<a href=\"https://example.com\">link</a>.</p>"
        "<ul><li>one</li><li>two</li></ul>"
        "<blockquote><p>quoted</p></blockquote>"
        "<pre><code class=\"language-c\">int x;</code></pre>"
        "<table><tr><th>H</th></tr><tr><td>D</td></tr></table>";

    const Document first = Read(source);
    const Document second = Read(Write(first));

    CHECK(PlainText(first.blocks) == PlainText(second.blocks));
    CHECK(CountBlocks(first.blocks) == CountBlocks(second.blocks));
}

TEST_CASE("markdown reaches HTML through the IR with no direct converter",
          "[html][roundtrip]") {
    // The whole point of section 8.2: N formats need 2N readers and writers.
    // Nobody wrote a Markdown-to-HTML converter; the graph composed one.
    Document document;
    REQUIRE(morph::docmd::ReadMarkdown(
                "# Heading\n\nA paragraph with **bold** text.\n\n- a\n- b\n", {}, document)
                .ok());

    const std::string html = Write(document);
    CHECK(html.find("<h1 id=\"heading\">Heading</h1>") != std::string::npos);
    CHECK(html.find("<strong>bold</strong>") != std::string::npos);
    CHECK(html.find("<li>a</li>") != std::string::npos);
}

TEST_CASE("html reaches markdown through the IR", "[html][roundtrip]") {
    const Document d = Read(
        "<h2>Section</h2><p>Some <em>text</em>.</p><ol start=\"3\"><li>third</li></ol>");

    std::string markdown;
    REQUIRE(morph::docmd::WriteMarkdown(d, {}, markdown).ok());

    CHECK(markdown.find("## Section") != std::string::npos);
    CHECK(markdown.find("*text*") != std::string::npos);
    CHECK(markdown.find("3. third") != std::string::npos);
}
