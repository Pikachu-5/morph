// The DOCX reader (architecture section 8.4), P1 and P2.
//
// Organised around section 8.4's four tricky mappings, because those are where
// the bugs live and a test file organised any other way makes it hard to tell
// whether they are all covered.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "docx_fixture.h"
#include "morph/providers/docx.h"
#include "morph/providers/markdown.h"

using namespace morph;
using namespace morph::doc;
using morph::docooxml::ReadDocx;
using morph::test::BuildDocx;
using morph::test::DocxParts;
using morph::test::Para;

namespace {

Document Read(const DocxParts& parts) {
    Document document;
    const Result result = ReadDocx(BuildDocx(parts), {}, document);
    INFO("reader said: " << result.message());
    REQUIRE(result.ok());
    return document;
}

Result ReadWithResult(const DocxParts& parts, Document& into) {
    return ReadDocx(BuildDocx(parts), {}, into);
}

DocxParts WithBody(std::string body) {
    DocxParts parts;
    parts.body = std::move(body);
    return parts;
}

// Counts inline nodes of a given kind anywhere in the document.
template <typename T>
std::size_t CountInlines(const Document& document) {
    std::size_t count = 0;
    ForEachBlock(document.blocks, [&](const Block& block) {
        const Inlines* inlines = nullptr;
        if (const auto* p = block.As<Paragraph>()) inlines = &p->inlines;
        if (const auto* h = block.As<Heading>()) inlines = &h->inlines;
        if (inlines == nullptr) return;
        ForEachInline(*inlines, [&](const Inline& node) { count += node.Is<T>() ? 1 : 0; });
    });
    return count;
}

std::vector<std::byte> FakePng() {
    const std::string png = "\x89PNG\r\n\x1a\n" + std::string(64, 'x');
    return std::vector<std::byte>(reinterpret_cast<const std::byte*>(png.data()),
                                  reinterpret_cast<const std::byte*>(png.data()) + png.size());
}

}  // namespace

// --- P1: paragraphs and runs -------------------------------------------------

TEST_CASE("a plain document reads as paragraphs", "[docx][p1]") {
    const Document d = Read(WithBody(Para("First.") + Para("Second.")));
    REQUIRE(d.blocks.size() == 2);
    CHECK(PlainText(d.blocks) == "First.\nSecond.\n");
}

TEST_CASE("adjacent runs with identical formatting are merged", "[docx][p1][mapping1]") {
    // Mapping 1. Word splits a sentence into arbitrarily many w:r for reasons
    // unrelated to formatting -- spell-check state, revision marks, rsids -- and
    // without merging, a five-word paragraph becomes eleven IR nodes.
    const Document d = Read(WithBody(
        "<w:p>"
        "<w:r><w:t>The </w:t></w:r>"
        "<w:r><w:t>quick </w:t></w:r>"
        "<w:r><w:t>brown </w:t></w:r>"
        "<w:r><w:t>fox</w:t></w:r>"
        "</w:p>"));

    REQUIRE(d.blocks.size() == 1);
    const auto* paragraph = d.blocks[0].As<Paragraph>();
    REQUIRE(paragraph != nullptr);
    CHECK(paragraph->inlines.size() == 1);
    CHECK(paragraph->inlines[0].As<Text>()->text == "The quick brown fox");
}

TEST_CASE("runs with different formatting are not merged", "[docx][p1][mapping1]") {
    const Document d = Read(WithBody(
        "<w:p>"
        "<w:r><w:t>plain </w:t></w:r>"
        "<w:r><w:rPr><w:b/></w:rPr><w:t>bold</w:t></w:r>"
        "<w:r><w:t> plain</w:t></w:r>"
        "</w:p>"));

    const auto* paragraph = d.blocks[0].As<Paragraph>();
    REQUIRE(paragraph != nullptr);
    CHECK(paragraph->inlines.size() == 3);
    CHECK(CountInlines<Strong>(d) == 1);
    CHECK(PlainText(d.blocks) == "plain bold plain\n");
}

TEST_CASE("bold and italic become semantic nodes, not styles", "[docx][p1]") {
    // Every target format has a construct for them, so a Markdown writer should
    // emit **bold** rather than a span carrying a weight.
    const Document d = Read(WithBody(
        "<w:p>"
        "<w:r><w:rPr><w:b/><w:i/></w:rPr><w:t>both</w:t></w:r>"
        "<w:r><w:rPr><w:strike/></w:rPr><w:t>gone</w:t></w:r>"
        "</w:p>"));

    CHECK(CountInlines<Strong>(d) == 1);
    CHECK(CountInlines<Emphasis>(d) == 1);
    CHECK(CountInlines<Strikethrough>(d) == 1);
}

TEST_CASE("an explicit off value switches formatting off", "[docx][p1]") {
    // <w:b w:val="0"/> is not the same as an absent <w:b/>, and treating a
    // present element as unconditionally true is a classic OOXML bug.
    const Document d = Read(WithBody(
        R"(<w:p><w:r><w:rPr><w:b w:val="0"/></w:rPr><w:t>not bold</w:t></w:r></w:p>)"));
    CHECK(CountInlines<Strong>(d) == 0);
}

TEST_CASE("font size is read as half-points", "[docx][p1][units]") {
    // w:sz val="22" is 11pt. Reading it as 22 is the single most common OOXML
    // unit mistake and produces documents that look almost right.
    const Document d = Read(WithBody(
        R"(<w:p><w:r><w:rPr><w:sz w:val="22"/></w:rPr><w:t>eleven point</w:t></w:r></w:p>)"));

    const auto* paragraph = d.blocks[0].As<Paragraph>();
    REQUIRE(paragraph != nullptr);
    const auto* text = paragraph->inlines[0].As<Text>();
    REQUIRE(text != nullptr);
    REQUIRE(text->style.fontSizePt.has_value());
    CHECK(*text->style.fontSizePt == 11.0f);
}

TEST_CASE("indents are read as twips", "[docx][p1][units]") {
    // 720 twips is 36 points is half an inch.
    const Document d = Read(WithBody(
        R"(<w:p><w:pPr><w:ind w:left="720"/></w:pPr><w:r><w:t>indented</w:t></w:r></w:p>)"));

    const auto* paragraph = d.blocks[0].As<Paragraph>();
    REQUIRE(paragraph != nullptr);
    REQUIRE(paragraph->direct.indentLeftPt.has_value());
    CHECK(*paragraph->direct.indentLeftPt == 36.0f);
}

TEST_CASE("an auto colour stays unset", "[docx][p1]") {
    // Design rule 2: an unset optional means "not specified". Turning "auto"
    // into black invents a colour the document never chose.
    const Document d = Read(WithBody(
        R"(<w:p><w:r><w:rPr><w:color w:val="auto"/></w:rPr><w:t>x</w:t></w:r></w:p>)"));

    const auto* text = d.blocks[0].As<Paragraph>()->inlines[0].As<Text>();
    REQUIRE(text != nullptr);
    CHECK_FALSE(text->style.colour.has_value());
}

TEST_CASE("empty paragraphs are dropped", "[docx][p1]") {
    // Word documents are full of them as vertical spacing, and every one becomes
    // a stray blank line downstream.
    const Document d = Read(WithBody(Para("real") + "<w:p/>" + "<w:p><w:pPr/></w:p>" +
                                     Para("also real")));
    CHECK(d.blocks.size() == 2);
}

TEST_CASE("tabs and breaks survive", "[docx][p1]") {
    const Document d = Read(WithBody(
        R"(<w:p><w:r><w:t>a</w:t><w:tab/><w:t>b</w:t><w:br/><w:t>c</w:t></w:r></w:p>)"));
    CHECK(PlainText(d.blocks).find("a\tb") != std::string::npos);
    CHECK(CountInlines<LineBreak>(d) == 1);
}

// --- P1: headings ------------------------------------------------------------

TEST_CASE("a heading style becomes a Heading", "[docx][p1][headings]") {
    DocxParts parts;
    parts.styles =
        R"(<w:style w:type="paragraph" w:styleId="Heading1"><w:name w:val="heading 1"/></w:style>)"
        R"(<w:style w:type="paragraph" w:styleId="Heading2"><w:name w:val="heading 2"/></w:style>)";
    parts.body =
        R"(<w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr><w:r><w:t>Title</w:t></w:r></w:p>)"
        R"(<w:p><w:pPr><w:pStyle w:val="Heading2"/></w:pPr><w:r><w:t>Sub</w:t></w:r></w:p>)" +
        Para("body");

    const Document d = Read(parts);
    REQUIRE(d.blocks.size() == 3);
    REQUIRE(d.blocks[0].As<Heading>() != nullptr);
    CHECK(d.blocks[0].As<Heading>()->level == 1);
    CHECK(d.blocks[0].As<Heading>()->anchorId == "title");
    CHECK(d.blocks[1].As<Heading>()->level == 2);
    CHECK(d.blocks[2].Is<Paragraph>());
}

TEST_CASE("an explicit outline level beats the style name", "[docx][p1][headings]") {
    // The author's own statement of intent, and the only reliable signal in a
    // document with renamed styles.
    DocxParts parts;
    parts.styles =
        R"(<w:style w:type="paragraph" w:styleId="MyBigTitle"><w:name w:val="My Big Title"/>)"
        R"(<w:pPr><w:outlineLvl w:val="0"/></w:pPr></w:style>)";
    parts.body =
        R"(<w:p><w:pPr><w:pStyle w:val="MyBigTitle"/></w:pPr><w:r><w:t>Custom</w:t></w:r></w:p>)";

    const Document d = Read(parts);
    REQUIRE(d.blocks[0].As<Heading>() != nullptr);
    CHECK(d.blocks[0].As<Heading>()->level == 1);
}

TEST_CASE("a heading level is found through the basedOn chain", "[docx][p1][headings]") {
    DocxParts parts;
    parts.styles =
        R"(<w:style w:type="paragraph" w:styleId="Heading2"><w:name w:val="heading 2"/></w:style>)"
        R"(<w:style w:type="paragraph" w:styleId="MyHeading"><w:name w:val="My Heading"/>)"
        R"(<w:basedOn w:val="Heading2"/></w:style>)";
    parts.body =
        R"(<w:p><w:pPr><w:pStyle w:val="MyHeading"/></w:pPr><w:r><w:t>Derived</w:t></w:r></w:p>)";

    const Document d = Read(parts);
    REQUIRE(d.blocks[0].As<Heading>() != nullptr);
    CHECK(d.blocks[0].As<Heading>()->level == 2);
}

// --- P1: style resolution ----------------------------------------------------

TEST_CASE("docDefaults, basedOn and direct formatting resolve in the right order",
          "[docx][p1][styles]") {
    // Design rule 2, end to end. Three layers, each contributing a different
    // property, with direct formatting winning where they overlap.
    DocxParts parts;
    parts.styles =
        R"(<w:docDefaults><w:rPrDefault><w:rPr>)"
        R"(<w:rFonts w:ascii="Calibri"/><w:sz w:val="20"/>)"
        R"(</w:rPr></w:rPrDefault></w:docDefaults>)"
        R"(<w:style w:type="paragraph" w:styleId="Base"><w:name w:val="Base"/>)"
        R"(<w:rPr><w:sz w:val="24"/><w:color w:val="FF0000"/></w:rPr></w:style>)"
        R"(<w:style w:type="paragraph" w:styleId="Derived"><w:name w:val="Derived"/>)"
        R"(<w:basedOn w:val="Base"/><w:rPr><w:color w:val="00FF00"/></w:rPr></w:style>)";
    parts.body =
        R"(<w:p><w:pPr><w:pStyle w:val="Derived"/></w:pPr>)"
        R"(<w:r><w:rPr><w:sz w:val="28"/></w:rPr><w:t>x</w:t></w:r></w:p>)";

    const Document d = Read(parts);

    // WHERE EACH LAYER ENDS UP. A run carries only what is not already implied
    // by the block it sits in; the paragraph's style is reachable through
    // styleRef, and docDefaults through StyleTable.
    //
    // Folding all three onto every run would be the obvious reading of "styles
    // resolved on read", and it is wrong in practice: the document's default
    // font and size would land on every single run, so the HTML writer would
    // wrap every word in a span and the Markdown writer would report dropped
    // character formatting for a document that has none.
    const auto* paragraph = d.blocks[0].As<Paragraph>();
    REQUIRE(paragraph != nullptr);
    const auto* text = paragraph->inlines[0].As<Text>();
    REQUIRE(text != nullptr);

    // Direct formatting is the run's own, so it is on the run.
    REQUIRE(text->style.fontSizePt.has_value());
    CHECK(*text->style.fontSizePt == 14.0f);
    // The style's colour and the default font are not.
    CHECK_FALSE(text->style.colour.has_value());
    CHECK_FALSE(text->style.fontFamily.has_value());

    // They are reachable through the paragraph's style, already flattened, with
    // the nearest style winning the colour over its basedOn parent and
    // docDefaults supplying what nothing else did.
    CHECK(paragraph->styleRef == "Derived");
    const NamedStyle* style = d.styles.Find(paragraph->styleRef);
    REQUIRE(style != nullptr);
    REQUIRE(style->paragraph.text.colour.has_value());
    CHECK(style->paragraph.text.colour->ToHex() == "#00ff00");
    REQUIRE(style->paragraph.text.fontFamily.has_value());
    CHECK(*style->paragraph.text.fontFamily == "Calibri");

    // And the document default is a question the IR can answer directly.
    REQUIRE(d.styles.Default().text.fontFamily.has_value());
    CHECK(*d.styles.Default().text.fontFamily == "Calibri");
}

TEST_CASE("a character style still resolves onto its run", "[docx][p1][styles]") {
    // The chain a run DOES walk: w:rStyle and its basedOn ancestors. This is
    // direct formatting by another name -- the author applied it to this text,
    // not to the paragraph.
    DocxParts parts;
    parts.styles =
        R"(<w:style w:type="character" w:styleId="CodeBase"><w:name w:val="Code Base"/>)"
        R"(<w:rPr><w:rFonts w:ascii="Consolas"/><w:color w:val="FF0000"/></w:rPr></w:style>)"
        R"(<w:style w:type="character" w:styleId="CodeEmph"><w:name w:val="Code Emphasis"/>)"
        R"(<w:basedOn w:val="CodeBase"/><w:rPr><w:i/><w:color w:val="0000FF"/></w:rPr></w:style>)";
    parts.body =
        R"(<w:p><w:r><w:rPr><w:rStyle w:val="CodeEmph"/></w:rPr><w:t>styled</w:t></w:r></w:p>)";

    const Document d = Read(parts);

    // Italic came from the character style, so it is a semantic node.
    std::size_t emphasis = 0;
    ForEachInline(d.blocks[0].As<Paragraph>()->inlines,
                  [&](const Inline& node) { emphasis += node.Is<Emphasis>() ? 1 : 0; });
    CHECK(emphasis == 1);

    const Text* text = nullptr;
    ForEachInline(d.blocks[0].As<Paragraph>()->inlines,
                  [&](const Inline& node) { if (node.As<Text>()) text = node.As<Text>(); });
    REQUIRE(text != nullptr);
    // The nearest character style wins the colour; its parent supplies the font.
    REQUIRE(text->style.colour.has_value());
    CHECK(text->style.colour->ToHex() == "#0000ff");
    REQUIRE(text->style.fontFamily.has_value());
    CHECK(*text->style.fontFamily == "Consolas");
}

TEST_CASE("a basedOn cycle is truncated rather than followed", "[docx][p1][styles]") {
    // Hand-edited and hostile styles.xml files contain these, and Word tolerates
    // them. Following one is a hang.
    DocxParts parts;
    parts.styles =
        R"(<w:style w:type="paragraph" w:styleId="A"><w:basedOn w:val="B"/></w:style>)"
        R"(<w:style w:type="paragraph" w:styleId="B"><w:basedOn w:val="A"/></w:style>)";
    parts.body = R"(<w:p><w:pPr><w:pStyle w:val="A"/></w:pPr><w:r><w:t>x</w:t></w:r></w:p>)";

    const Document d = Read(parts);
    CHECK(PlainText(d.blocks) == "x\n");
}

TEST_CASE("the flattened style table reaches the IR", "[docx][p1][styles]") {
    DocxParts parts;
    parts.styles =
        R"(<w:style w:type="paragraph" w:styleId="Quote"><w:name w:val="Quote"/>)"
        R"(<w:pPr><w:ind w:left="720"/></w:pPr></w:style>)";
    parts.body = Para("x");

    const Document d = Read(parts);
    const NamedStyle* style = d.styles.Find("Quote");
    REQUIRE(style != nullptr);
    CHECK(style->name == "Quote");
    REQUIRE(style->paragraph.indentLeftPt.has_value());
    CHECK(*style->paragraph.indentLeftPt == 36.0f);
}

// --- P1: hyperlinks ----------------------------------------------------------

TEST_CASE("an external hyperlink resolves through the relationship graph",
          "[docx][p1][links]") {
    DocxParts parts;
    parts.documentRelationships.push_back(
        opc::Relationship{"rIdLink", std::string(opc::rel::kHyperlink), "https://example.com",
                          true});
    parts.body =
        R"(<w:p><w:hyperlink r:id="rIdLink" w:tooltip="Go there">)"
        R"(<w:r><w:t>click me</w:t></w:r></w:hyperlink></w:p>)";

    const Document d = Read(parts);
    REQUIRE(CountInlines<Link>(d) == 1);

    const Link* link = nullptr;
    ForEachInline(d.blocks[0].As<Paragraph>()->inlines,
                  [&](const Inline& node) { if (node.As<Link>()) link = node.As<Link>(); });
    REQUIRE(link != nullptr);
    CHECK(link->target == "https://example.com");
    CHECK(link->title == "Go there");
    CHECK(PlainText(d.blocks) == "click me\n");
}

TEST_CASE("an internal hyperlink becomes an anchor link", "[docx][p1][links]") {
    const Document d = Read(WithBody(
        R"(<w:p><w:hyperlink w:anchor="section2"><w:r><w:t>jump</w:t></w:r></w:hyperlink></w:p>)"));

    const Link* link = nullptr;
    ForEachInline(d.blocks[0].As<Paragraph>()->inlines,
                  [&](const Inline& node) { if (node.As<Link>()) link = node.As<Link>(); });
    REQUIRE(link != nullptr);
    CHECK(link->target == "#section2");
}

TEST_CASE("a hyperlink with no destination is just text", "[docx][p1][links]") {
    const Document d = Read(WithBody(
        R"(<w:p><w:hyperlink><w:r><w:t>not a link</w:t></w:r></w:hyperlink></w:p>)"));
    CHECK(CountInlines<Link>(d) == 0);
    CHECK(PlainText(d.blocks) == "not a link\n");
}

// --- P1: images --------------------------------------------------------------

TEST_CASE("an embedded image lands in the resource store", "[docx][p1][images]") {
    DocxParts parts;
    parts.media.push_back({"rIdImg", "image1.png", "image/png", FakePng()});
    parts.body =
        R"(<w:p><w:r><w:drawing><wp:inline>)"
        R"(<wp:extent cx="914400" cy="457200"/>)"
        R"(<wp:docPr id="1" name="Picture 1" descr="A diagram"/>)"
        R"(<a:graphic><a:graphicData><pic:pic><pic:blipFill>)"
        R"(<a:blip r:embed="rIdImg"/>)"
        R"(</pic:blipFill></pic:pic></a:graphicData></a:graphic>)"
        R"(</wp:inline></w:drawing></w:r></w:p>)";

    const Document d = Read(parts);
    CHECK(d.res.Size() == 1);
    REQUIRE(CountInlines<InlineImage>(d) == 1);

    const InlineImage* image = nullptr;
    ForEachInline(d.blocks[0].As<Paragraph>()->inlines, [&](const Inline& node) {
        if (node.As<InlineImage>()) image = node.As<InlineImage>();
    });
    REQUIRE(image != nullptr);
    CHECK(image->alt == "A diagram");
    // 914400 EMU is one inch is 72 points.
    CHECK(image->widthPt == 72.0f);
    CHECK(image->heightPt == 36.0f);
    CHECK(d.res.Contains(image->resource));
}

TEST_CASE("the same image used twice is stored once", "[docx][p1][images]") {
    // Content addressing (section 8.2). A logo in a header on forty pages
    // arrives forty times.
    DocxParts parts;
    parts.media.push_back({"rIdA", "image1.png", "image/png", FakePng()});
    parts.media.push_back({"rIdB", "image2.png", "image/png", FakePng()});

    const std::string drawing =
        R"(<w:r><w:drawing><wp:inline><wp:docPr id="1" name="p"/>)"
        R"(<a:graphic><a:graphicData><pic:pic><pic:blipFill><a:blip r:embed=")";
    parts.body = "<w:p>" + drawing + R"(rIdA"/></pic:blipFill></pic:pic></a:graphicData>)"
                 R"(</a:graphic></wp:inline></w:drawing></w:r>)" +
                 drawing + R"(rIdB"/></pic:blipFill></pic:pic></a:graphicData>)"
                 R"(</a:graphic></wp:inline></w:drawing></w:r></w:p>)";

    const Document d = Read(parts);
    CHECK(CountInlines<InlineImage>(d) == 2);
    CHECK(d.res.Size() == 1);
}

TEST_CASE("a chart is reported rather than silently dropped", "[docx][p1][images]") {
    Document d;
    DocxParts parts;
    parts.body =
        R"(<w:p><w:r><w:drawing><wp:inline><wp:docPr id="1" name="Chart"/>)"
        R"(<a:graphic><a:graphicData/></a:graphic></wp:inline></w:drawing></w:r></w:p>)";

    const Result result = ReadWithResult(parts, d);
    REQUIRE(result.ok());
    bool noted = false;
    for (const FidelityNote& note : result.notes()) noted |= note.code == "drawing-dropped";
    CHECK(noted);
    CHECK(result.incurred() == Fidelity::Structural);
}

// --- P2: lists (mapping 2) ---------------------------------------------------

namespace {

// One abstract list with three levels: bullet, bullet, bullet.
constexpr std::string_view kBulletNumbering =
    R"(<w:abstractNum w:abstractNumId="0">)"
    R"(<w:lvl w:ilvl="0"><w:numFmt w:val="bullet"/><w:lvlText w:val="-"/></w:lvl>)"
    R"(<w:lvl w:ilvl="1"><w:numFmt w:val="bullet"/><w:lvlText w:val="-"/></w:lvl>)"
    R"(<w:lvl w:ilvl="2"><w:numFmt w:val="bullet"/><w:lvlText w:val="-"/></w:lvl>)"
    R"(</w:abstractNum>)"
    R"(<w:abstractNum w:abstractNumId="1">)"
    R"(<w:lvl w:ilvl="0"><w:start w:val="1"/><w:numFmt w:val="decimal"/></w:lvl>)"
    R"(</w:abstractNum>)"
    R"(<w:num w:numId="1"><w:abstractNumId w:val="0"/></w:num>)"
    R"(<w:num w:numId="2"><w:abstractNumId w:val="1"/></w:num>)"
    R"(<w:num w:numId="3"><w:abstractNumId w:val="1"/>)"
    R"(<w:lvlOverride w:ilvl="0"><w:startOverride w:val="5"/></w:lvlOverride></w:num>)";

std::string ListItem_(std::string_view text, int numId, int level) {
    return R"(<w:p><w:pPr><w:numPr><w:ilvl w:val=")" + std::to_string(level) +
           R"("/><w:numId w:val=")" + std::to_string(numId) + R"("/></w:numPr></w:pPr>)"
           "<w:r><w:t>" + std::string(text) + "</w:t></w:r></w:p>";
}

}  // namespace

TEST_CASE("consecutive numbered paragraphs become one list", "[docx][p2][mapping2]") {
    // There is no list element in WordprocessingML; this is the reconstruction.
    DocxParts parts;
    parts.numbering = std::string(kBulletNumbering);
    parts.body = ListItem_("one", 1, 0) + ListItem_("two", 1, 0) + ListItem_("three", 1, 0);

    const Document d = Read(parts);
    REQUIRE(d.blocks.size() == 1);
    const auto* list = d.blocks[0].As<ListBlock>();
    REQUIRE(list != nullptr);
    CHECK_FALSE(list->ordered);
    CHECK(list->items.size() == 3);
    CHECK(PlainText(d.blocks) == "one\ntwo\nthree\n");
}

TEST_CASE("ilvl becomes nesting", "[docx][p2][mapping2]") {
    DocxParts parts;
    parts.numbering = std::string(kBulletNumbering);
    parts.body = ListItem_("outer", 1, 0) + ListItem_("inner", 1, 1) + ListItem_("back", 1, 0);

    const Document d = Read(parts);
    REQUIRE(d.blocks.size() == 1);
    const auto* list = d.blocks[0].As<ListBlock>();
    REQUIRE(list != nullptr);
    REQUIRE(list->items.size() == 2);

    // The nested list hangs off the first item.
    bool nested = false;
    for (const Block& block : list->items[0].blocks) nested |= block.Is<ListBlock>();
    CHECK(nested);
    CHECK(PlainText(d.blocks) == "outer\ninner\nback\n");
}

TEST_CASE("a level skip still produces a well-formed tree", "[docx][p2][mapping2]") {
    // A document can jump from ilvl 0 to ilvl 2. The intermediate list has to
    // exist or the tree is malformed.
    DocxParts parts;
    parts.numbering = std::string(kBulletNumbering);
    parts.body = ListItem_("top", 1, 0) + ListItem_("deep", 1, 2);

    const Document d = Read(parts);
    REQUIRE(d.blocks.size() == 1);
    CHECK(MaxDepth(d.blocks) >= 3);
    CHECK(PlainText(d.blocks) == "top\ndeep\n");
}

TEST_CASE("a different numId starts a different list", "[docx][p2][mapping2]") {
    // This is how Word says "a new list that looks the same". Merging them
    // numbers the second continuously from the first.
    DocxParts parts;
    parts.numbering = std::string(kBulletNumbering);
    parts.body = ListItem_("a", 2, 0) + ListItem_("b", 2, 0) + ListItem_("c", 3, 0);

    const Document d = Read(parts);
    REQUIRE(d.blocks.size() == 2);
    CHECK(d.blocks[0].As<ListBlock>()->items.size() == 2);
    CHECK(d.blocks[1].As<ListBlock>()->items.size() == 1);
}

TEST_CASE("a startOverride restarts the numbering", "[docx][p2][mapping4]") {
    // Mapping 4. Ignore this and long documents get subtly wrong numbering.
    DocxParts parts;
    parts.numbering = std::string(kBulletNumbering);
    parts.body = ListItem_("five", 3, 0);

    const Document d = Read(parts);
    const auto* list = d.blocks[0].As<ListBlock>();
    REQUIRE(list != nullptr);
    CHECK(list->ordered);
    CHECK(list->start == 5);
}

TEST_CASE("an override that only restarts keeps the abstract format",
          "[docx][p2][mapping4]") {
    // A naive "the override replaces the level" implementation turns a restarted
    // bullet list into "1. 2. 3.".
    DocxParts parts;
    parts.numbering =
        R"(<w:abstractNum w:abstractNumId="0">)"
        R"(<w:lvl w:ilvl="0"><w:numFmt w:val="bullet"/><w:lvlText w:val="-"/></w:lvl>)"
        R"(</w:abstractNum>)"
        R"(<w:num w:numId="7"><w:abstractNumId w:val="0"/>)"
        R"(<w:lvlOverride w:ilvl="0"><w:startOverride w:val="1"/></w:lvlOverride></w:num>)";
    parts.body = ListItem_("still a bullet", 7, 0);

    const Document d = Read(parts);
    const auto* list = d.blocks[0].As<ListBlock>();
    REQUIRE(list != nullptr);
    CHECK_FALSE(list->ordered);
}

TEST_CASE("an ordinary paragraph ends the list", "[docx][p2][mapping2]") {
    DocxParts parts;
    parts.numbering = std::string(kBulletNumbering);
    parts.body = ListItem_("a", 1, 0) + Para("interrupting") + ListItem_("b", 1, 0);

    const Document d = Read(parts);
    REQUIRE(d.blocks.size() == 3);
    CHECK(d.blocks[0].Is<ListBlock>());
    CHECK(d.blocks[1].Is<Paragraph>());
    CHECK(d.blocks[2].Is<ListBlock>());
}

TEST_CASE("numId 0 means explicitly not numbered", "[docx][p2][mapping2]") {
    // How Word removes a paragraph from a list without removing its style.
    DocxParts parts;
    parts.numbering = std::string(kBulletNumbering);
    parts.body = ListItem_("not a list item", 0, 0);

    const Document d = Read(parts);
    CHECK(d.blocks[0].Is<Paragraph>());
}

TEST_CASE("a numId with no definition is not treated as a list", "[docx][p2][mapping2]") {
    // Malformed. Treating it as a list produces a bullet where Word renders a
    // plain paragraph.
    DocxParts parts;
    parts.numbering = std::string(kBulletNumbering);
    parts.body = ListItem_("orphan", 99, 0);

    const Document d = Read(parts);
    CHECK(d.blocks[0].Is<Paragraph>());
}

// --- P2: tables (mapping 3) --------------------------------------------------

namespace {

std::string Cell(std::string_view text, std::string_view properties = "") {
    return "<w:tc><w:tcPr>" + std::string(properties) + "</w:tcPr>" + Para(text) + "</w:tc>";
}

}  // namespace

TEST_CASE("a simple table reads as a grid", "[docx][p2][mapping3]") {
    DocxParts parts;
    parts.body =
        R"(<w:tbl><w:tblGrid><w:gridCol w:w="2880"/><w:gridCol w:w="2880"/></w:tblGrid>)"
        "<w:tr>" + Cell("A") + Cell("B") + "</w:tr>"
        "<w:tr>" + Cell("1") + Cell("2") + "</w:tr>"
        "</w:tbl>";

    const Document d = Read(parts);
    REQUIRE(d.blocks.size() == 1);
    const auto* table = d.blocks[0].As<Table>();
    REQUIRE(table != nullptr);
    REQUIRE(table->rows.size() == 2);
    CHECK(table->rows[0].cells.size() == 2);
    CHECK(table->columns.size() == 2);
    CHECK(table->columns[0].widthPt == 144.0f);  // 2880 twips
    CHECK(PlainText(table->rows[1].cells[1].blocks) == "2\n");
}

TEST_CASE("gridSpan becomes colspan with covered placeholders", "[docx][p2][mapping3]") {
    DocxParts parts;
    parts.body =
        R"(<w:tbl><w:tblGrid><w:gridCol w:w="1440"/><w:gridCol w:w="1440"/></w:tblGrid>)"
        R"(<w:tr>)" + Cell("wide", R"(<w:gridSpan w:val="2"/>)") + "</w:tr>"
        "<w:tr>" + Cell("a") + Cell("b") + "</w:tr>"
        "</w:tbl>";

    const Document d = Read(parts);
    const auto* table = d.blocks[0].As<Table>();
    REQUIRE(table != nullptr);
    REQUIRE(table->rows[0].cells.size() == 2);
    CHECK(table->rows[0].cells[0].colSpan == 2);
    CHECK(table->rows[0].cells[1].covered);
    CHECK_FALSE(table->rows[1].cells[0].covered);
}

TEST_CASE("vMerge becomes rowspan on the starting cell", "[docx][p2][mapping3]") {
    // The fiddly one: a start marker plus continuation markers on later rows,
    // where an ABSENT w:val means "continue" -- the opposite of the usual OOXML
    // convention.
    DocxParts parts;
    parts.body =
        R"(<w:tbl><w:tblGrid><w:gridCol w:w="1440"/><w:gridCol w:w="1440"/></w:tblGrid>)"
        "<w:tr>" + Cell("tall", R"(<w:vMerge w:val="restart"/>)") + Cell("a") + "</w:tr>"
        "<w:tr>" + Cell("", R"(<w:vMerge/>)") + Cell("b") + "</w:tr>"
        "<w:tr>" + Cell("", R"(<w:vMerge/>)") + Cell("c") + "</w:tr>"
        "</w:tbl>";

    const Document d = Read(parts);
    const auto* table = d.blocks[0].As<Table>();
    REQUIRE(table != nullptr);
    REQUIRE(table->rows.size() == 3);

    CHECK(table->rows[0].cells[0].rowSpan == 3);
    CHECK(PlainText(table->rows[0].cells[0].blocks) == "tall\n");
    CHECK(table->rows[1].cells[0].covered);
    CHECK(table->rows[2].cells[0].covered);
    // The cells beside the merge are ordinary.
    CHECK_FALSE(table->rows[1].cells[1].covered);
    CHECK(PlainText(table->rows[2].cells[1].blocks) == "c\n");
}

TEST_CASE("an ordinary cell ends a vertical merge in its column",
          "[docx][p2][mapping3]") {
    DocxParts parts;
    parts.body =
        R"(<w:tbl><w:tblGrid><w:gridCol w:w="1440"/></w:tblGrid>)"
        "<w:tr>" + Cell("tall", R"(<w:vMerge w:val="restart"/>)") + "</w:tr>"
        "<w:tr>" + Cell("", R"(<w:vMerge/>)") + "</w:tr>"
        "<w:tr>" + Cell("separate") + "</w:tr>"
        "<w:tr>" + Cell("", R"(<w:vMerge/>)") + "</w:tr>"
        "</w:tbl>";

    const Document d = Read(parts);
    const auto* table = d.blocks[0].As<Table>();
    REQUIRE(table != nullptr);
    CHECK(table->rows[0].cells[0].rowSpan == 2);
    // The fourth row's continuation has no open merge above it, so it is a cell
    // in its own right rather than being attached to the first one.
    CHECK_FALSE(table->rows[3].cells[0].covered);
}

TEST_CASE("a header row is recorded", "[docx][p2][mapping3]") {
    DocxParts parts;
    parts.body =
        R"(<w:tbl><w:tblGrid><w:gridCol w:w="1440"/></w:tblGrid>)"
        R"(<w:tr><w:trPr><w:tblHeader/></w:trPr>)" + Cell("H") + "</w:tr>"
        "<w:tr>" + Cell("D") + "</w:tr>"
        "</w:tbl>";

    const Document d = Read(parts);
    CHECK(d.blocks[0].As<Table>()->headerRows == 1);
}

TEST_CASE("a list inside a table cell is reconstructed", "[docx][p2]") {
    DocxParts parts;
    parts.numbering = std::string(kBulletNumbering);
    parts.body =
        R"(<w:tbl><w:tblGrid><w:gridCol w:w="1440"/></w:tblGrid><w:tr><w:tc><w:tcPr/>)" +
        ListItem_("a", 1, 0) + ListItem_("b", 1, 0) + "</w:tc></w:tr></w:tbl>";

    const Document d = Read(parts);
    const auto* table = d.blocks[0].As<Table>();
    REQUIRE(table != nullptr);
    REQUIRE(table->rows[0].cells[0].blocks.size() == 1);
    CHECK(table->rows[0].cells[0].blocks[0].Is<ListBlock>());
}

// --- Content controls, fields, tracked changes -------------------------------

TEST_CASE("a content control is unwrapped, not dropped", "[docx][p1]") {
    // Section 8.2's rule 3 nominates these for RawBlock, but they are how every
    // Word template expresses a title. Treating one as opaque would make
    // docx -> md silently drop the document's title.
    Document d;
    DocxParts parts;
    parts.body =
        R"(<w:sdt><w:sdtPr/><w:sdtContent>)" + Para("The Title") + R"(</w:sdtContent></w:sdt>)" +
        Para("Body");

    const Result result = ReadWithResult(parts, d);
    REQUIRE(result.ok());
    CHECK(PlainText(d.blocks) == "The Title\nBody\n");

    bool noted = false;
    for (const FidelityNote& note : result.notes()) {
        noted |= note.code == "content-control-flattened";
    }
    CHECK(noted);
}

TEST_CASE("a field renders as its cached result", "[docx][p1]") {
    // A page number should render as the number, not as "PAGE \\* MERGEFORMAT".
    const Document d = Read(WithBody(
        R"(<w:p><w:fldSimple w:instr=" PAGE ">)"
        R"(<w:r><w:t>7</w:t></w:r></w:fldSimple></w:p>)"));
    CHECK(PlainText(d.blocks) == "7\n");
}

TEST_CASE("a tracked insertion is kept and a deletion is not", "[docx][p1]") {
    // Inserted text is part of the document as it currently reads. Deleted text
    // is not, and putting it back would be a confidentiality problem, not just a
    // fidelity one.
    Document d;
    DocxParts parts;
    parts.body =
        R"(<w:p><w:ins><w:r><w:t>kept </w:t></w:r></w:ins>)"
        R"(<w:del><w:r><w:delText>removed</w:delText></w:r></w:del>)"
        R"(<w:r><w:t>tail</w:t></w:r></w:p>)";

    const Result result = ReadWithResult(parts, d);
    REQUIRE(result.ok());
    CHECK(PlainText(d.blocks) == "kept tail\n");
    CHECK(PlainText(d.blocks).find("removed") == std::string::npos);

    bool noted = false;
    for (const FidelityNote& note : result.notes()) {
        noted |= note.code == "tracked-deletion-dropped";
    }
    CHECK(noted);
}

TEST_CASE("a repeated problem is reported once", "[docx][fidelity]") {
    // A document with four hundred content controls should say so once.
    Document d;
    DocxParts parts;
    for (int i = 0; i < 20; ++i) {
        parts.body += R"(<w:sdt><w:sdtContent>)" + Para("x") + R"(</w:sdtContent></w:sdt>)";
    }

    const Result result = ReadWithResult(parts, d);
    REQUIRE(result.ok());
    std::size_t count = 0;
    for (const FidelityNote& note : result.notes()) {
        count += note.code == "content-control-flattened" ? 1 : 0;
    }
    CHECK(count == 1);
}

// --- Metadata ----------------------------------------------------------------

TEST_CASE("core properties become document metadata", "[docx][metadata]") {
    DocxParts parts;
    parts.coreProperties =
        "<dc:title>Quarterly Report</dc:title>"
        "<dc:creator>A. Author</dc:creator>"
        "<dc:language>en-GB</dc:language>"
        "<dcterms:created>2026-01-15T10:00:00Z</dcterms:created>";
    parts.body = Para("x");

    const Document d = Read(parts);
    CHECK(d.meta.title == "Quarterly Report");
    REQUIRE(d.meta.authors.size() == 1);
    CHECK(d.meta.authors[0] == "A. Author");
    CHECK(d.meta.lang == "en-GB");
    CHECK(d.meta.created == "2026-01-15T10:00:00Z");
}

// --- Malformed input ---------------------------------------------------------

TEST_CASE("a package with no Word document is refused clearly", "[docx][security]") {
    opc::PackageWriter writer;
    writer.AddDefault("xml", "application/xml");
    writer.AddDefault("rels", std::string(opc::mime::kRelationships));
    writer.AddPart("/xl/workbook.xml", "application/xml", "<workbook/>");
    writer.AddRelationship("", "http://example.com/other", "xl/workbook.xml");

    std::vector<std::byte> zip;
    REQUIRE(writer.Finish(zip).ok());

    Document d;
    const Result result = ReadDocx(zip, {}, d);
    CHECK_FALSE(result.ok());
    CHECK(result.status() == Status::kMalformedInput);
    CHECK(result.message().find("does not contain a Word document") != std::string::npos);
}

TEST_CASE("a document part that is not XML is refused", "[docx][security]") {
    opc::PackageWriter writer;
    writer.AddDefault("xml", "application/xml");
    writer.AddDefault("rels", std::string(opc::mime::kRelationships));
    writer.AddPart("/word/document.xml", std::string(opc::mime::kMainDocument),
                   "<w:document><this is not closed");
    writer.AddRelationship("", std::string(opc::rel::kOfficeDocument), "word/document.xml");

    std::vector<std::byte> zip;
    REQUIRE(writer.Finish(zip).ok());

    Document d;
    const Result result = ReadDocx(zip, {}, d);
    CHECK_FALSE(result.ok());
    CHECK(result.status() == Status::kMalformedInput);
}

// --- The point of all of this ------------------------------------------------

TEST_CASE("DOCX reaches Markdown through the IR with no direct converter",
          "[docx][roundtrip]") {
    DocxParts parts;
    parts.numbering = std::string(kBulletNumbering);
    parts.styles =
        R"(<w:style w:type="paragraph" w:styleId="Heading1"><w:name w:val="heading 1"/></w:style>)";
    parts.documentRelationships.push_back(
        opc::Relationship{"rIdLink", std::string(opc::rel::kHyperlink), "https://example.com",
                          true});
    parts.body =
        R"(<w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr><w:r><w:t>Report</w:t></w:r></w:p>)"
        R"(<w:p><w:r><w:t>Text with </w:t></w:r>)"
        R"(<w:r><w:rPr><w:b/></w:rPr><w:t>bold</w:t></w:r>)"
        R"(<w:r><w:t> and a </w:t></w:r>)"
        R"(<w:hyperlink r:id="rIdLink"><w:r><w:t>link</w:t></w:r></w:hyperlink>)"
        R"(<w:r><w:t>.</w:t></w:r></w:p>)" +
        ListItem_("first", 1, 0) + ListItem_("second", 1, 0);

    const Document d = Read(parts);

    std::string markdown;
    const Result written = morph::docmd::WriteMarkdown(d, {}, markdown);
    REQUIRE(written.ok());

    INFO(markdown);
    CHECK(markdown.find("# Report") != std::string::npos);
    CHECK(markdown.find("**bold**") != std::string::npos);
    CHECK(markdown.find("[link](https://example.com)") != std::string::npos);
    CHECK(markdown.find("- first") != std::string::npos);
    CHECK(markdown.find("- second") != std::string::npos);
}
