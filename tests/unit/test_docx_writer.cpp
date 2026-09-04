// The DOCX writer (architecture section 8.4, WRITING).
//
// The strongest test here is the round trip: write a document, read it back with
// our own reader, and compare. That catches both halves at once and is the same
// property section 16 asks for at the IR level.
//
// It has one blind spot, learned the hard way from the Markdown tight-list bug:
// a round trip cannot see a mistake both sides make. So the structural
// assertions below check the emitted XML directly wherever the reader and writer
// could plausibly agree on something wrong.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "docx_fixture.h"
#include "morph/providers/docx.h"
#include "morph/providers/html.h"
#include "morph/providers/markdown.h"
#include "morph/providers/opc.h"

using namespace morph;
using namespace morph::doc;
using morph::docooxml::ReadDocx;
using morph::docooxml::WriteDocx;

namespace {

std::vector<std::byte> Write(const Document& document) {
    std::vector<std::byte> bytes;
    const Result result = WriteDocx(document, {}, bytes);
    INFO("writer said: " << result.message());
    REQUIRE(result.ok());
    return bytes;
}

Document Read(std::span<const std::byte> bytes) {
    Document document;
    const Result result = ReadDocx(bytes, {}, document);
    INFO("reader said: " << result.message());
    REQUIRE(result.ok());
    return document;
}

Document RoundTrip(const Document& document) { return Read(Write(document)); }

// The raw word/document.xml, for the assertions a round trip cannot make.
std::string DocumentXml(std::span<const std::byte> package) {
    opc::Package opened;
    REQUIRE(opc::Package::Open(package, opened).ok());
    const opc::Part* part = opened.Find("/word/document.xml");
    REQUIRE(part != nullptr);
    return std::string(reinterpret_cast<const char*>(part->bytes.data()), part->bytes.size());
}

Document FromMarkdown(std::string_view markdown) {
    Document document;
    REQUIRE(morph::docmd::ReadMarkdown(markdown, {}, document).ok());
    return document;
}

std::vector<std::byte> FakePng() {
    std::string png = "\x89PNG\r\n\x1a\n";
    png += std::string(4, '\0');
    png += "IHDR";
    png += std::string({'\0', '\0', '\0', '\x60'});  // 96 wide
    png += std::string({'\0', '\0', '\0', '\x30'});  // 48 high
    return std::vector<std::byte>(reinterpret_cast<const std::byte*>(png.data()),
                                  reinterpret_cast<const std::byte*>(png.data()) + png.size());
}

}  // namespace

// --- The package ------------------------------------------------------------

TEST_CASE("the writer emits the minimum valid package", "[docx][write]") {
    // Section 8.4's list, and nothing else: a package with parts Word does not
    // need is a package with more surface to get wrong.
    const std::vector<std::byte> bytes = Write(FromMarkdown("# Title\n\nBody.\n"));

    opc::Package package;
    REQUIRE(opc::Package::Open(bytes, package).ok());

    CHECK(package.Contains("/[Content_Types].xml"));
    CHECK(package.Contains("/_rels/.rels"));
    CHECK(package.Contains("/word/document.xml"));
    CHECK(package.Contains("/word/styles.xml"));
    CHECK(package.Contains("/word/_rels/document.xml.rels"));
    // No lists and no images in this document, so neither part should exist.
    CHECK_FALSE(package.Contains("/word/numbering.xml"));

    const opc::Relationship* main =
        package.FindRelationshipByType("", opc::rel::kOfficeDocument);
    REQUIRE(main != nullptr);
    CHECK(opc::Package::ResolveTarget("/", main->target) == "/word/document.xml");
}

TEST_CASE("the output is byte-reproducible", "[docx][write][determinism]") {
    // Section 8.4 asks for this so golden-file testing is possible.
    const Document d = FromMarkdown("# Title\n\n- a\n- b\n\nText with **bold**.\n");
    CHECK(Write(d) == Write(d));
}

TEST_CASE("a body ends with section properties", "[docx][write]") {
    // Word reports a document with no sectPr as having a layout problem.
    const std::string xml = DocumentXml(Write(FromMarkdown("Text.\n")));
    CHECK(xml.find("<w:sectPr>") != std::string::npos);
    CHECK(xml.find("</w:body>") > xml.find("</w:sectPr>"));
}

// --- Paragraphs, runs, formatting --------------------------------------------

TEST_CASE("text and formatting round-trip", "[docx][write][roundtrip]") {
    const Document original = FromMarkdown(
        "Plain text with **bold**, *italic*, ~~struck~~ and `code`.\n");
    const Document back = RoundTrip(original);

    CHECK(PlainText(back.blocks) == PlainText(original.blocks));

    std::size_t strong = 0, emphasis = 0, strike = 0;
    ForEachBlock(back.blocks, [&](const Block& block) {
        if (const auto* p = block.As<Paragraph>()) {
            ForEachInline(p->inlines, [&](const Inline& node) {
                strong += node.Is<Strong>() ? 1 : 0;
                emphasis += node.Is<Emphasis>() ? 1 : 0;
                strike += node.Is<Strikethrough>() ? 1 : 0;
            });
        }
    });
    CHECK(strong == 1);
    CHECK(emphasis == 1);
    CHECK(strike == 1);
}

TEST_CASE("every run preserves its whitespace", "[docx][write]") {
    // Without xml:space="preserve" Word collapses a run that is a single space,
    // and "bold" followed by " text" loses the gap. It only shows up where
    // formatting changes mid-sentence, which is why it is asserted on the XML
    // rather than left to the round trip.
    const std::string xml = DocumentXml(Write(FromMarkdown("a **b** c\n")));

    std::size_t runs = 0, preserved = 0;
    for (std::size_t at = 0; (at = xml.find("<w:t", at)) != std::string::npos; ++at) {
        ++runs;
        if (xml.compare(at, 26, R"(<w:t xml:space="preserve">)") == 0) ++preserved;
    }
    REQUIRE(runs > 0);
    CHECK(runs == preserved);
}

TEST_CASE("headings use the built-in styles and outline levels", "[docx][write]") {
    // This is what puts a converted document's headings in Word's navigation
    // pane instead of rendering them as anonymous bold text.
    const std::vector<std::byte> bytes = Write(FromMarkdown("# One\n\n## Two\n"));
    const std::string xml = DocumentXml(bytes);

    CHECK(xml.find(R"(<w:pStyle w:val="Heading1"/>)") != std::string::npos);
    CHECK(xml.find(R"(<w:pStyle w:val="Heading2"/>)") != std::string::npos);

    opc::Package package;
    REQUIRE(opc::Package::Open(bytes, package).ok());
    const opc::Part* styles = package.Find("/word/styles.xml");
    REQUIRE(styles != nullptr);
    const std::string styleXml(reinterpret_cast<const char*>(styles->bytes.data()),
                               styles->bytes.size());
    CHECK(styleXml.find(R"(<w:outlineLvl w:val="0"/>)") != std::string::npos);
    CHECK(styleXml.find(R"(<w:name w:val="heading 1"/>)") != std::string::npos);
}

TEST_CASE("headings round-trip as headings", "[docx][write][roundtrip]") {
    const Document back = RoundTrip(FromMarkdown("# One\n\n## Two\n\nBody.\n"));
    REQUIRE(back.blocks.size() == 3);
    REQUIRE(back.blocks[0].As<Heading>() != nullptr);
    CHECK(back.blocks[0].As<Heading>()->level == 1);
    CHECK(back.blocks[1].As<Heading>()->level == 2);
    CHECK(back.blocks[2].Is<Paragraph>());
}

TEST_CASE("font size is written as half-points", "[docx][write][units]") {
    Document document;
    Text styled{"eleven point", {}};
    styled.style.fontSizePt = 11.0f;
    document.blocks.push_back(
        Block::Of(Paragraph{{Inline::Of(std::move(styled))}, {}, {}, std::nullopt}));

    CHECK(DocumentXml(Write(document)).find(R"(<w:sz w:val="22"/>)") != std::string::npos);

    const Document back = RoundTrip(document);
    const auto* text = back.blocks[0].As<Paragraph>()->inlines[0].As<Text>();
    REQUIRE(text != nullptr);
    REQUIRE(text->style.fontSizePt.has_value());
    CHECK(*text->style.fontSizePt == 11.0f);
}

// --- Lists (mapping 2, backwards) --------------------------------------------

TEST_CASE("a list is flattened into numbered paragraphs", "[docx][write][mapping2]") {
    // WordprocessingML has no list element, so the tree becomes paragraphs
    // carrying w:numPr -- exactly the encoding the reader reconstructs.
    const std::vector<std::byte> bytes = Write(FromMarkdown("- a\n- b\n"));
    const std::string xml = DocumentXml(bytes);

    CHECK(xml.find("<w:numPr>") != std::string::npos);
    CHECK(xml.find(R"(<w:ilvl w:val="0"/>)") != std::string::npos);

    opc::Package package;
    REQUIRE(opc::Package::Open(bytes, package).ok());
    CHECK(package.Contains("/word/numbering.xml"));
}

TEST_CASE("lists round-trip with their content", "[docx][write][roundtrip]") {
    // The assertion the Markdown tight-list bug taught: check the CONTENT, not
    // just the shape, because a round trip cannot see a mistake both sides make.
    const Document back = RoundTrip(FromMarkdown("- alpha\n- beta\n- gamma\n"));

    REQUIRE(back.blocks.size() == 1);
    const auto* list = back.blocks[0].As<ListBlock>();
    REQUIRE(list != nullptr);
    REQUIRE(list->items.size() == 3);
    CHECK(PlainText(list->items[0].blocks) == "alpha\n");
    CHECK(PlainText(back.blocks) == "alpha\nbeta\ngamma\n");
}

TEST_CASE("nested lists round-trip", "[docx][write][roundtrip]") {
    const Document back = RoundTrip(FromMarkdown("- outer\n  - inner\n- back\n"));

    REQUIRE(back.blocks.size() == 1);
    const auto* list = back.blocks[0].As<ListBlock>();
    REQUIRE(list != nullptr);
    REQUIRE(list->items.size() == 2);

    bool nested = false;
    for (const Block& block : list->items[0].blocks) nested |= block.Is<ListBlock>();
    CHECK(nested);
    CHECK(PlainText(back.blocks) == "outer\ninner\nback\n");
}

TEST_CASE("an ordered list keeps its start number", "[docx][write][roundtrip]") {
    const Document back = RoundTrip(FromMarkdown("5. five\n6. six\n"));
    const auto* list = back.blocks[0].As<ListBlock>();
    REQUIRE(list != nullptr);
    CHECK(list->ordered);
    CHECK(list->start == 5);
}

TEST_CASE("two sibling ordered lists restart rather than continue",
          "[docx][write][mapping4]") {
    // Each list gets its own numId with an explicit startOverride. Sharing one
    // numId would number the second list 3, 4 instead of 1, 2.
    Document document;
    document.blocks.push_back(Block::Of(ListBlock{
        true, 1, true,
        {ListItem{{Block::Of(Paragraph{{Inline::Of(Text{"a", {}})}, {}, {}, std::nullopt})}, {}}}}));
    document.blocks.push_back(
        Block::Of(Paragraph{{Inline::Of(Text{"between", {}})}, {}, {}, std::nullopt}));
    document.blocks.push_back(Block::Of(ListBlock{
        true, 1, true,
        {ListItem{{Block::Of(Paragraph{{Inline::Of(Text{"b", {}})}, {}, {}, std::nullopt})}, {}}}}));

    const std::vector<std::byte> bytes = Write(document);
    const std::string xml = DocumentXml(bytes);
    CHECK(xml.find(R"(<w:numId w:val="1"/>)") != std::string::npos);
    CHECK(xml.find(R"(<w:numId w:val="2"/>)") != std::string::npos);

    const Document back = Read(bytes);
    REQUIRE(back.blocks.size() == 3);
    CHECK(back.blocks[0].Is<ListBlock>());
    CHECK(back.blocks[2].Is<ListBlock>());
    CHECK(back.blocks[2].As<ListBlock>()->start == 1);
}

// --- Tables (mapping 3, backwards) -------------------------------------------

TEST_CASE("a table round-trips", "[docx][write][roundtrip]") {
    const Document back = RoundTrip(FromMarkdown(
        "| A | B |\n"
        "| - | - |\n"
        "| 1 | 2 |\n"));

    REQUIRE(back.blocks.size() >= 1);
    const auto* table = back.blocks[0].As<Table>();
    REQUIRE(table != nullptr);
    REQUIRE(table->rows.size() == 2);
    CHECK(table->headerRows == 1);
    CHECK(PlainText(table->rows[0].cells[0].blocks) == "A\n");
    CHECK(PlainText(table->rows[1].cells[1].blocks) == "2\n");
}

TEST_CASE("colspan survives as gridSpan", "[docx][write][mapping3]") {
    Document document;
    Table table;
    TableCell wide;
    wide.colSpan = 2;
    wide.blocks.push_back(
        Block::Of(Paragraph{{Inline::Of(Text{"wide", {}})}, {}, {}, std::nullopt}));
    TableCell covered;
    covered.covered = true;

    TableCell a, b;
    a.blocks.push_back(Block::Of(Paragraph{{Inline::Of(Text{"a", {}})}, {}, {}, std::nullopt}));
    b.blocks.push_back(Block::Of(Paragraph{{Inline::Of(Text{"b", {}})}, {}, {}, std::nullopt}));

    table.columns.resize(2);
    table.rows.push_back(TableRow{{std::move(wide), std::move(covered)}});
    table.rows.push_back(TableRow{{std::move(a), std::move(b)}});
    document.blocks.push_back(Block::Of(std::move(table)));

    const std::vector<std::byte> bytes = Write(document);
    CHECK(DocumentXml(bytes).find(R"(<w:gridSpan w:val="2"/>)") != std::string::npos);

    const Document back = Read(bytes);
    const auto* out = back.blocks[0].As<Table>();
    REQUIRE(out != nullptr);
    CHECK(out->rows[0].cells[0].colSpan == 2);
    CHECK(out->rows[0].cells[1].covered);
    CHECK(PlainText(out->rows[1].cells[1].blocks) == "b\n");
}

TEST_CASE("rowspan survives as vMerge", "[docx][write][mapping3]") {
    // The covering cell gets w:vMerge val="restart"; the covered cells below get
    // a bare w:vMerge. Dropping the covered ones -- which is what the horizontal
    // case needs -- would collapse the table.
    Document document;
    Table table;
    table.columns.resize(2);

    TableCell tall;
    tall.rowSpan = 2;
    tall.blocks.push_back(
        Block::Of(Paragraph{{Inline::Of(Text{"tall", {}})}, {}, {}, std::nullopt}));
    TableCell right1;
    right1.blocks.push_back(
        Block::Of(Paragraph{{Inline::Of(Text{"x", {}})}, {}, {}, std::nullopt}));

    TableCell under;
    under.covered = true;
    TableCell right2;
    right2.blocks.push_back(
        Block::Of(Paragraph{{Inline::Of(Text{"y", {}})}, {}, {}, std::nullopt}));

    table.rows.push_back(TableRow{{std::move(tall), std::move(right1)}});
    table.rows.push_back(TableRow{{std::move(under), std::move(right2)}});
    document.blocks.push_back(Block::Of(std::move(table)));

    const std::vector<std::byte> bytes = Write(document);
    const std::string xml = DocumentXml(bytes);
    CHECK(xml.find(R"(<w:vMerge w:val="restart"/>)") != std::string::npos);
    CHECK(xml.find("<w:vMerge/>") != std::string::npos);

    const Document back = Read(bytes);
    const auto* out = back.blocks[0].As<Table>();
    REQUIRE(out != nullptr);
    REQUIRE(out->rows.size() == 2);
    CHECK(out->rows[0].cells[0].rowSpan == 2);
    CHECK(out->rows[1].cells[0].covered);
    CHECK(PlainText(out->rows[1].cells[1].blocks) == "y\n");
}

TEST_CASE("an empty cell still contains a paragraph", "[docx][write]") {
    // Word reports a document with an empty w:tc as corrupt.
    Document document;
    Table table;
    table.columns.resize(1);
    table.rows.push_back(TableRow{{TableCell{}}});
    document.blocks.push_back(Block::Of(std::move(table)));

    const std::string xml = DocumentXml(Write(document));
    CHECK(xml.find("<w:tcPr></w:tcPr><w:p/>") != std::string::npos);
}

TEST_CASE("a table is followed by a paragraph", "[docx][write]") {
    // A table at the very end of the body makes the document unopenable.
    const std::string xml = DocumentXml(Write(FromMarkdown("| A |\n| - |\n| 1 |\n")));
    const std::size_t table_end = xml.rfind("</w:tbl>");
    REQUIRE(table_end != std::string::npos);
    CHECK(xml.find("<w:p/>", table_end) != std::string::npos);
}

// --- Images ------------------------------------------------------------------

TEST_CASE("an embedded image becomes a media part and a drawing",
          "[docx][write][images]") {
    Document document;
    const ResourceId id = document.res.Add(FakePng(), "image/png", "shot.png");
    document.blocks.push_back(Block::Of(ImageBlock{id, "A screenshot", "", 0, 0}));

    const std::vector<std::byte> bytes = Write(document);

    opc::Package package;
    REQUIRE(opc::Package::Open(bytes, package).ok());

    bool has_media = false;
    for (const auto& [name, part] : package.Parts()) {
        has_media |= name.starts_with("/word/media/");
    }
    CHECK(has_media);
    CHECK(package.FindRelationshipByType("/word/document.xml", opc::rel::kImage) != nullptr);

    const std::string xml = DocumentXml(bytes);
    CHECK(xml.find("<w:drawing>") != std::string::npos);
    CHECK(xml.find(R"(descr="A screenshot")") != std::string::npos);
}

TEST_CASE("an image round-trips with its alt text", "[docx][write][roundtrip]") {
    Document document;
    const ResourceId id = document.res.Add(FakePng(), "image/png", "shot.png");
    document.blocks.push_back(Block::Of(ImageBlock{id, "alt text", "", 0, 0}));

    const Document back = RoundTrip(document);
    CHECK(back.res.Size() == 1);

    const InlineImage* image = nullptr;
    ForEachBlock(back.blocks, [&](const Block& block) {
        if (const auto* p = block.As<Paragraph>()) {
            ForEachInline(p->inlines, [&](const Inline& node) {
                if (node.As<InlineImage>()) image = node.As<InlineImage>();
            });
        }
    });
    REQUIRE(image != nullptr);
    CHECK(image->alt == "alt text");
    // The intrinsic size, read from the PNG header, is used when the IR carries
    // no display size: 96px at 96 DPI is 72pt.
    CHECK(image->widthPt == 72.0f);
}

TEST_CASE("an image referenced by URL is reported, not silently lost",
          "[docx][write][fidelity]") {
    // Markdown addresses images by URL and we hold no bytes for them. Section 1
    // is local only, so downloading is not an option.
    Document document;
    document.blocks.push_back(
        Block::Of(ImageBlock{"https://example.com/a.png", "remote", "", 0, 0}));

    std::vector<std::byte> bytes;
    const Result result = WriteDocx(document, {}, bytes);
    REQUIRE(result.ok());

    bool noted = false;
    for (const FidelityNote& note : result.notes()) noted |= note.code == "linked-image-dropped";
    CHECK(noted);
}

// --- Links -------------------------------------------------------------------

TEST_CASE("a hyperlink becomes an external relationship", "[docx][write][roundtrip]") {
    const Document back = RoundTrip(FromMarkdown("A [link](https://example.com) here.\n"));

    const Link* link = nullptr;
    ForEachBlock(back.blocks, [&](const Block& block) {
        if (const auto* p = block.As<Paragraph>()) {
            ForEachInline(p->inlines, [&](const Inline& node) {
                if (node.As<Link>()) link = node.As<Link>();
            });
        }
    });
    REQUIRE(link != nullptr);
    CHECK(link->target == "https://example.com");
    CHECK(PlainText(back.blocks) == "A link here.\n");
}

TEST_CASE("the same link target is one relationship", "[docx][write]") {
    const std::vector<std::byte> bytes =
        Write(FromMarkdown("[a](https://example.com) and [b](https://example.com)\n"));

    opc::Package package;
    REQUIRE(opc::Package::Open(bytes, package).ok());

    std::size_t links = 0;
    for (const opc::Relationship& relationship :
         package.RelationshipsOf("/word/document.xml")) {
        links += relationship.type == opc::rel::kHyperlink ? 1 : 0;
    }
    CHECK(links == 1);
}

// --- Other blocks ------------------------------------------------------------

TEST_CASE("a code block becomes one paragraph per line", "[docx][write]") {
    // A single paragraph with breaks also renders, but Word then treats the
    // whole listing as one block for page-break purposes and a long one refuses
    // to split across pages.
    const std::string xml = DocumentXml(Write(FromMarkdown("```\nline one\nline two\n```\n")));

    std::size_t code_paragraphs = 0;
    for (std::size_t at = 0;
         (at = xml.find(R"(<w:pStyle w:val="SourceCode"/>)", at)) != std::string::npos; ++at) {
        ++code_paragraphs;
    }
    CHECK(code_paragraphs == 2);
}

TEST_CASE("a block quote uses the Quote style", "[docx][write]") {
    const std::string xml = DocumentXml(Write(FromMarkdown("> quoted\n")));
    CHECK(xml.find(R"(<w:pStyle w:val="Quote"/>)") != std::string::npos);
}

TEST_CASE("a horizontal rule becomes a bordered paragraph", "[docx][write]") {
    // Word has no horizontal rule element; this is what Word itself produces.
    const std::string xml = DocumentXml(Write(FromMarkdown("---\n")));
    CHECK(xml.find("<w:pBdr>") != std::string::npos);
}

TEST_CASE("metadata becomes core properties", "[docx][write][roundtrip]") {
    Document document;
    document.meta.title = "The Title";
    document.meta.authors = {"An Author"};
    document.meta.lang = "en-GB";
    document.blocks.push_back(
        Block::Of(Paragraph{{Inline::Of(Text{"x", {}})}, {}, {}, std::nullopt}));

    const Document back = RoundTrip(document);
    CHECK(back.meta.title == "The Title");
    REQUIRE(back.meta.authors.size() == 1);
    CHECK(back.meta.authors[0] == "An Author");
    CHECK(back.meta.lang == "en-GB");
}

TEST_CASE("no creation timestamp is written", "[docx][write][determinism]") {
    // A timestamp would break byte reproducibility, and the creation date of a
    // conversion is not a meaningful fact about the document.
    Document document;
    document.meta.title = "x";
    document.blocks.push_back(
        Block::Of(Paragraph{{Inline::Of(Text{"x", {}})}, {}, {}, std::nullopt}));

    opc::Package package;
    REQUIRE(opc::Package::Open(Write(document), package).ok());
    const opc::Part* core = package.Find("/docProps/core.xml");
    REQUIRE(core != nullptr);
    const std::string xml(reinterpret_cast<const char*>(core->bytes.data()), core->bytes.size());
    CHECK(xml.find("dcterms:created") == std::string::npos);
}

TEST_CASE("control characters are stripped rather than written", "[docx][write][security]") {
    // XML 1.0 forbids most of them, and emitting one produces a file Word
    // refuses to open.
    Document document;
    document.blocks.push_back(Block::Of(
        Paragraph{{Inline::Of(Text{std::string("before\x01" "\x02" "after"), {}})}, {}, {}, std::nullopt}));

    const Document back = RoundTrip(document);
    CHECK(PlainText(back.blocks) == "beforeafter\n");
}

// --- The whole point ---------------------------------------------------------

TEST_CASE("Markdown reaches DOCX through the IR with no direct converter",
          "[docx][write][roundtrip]") {
    const Document original = FromMarkdown(
        "# Report\n"
        "\n"
        "Text with **bold** and a [link](https://example.com).\n"
        "\n"
        "- first\n"
        "- second\n"
        "\n"
        "| A | B |\n"
        "| - | - |\n"
        "| 1 | 2 |\n"
        "\n"
        "> A quotation.\n"
        "\n"
        "```c\n"
        "int x = 1;\n"
        "```\n");

    const Document back = RoundTrip(original);

    // Structure survives.
    CHECK(back.blocks[0].As<Heading>() != nullptr);
    bool has_list = false, has_table = false, has_code = false;
    ForEachBlock(back.blocks, [&](const Block& block) {
        has_list |= block.Is<ListBlock>();
        has_table |= block.Is<Table>();
        has_code |= block.Is<CodeBlock>();
    });
    CHECK(has_list);
    CHECK(has_table);
    CHECK(has_code);

    // And so does every word.
    const std::string text = PlainText(back.blocks);
    for (const std::string_view word :
         {"Report", "bold", "link", "first", "second", "quotation", "int x = 1;"}) {
        INFO("looking for: " << word << " in\n" << text);
        CHECK(text.find(word) != std::string::npos);
    }
}

TEST_CASE("DOCX reaches HTML through the IR", "[docx][write][roundtrip]") {
    const Document d = RoundTrip(FromMarkdown("# Title\n\nSome **text**.\n"));

    std::string html;
    REQUIRE(morph::dochtml::WriteHtml(d, {}, html).ok());
    CHECK(html.find("<h1") != std::string::npos);
    CHECK(html.find("<strong>text</strong>") != std::string::npos);
}

// --- Round-trip asymmetries found by running the real CLI --------------------
//
// Each of these was invisible to the component tests: the reader tests passed,
// the writer tests passed, and the pipeline still lost content. They are the
// argument for testing what the product actually does, not only what its parts
// do individually.

TEST_CASE("a space between differently formatted runs survives", "[docx][regression]") {
    // pugixml DISCARDS a whitespace-only text node unless asked not to, and Word
    // writes exactly that whenever a space falls between two runs with different
    // formatting. Without parse_ws_pcdata_single the space vanished and
    // "and a [link]" came back as "and a[link]" -- a corruption that only shows
    // up across a formatting boundary, which is why no earlier test saw it.
    const Document back = RoundTrip(FromMarkdown(
        "Text with **bold** and a [link](https://example.com) after it.\n"));

    const std::string text = PlainText(back.blocks);
    INFO(text);
    CHECK(text.find("with bold and a link after it") != std::string::npos);
}

TEST_CASE("a whitespace-only run is not dropped", "[docx][regression]") {
    // The same bug, isolated to one run.
    Document document;
    Paragraph paragraph;
    paragraph.inlines.push_back(Inline::Of(Strong{{Inline::Of(Text{"one", {}})}}));
    paragraph.inlines.push_back(Inline::Of(Text{" ", {}}));
    paragraph.inlines.push_back(Inline::Of(Strong{{Inline::Of(Text{"two", {}})}}));
    document.blocks.push_back(Block::Of(std::move(paragraph)));

    CHECK(PlainText(RoundTrip(document).blocks) == "one two\n");
}

TEST_CASE("a block quote round-trips as a block quote", "[docx][regression]") {
    // Word has no block quote element, so the writer emits Quote-styled
    // paragraphs. A reader that does not recognise the style back degrades every
    // quotation into ordinary prose -- the same class of bug as code blocks.
    const Document back = RoundTrip(FromMarkdown("> A quotation that matters.\n"));

    REQUIRE(back.blocks.size() == 1);
    const auto* quote = back.blocks[0].As<BlockQuote>();
    REQUIRE(quote != nullptr);
    CHECK(PlainText(quote->blocks) == "A quotation that matters.\n");
}

TEST_CASE("a quote does not swallow the prose after it", "[docx][regression]") {
    const Document back = RoundTrip(FromMarkdown("> Quoted.\n\nNot quoted.\n"));

    REQUIRE(back.blocks.size() == 2);
    CHECK(back.blocks[0].Is<BlockQuote>());
    CHECK(back.blocks[1].Is<Paragraph>());
    CHECK(PlainText({back.blocks[1]}) == "Not quoted.\n");
}

TEST_CASE("a dropped code language is reported", "[docx][fidelity]") {
    // Word has nowhere to record it, so the loss is unavoidable -- which makes
    // naming it the fidelity contract's entire purpose.
    std::vector<std::byte> bytes;
    const Result result = WriteDocx(FromMarkdown("```cpp\nint x;\n```\n"), {}, bytes);

    REQUIRE(result.ok());
    bool noted = false;
    for (const FidelityNote& note : result.notes()) noted |= note.code == "code-language-dropped";
    CHECK(noted);

    // And a listing with no language claims no loss.
    std::vector<std::byte> plain;
    const Result plainResult = WriteDocx(FromMarkdown("```\nint x;\n```\n"), {}, plain);
    REQUIRE(plainResult.ok());
    for (const FidelityNote& note : plainResult.notes()) {
        CHECK(note.code != "code-language-dropped");
    }
}

TEST_CASE("A code span survives a Word round trip", "[docx]") {
    // WordprocessingML has no code-span element, so the writer marks the run
    // with a character style. Marking it only with a monospace FONT is a guess
    // the reader cannot safely act on, and without the style `x` came back as
    // ordinary text in Consolas -- lost the moment it was written anywhere else.
    Document document;
    Paragraph paragraph;
    paragraph.inlines.push_back(Inline::Of(Text{"call ", {}}));
    paragraph.inlines.push_back(Inline::Of(InlineCode{"main()"}));
    document.blocks.push_back(Block::Of(std::move(paragraph)));

    const std::vector<std::byte> package = Write(document);
    CHECK(DocumentXml(package).find(R"(<w:rStyle w:val="HTMLCode"/>)") != std::string::npos);

    const Document back = Read(package);
    const Inlines& inlines = back.blocks.front().As<Paragraph>()->inlines;
    REQUIRE(inlines.size() == 2);
    REQUIRE(inlines[1].Is<InlineCode>());
    CHECK(inlines[1].As<InlineCode>()->text == "main()");
}

TEST_CASE("A code span inside a link stays a code span", "[docx]") {
    // A run carries ONE character style. Emitting both the hyperlink style and
    // the code style produced two w:rStyle elements, which is not valid and
    // which a reader resolves by taking the first -- so the code span silently
    // became link text.
    Document document;
    Paragraph paragraph;
    Link link;
    link.target = "https://example.com/";
    link.children.push_back(Inline::Of(InlineCode{"code"}));
    paragraph.inlines.push_back(Inline::Of(std::move(link)));
    document.blocks.push_back(Block::Of(std::move(paragraph)));

    const Document back = RoundTrip(document);
    const Inlines& inlines = back.blocks.front().As<Paragraph>()->inlines;
    REQUIRE(inlines.size() == 1);
    REQUIRE(inlines[0].Is<Link>());
    const Inlines& inner = inlines[0].As<Link>()->children;
    REQUIRE(inner.size() == 1);
    CHECK(inner[0].Is<InlineCode>());
}

TEST_CASE("A horizontal rule survives a Word round trip", "[docx]") {
    // The writer has always emitted Word's own spelling for a rule -- an empty
    // paragraph with a bottom border -- and the reader had no ear for it, so
    // every rule became an empty paragraph and vanished.
    Document document;
    document.blocks.push_back(Block::Of(Paragraph{{Inline::Of(Text{"above", {}})}, {}, {}, {}}));
    document.blocks.push_back(Block::Of(HorizontalRule{}));
    document.blocks.push_back(Block::Of(Paragraph{{Inline::Of(Text{"below", {}})}, {}, {}, {}}));

    const Document back = RoundTrip(document);
    REQUIRE(back.blocks.size() == 3);
    CHECK(back.blocks[1].Is<HorizontalRule>());
}

TEST_CASE("An item whose only content is a list keeps its place", "[docx]") {
    // The flat encoding needs a numbered paragraph to say "this item exists",
    // and it needs it BEFORE the nested list's paragraphs. Emitted afterwards,
    // the nested list attaches to the previous item on the way back in and the
    // empty paragraph reads as a trailing empty item.
    ListBlock inner;
    inner.ordered = false;
    inner.items.push_back(
        ListItem{{Block::Of(Paragraph{{Inline::Of(Text{"deep", {}})}, {}, {}, {}})}, {}});

    ListBlock outer;
    outer.ordered = false;
    outer.items.push_back(
        ListItem{{Block::Of(Paragraph{{Inline::Of(Text{"first", {}})}, {}, {}, {}})}, {}});
    outer.items.push_back(ListItem{{Block::Of(std::move(inner))}, {}});

    Document document;
    document.blocks.push_back(Block::Of(std::move(outer)));

    const Document back = RoundTrip(document);
    REQUIRE(back.blocks.size() == 1);
    const ListBlock* list = back.blocks[0].As<ListBlock>();
    REQUIRE(list != nullptr);
    REQUIRE(list->items.size() == 2);
    CHECK(list->items[1].blocks.size() == 1);
    CHECK(list->items[1].blocks[0].Is<ListBlock>());
}
