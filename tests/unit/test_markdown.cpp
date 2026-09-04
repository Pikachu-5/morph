// Markdown reader and writer (architecture section 8.3).
//
// The assertion that matters here is the round trip. Section 19's M7 exit
// criterion is "md <-> IR <-> md round-trips without corruption", and section
// 8.3 says getting the escaping wrong "produces Markdown that does not
// round-trip, which is the one thing a converter must never do".
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "morph/providers/markdown.h"

using namespace morph;
using namespace morph::doc;
using morph::docmd::EscapeText;
using morph::docmd::Flavour;
using morph::docmd::ReadMarkdown;
using morph::docmd::WriteMarkdown;

namespace {

Document Read(std::string_view markdown, const OptionBag& options = {}) {
    Document document;
    const Result result = ReadMarkdown(markdown, options, document);
    REQUIRE(result.ok());
    return document;
}

std::string Write(const Document& document, const OptionBag& options = {}) {
    std::string out;
    const Result result = WriteMarkdown(document, options, out);
    REQUIRE(result.ok());
    return out;
}

// Reads, writes, reads again. Comparing the two IRs rather than the two texts is
// the right test: the writer normalises whitespace and marker style on purpose,
// so text equality would fail for changes that lose nothing.
void CheckRoundTrip(std::string_view markdown) {
    const Document first = Read(markdown);
    const std::string written = Write(first);
    const Document second = Read(written);

    INFO("original:\n" << markdown << "\nwritten:\n" << written);
    CHECK(PlainText(first.blocks) == PlainText(second.blocks));
    CHECK(CountBlocks(first.blocks) == CountBlocks(second.blocks));
}

OptionBag WithFlavour(std::string_view flavour) {
    OptionBag bag;
    bag.Set(std::string(opt::kMarkdownFlavour), std::string(flavour));
    return bag;
}

}  // namespace

// --- Reading -----------------------------------------------------------------

TEST_CASE("headings carry their level and a stable anchor", "[markdown][read]") {
    const Document d = Read("# One\n\n## Two\n");
    REQUIRE(d.blocks.size() == 2);

    const auto* first = d.blocks[0].As<Heading>();
    REQUIRE(first != nullptr);
    CHECK(first->level == 1);
    CHECK(first->anchorId == "one");

    const auto* second = d.blocks[1].As<Heading>();
    REQUIRE(second != nullptr);
    CHECK(second->level == 2);
    CHECK(second->anchorId == "two");
}

TEST_CASE("duplicate headings get distinct anchors", "[markdown][read]") {
    // Renderers disambiguate with -1, -2, and links written against a rendered
    // page rely on it.
    const Document d = Read("# Notes\n\n# Notes\n\n# Notes\n");
    REQUIRE(d.blocks.size() == 3);
    CHECK(d.blocks[0].As<Heading>()->anchorId == "notes");
    CHECK(d.blocks[1].As<Heading>()->anchorId == "notes-1");
    CHECK(d.blocks[2].As<Heading>()->anchorId == "notes-2");
}

TEST_CASE("adjacent text runs are merged", "[markdown][read]") {
    // Section 8.4 warns about this for DOCX runs and it applies just as much
    // here: without merging, a five-word paragraph becomes eleven IR nodes.
    const Document d = Read("plain text with entity &amp; more text\n");
    REQUIRE(d.blocks.size() == 1);

    const auto* paragraph = d.blocks[0].As<Paragraph>();
    REQUIRE(paragraph != nullptr);
    CHECK(paragraph->inlines.size() == 1);
    CHECK(paragraph->inlines[0].As<Text>()->text == "plain text with entity & more text");
}

TEST_CASE("emphasis and strong become semantic nodes, not styles", "[markdown][read]") {
    // Rule 2's corollary: bold and italic have a semantic node in every target
    // format, so they must not land on a TextStyle.
    const Document d = Read("*em* and **strong** and ~~gone~~\n");
    const auto* paragraph = d.blocks[0].As<Paragraph>();
    REQUIRE(paragraph != nullptr);

    bool saw_emphasis = false, saw_strong = false, saw_strike = false;
    ForEachInline(paragraph->inlines, [&](const Inline& node) {
        saw_emphasis |= node.Is<Emphasis>();
        saw_strong |= node.Is<Strong>();
        saw_strike |= node.Is<Strikethrough>();
    });
    CHECK(saw_emphasis);
    CHECK(saw_strong);
    CHECK(saw_strike);
}

TEST_CASE("a fenced code block keeps its language and its bytes", "[markdown][read]") {
    const Document d = Read("```cpp\nint main() { return *p; }\n```\n");
    const auto* code = d.blocks[0].As<CodeBlock>();
    REQUIRE(code != nullptr);
    CHECK(code->language == "cpp");
    CHECK(code->text == "int main() { return *p; }\n");
}

TEST_CASE("a GFM table becomes a Table with header rows", "[markdown][read]") {
    const Document d = Read(
        "| A | B |\n"
        "| - | - |\n"
        "| 1 | 2 |\n");
    const auto* table = d.blocks[0].As<Table>();
    REQUIRE(table != nullptr);
    CHECK(table->rows.size() == 2);
    CHECK(table->headerRows == 1);
    CHECK(table->rows[0].cells.size() == 2);
    CHECK(PlainText(table->rows[1].cells[1].blocks) == "2\n");
}

TEST_CASE("table column alignment survives reading", "[markdown][read]") {
    const Document d = Read(
        "| L | C | R |\n"
        "|:--|:-:|--:|\n"
        "| 1 | 2 | 3 |\n");
    const auto* table = d.blocks[0].As<Table>();
    REQUIRE(table != nullptr);
    CHECK(table->rows[0].cells[0].alignment == Alignment::kStart);
    CHECK(table->rows[0].cells[1].alignment == Alignment::kCenter);
    CHECK(table->rows[0].cells[2].alignment == Alignment::kEnd);
}

TEST_CASE("task list items keep their checked state", "[markdown][read]") {
    const Document d = Read("- [x] done\n- [ ] todo\n- plain\n");
    const auto* list = d.blocks[0].As<ListBlock>();
    REQUIRE(list != nullptr);
    REQUIRE(list->items.size() == 3);
    CHECK(list->items[0].checked == true);
    CHECK(list->items[1].checked == false);
    // nullopt, not false: "not a task" and "an unchecked task" are different.
    CHECK_FALSE(list->items[2].checked.has_value());
}

TEST_CASE("an ordered list keeps its start number", "[markdown][read]") {
    const Document d = Read("5. five\n6. six\n");
    const auto* list = d.blocks[0].As<ListBlock>();
    REQUIRE(list != nullptr);
    CHECK(list->ordered);
    CHECK(list->start == 5);
}

TEST_CASE("raw HTML is preserved as a RawBlock", "[markdown][read]") {
    // Rule 3, the pressure valve. Recognised structurally, not mappable, kept
    // byte-exact so a Markdown round trip does not destroy it.
    const Document d = Read("<div class=\"note\">\nhi\n</div>\n");
    const auto* raw = d.blocks[0].As<RawBlock>();
    REQUIRE(raw != nullptr);
    CHECK(raw->format == "html");
    CHECK(std::string(reinterpret_cast<const char*>(raw->bytes.data()), raw->bytes.size())
              .find("class=\"note\"") != std::string::npos);
}

TEST_CASE("numeric and named entities are decoded", "[markdown][read]") {
    const Document d = Read("&#65;&#x42;&amp;&mdash;\n");
    CHECK(PlainText(d.blocks) == "AB&\xE2\x80\x94\n");
}

TEST_CASE("an unknown entity is left exactly as written", "[markdown][read]") {
    // Guessing would be worse than passing it through: "&foo;" re-emitted is a
    // perfect round trip, whereas a wrong guess is silent corruption.
    const Document d = Read("&notanentity;\n");
    CHECK(PlainText(d.blocks) == "&notanentity;\n");
}

TEST_CASE("a byte order mark is not content", "[markdown][read]") {
    const Document d = Read("\xEF\xBB\xBF# Title\n");
    const auto* heading = d.blocks[0].As<Heading>();
    REQUIRE(heading != nullptr);
    CHECK(heading->inlines[0].As<Text>()->text == "Title");
}

TEST_CASE("an empty document reads as an empty document", "[markdown][read]") {
    const Document d = Read("");
    CHECK(d.blocks.empty());
}

// --- Escaping ----------------------------------------------------------------

TEST_CASE("characters that would start a construct are escaped", "[markdown][escape]") {
    CHECK(EscapeText("a*b", false, Flavour::kGitHub) == "a\\*b");
    CHECK(EscapeText("a[b]", false, Flavour::kGitHub) == "a\\[b\\]");
    CHECK(EscapeText("a`b", false, Flavour::kGitHub) == "a\\`b");
    CHECK(EscapeText("a\\b", false, Flavour::kGitHub) == "a\\\\b");
    CHECK(EscapeText("a<b", false, Flavour::kGitHub) == "a\\<b");
}

TEST_CASE("intraword underscores are left alone in GFM", "[markdown][escape]") {
    // snake_case is not emphasis in CommonMark or GFM, and escaping it produces
    // output that is technically correct and horrible to read.
    CHECK(EscapeText("snake_case_name", false, Flavour::kGitHub) == "snake_case_name");
    CHECK(EscapeText("_leading", false, Flavour::kGitHub) == "\\_leading");
    CHECK(EscapeText("trailing_", false, Flavour::kGitHub) == "trailing\\_");
    // Portable escapes anyway, because that is what it is for.
    CHECK(EscapeText("snake_case", false, Flavour::kPortable) == "snake\\_case");
}

TEST_CASE("block markers are escaped only at the start of a line", "[markdown][escape]") {
    CHECK(EscapeText("# not a heading", true, Flavour::kGitHub) == "\\# not a heading");
    CHECK(EscapeText("a # b", false, Flavour::kGitHub) == "a # b");
    CHECK(EscapeText("> not a quote", true, Flavour::kGitHub) == "\\> not a quote");
    CHECK(EscapeText("1. not a list", true, Flavour::kGitHub) == "\\1. not a list");
    // A digit that is not a list marker is left alone.
    CHECK(EscapeText("1990 was a year", true, Flavour::kGitHub) == "1990 was a year");
    // A dash needs whitespace after it to be a bullet.
    CHECK(EscapeText("-5 degrees", true, Flavour::kGitHub) == "-5 degrees");
    CHECK(EscapeText("- a bullet", true, Flavour::kGitHub) == "\\- a bullet");
}

TEST_CASE("ampersands are escaped only when they look like entities",
          "[markdown][escape]") {
    CHECK(EscapeText("Tom & Jerry", false, Flavour::kGitHub) == "Tom & Jerry");
    CHECK(EscapeText("A&amp;B", false, Flavour::kGitHub) == "A\\&amp;B");
    CHECK(EscapeText("A&#65;B", false, Flavour::kGitHub) == "A\\&#65;B");
}

TEST_CASE("a bang is escaped only before a bracket", "[markdown][escape]") {
    CHECK(EscapeText("wow!", false, Flavour::kGitHub) == "wow!");
    CHECK(EscapeText("![alt](x)", false, Flavour::kGitHub) == "\\!\\[alt\\](x)");
}

// --- Round trips -------------------------------------------------------------

TEST_CASE("prose round-trips", "[markdown][roundtrip]") {
    CheckRoundTrip(
        "# Title\n"
        "\n"
        "A paragraph with *emphasis*, **strong**, `code`, and a [link](https://example.com).\n"
        "\n"
        "## Section\n"
        "\n"
        "Another paragraph.\n");
}

TEST_CASE("lists round-trip", "[markdown][roundtrip]") {
    CheckRoundTrip(
        "- one\n"
        "- two\n"
        "  - nested\n"
        "  - also nested\n"
        "- three\n"
        "\n"
        "1. first\n"
        "2. second\n");
}

TEST_CASE("a tight list item keeps its text", "[markdown][read][regression]") {
    // md4c does not emit MD_BLOCK_P inside a tight item -- the text arrives as
    // inline content on the item itself. Discarding it produced empty list
    // items, and a round-trip check could not see the bug because both sides
    // lost the text equally. So this asserts the content directly.
    const Document d = Read("- alpha\n- beta\n");
    const auto* list = d.blocks[0].As<ListBlock>();
    REQUIRE(list != nullptr);
    REQUIRE(list->items.size() == 2);

    REQUIRE(list->items[0].blocks.size() == 1);
    CHECK(PlainText(list->items[0].blocks) == "alpha\n");
    CHECK(PlainText(list->items[1].blocks) == "beta\n");
    CHECK(PlainText(d.blocks) == "alpha\nbeta\n");
}

TEST_CASE("a tight item with a nested list keeps both", "[markdown][read][regression]") {
    const Document d = Read("- outer\n  - inner\n");
    const auto* list = d.blocks[0].As<ListBlock>();
    REQUIRE(list != nullptr);
    REQUIRE(list->items.size() == 1);

    // The item's own text comes first, then the nested list.
    REQUIRE(list->items[0].blocks.size() == 2);
    CHECK(list->items[0].blocks[0].Is<Paragraph>());
    CHECK(list->items[0].blocks[1].Is<ListBlock>());
    CHECK(PlainText(d.blocks) == "outer\ninner\n");
}

TEST_CASE("a loose list item keeps its text too", "[markdown][read][regression]") {
    const Document d = Read("- alpha\n\n- beta\n");
    const auto* list = d.blocks[0].As<ListBlock>();
    REQUIRE(list != nullptr);
    CHECK_FALSE(list->tight);
    CHECK(PlainText(d.blocks) == "alpha\nbeta\n");
}


TEST_CASE("tables round-trip", "[markdown][roundtrip]") {
    CheckRoundTrip(
        "| Name | Value |\n"
        "| ---- | ----: |\n"
        "| a    | 1     |\n"
        "| b    | 2     |\n");
}

TEST_CASE("code blocks round-trip", "[markdown][roundtrip]") {
    CheckRoundTrip(
        "```python\n"
        "def f(x):\n"
        "    return x * 2\n"
        "```\n");
}

TEST_CASE("block quotes round-trip", "[markdown][roundtrip]") {
    CheckRoundTrip(
        "> quoted text\n"
        ">\n"
        "> > nested quote\n");
}

TEST_CASE("text that looks like markup round-trips", "[markdown][roundtrip]") {
    // The escaping test. Every one of these would be re-parsed as markup if the
    // writer failed to escape it.
    CheckRoundTrip("A literal asterisk * and underscore _ and bracket [ here.\n");
    CheckRoundTrip("Not a heading: # hash, and not a quote: > angle.\n");
    CheckRoundTrip("Backslash \\\\ and backtick \\` in prose.\n");
    CheckRoundTrip("An ampersand entity that is literal: &amp;amp;\n");
}

TEST_CASE("a code fence longer than the content is chosen", "[markdown][roundtrip]") {
    // A naive three-backtick fence would terminate inside the content and turn
    // the rest of the document into prose.
    Document document;
    document.blocks.push_back(Block::Of(CodeBlock{"", "here is ``` a fence\n"}));

    const std::string written = Write(document);
    const Document reread = Read(written);

    const auto* code = reread.blocks[0].As<CodeBlock>();
    REQUIRE(code != nullptr);
    CHECK(code->text == "here is ``` a fence\n");
}

TEST_CASE("inline code containing backticks round-trips", "[markdown][roundtrip]") {
    Document document;
    Paragraph paragraph;
    paragraph.inlines.push_back(Inline::Of(InlineCode{"a ` b"}));
    document.blocks.push_back(Block::Of(std::move(paragraph)));

    const Document reread = Read(Write(document));
    bool found = false;
    ForEachBlock(reread.blocks, [&](const Block& block) {
        if (const auto* p = block.As<Paragraph>()) {
            ForEachInline(p->inlines, [&](const Inline& node) {
                if (const auto* code = node.As<InlineCode>()) {
                    found = true;
                    CHECK(code->text == "a ` b");
                }
            });
        }
    });
    CHECK(found);
}

TEST_CASE("raw HTML round-trips byte-exact", "[markdown][roundtrip]") {
    // Rule 3's promise.
    const std::string html = "<div class=\"note\" data-x='1'>\n<p>hi &amp; bye</p>\n</div>\n";
    const Document first = Read(html);
    const Document second = Read(Write(first));

    const auto* raw = second.blocks[0].As<RawBlock>();
    REQUIRE(raw != nullptr);
    CHECK(std::string(reinterpret_cast<const char*>(raw->bytes.data()), raw->bytes.size())
              .find("data-x='1'") != std::string::npos);
}

// --- Writing and the fidelity contract --------------------------------------

TEST_CASE("the writer emits exactly one trailing newline", "[markdown][write]") {
    const std::string written = Write(Read("# Title\n\n\n\nBody\n\n\n"));
    REQUIRE_FALSE(written.empty());
    CHECK(written.back() == '\n');
    CHECK(written[written.size() - 2] != '\n');
}

TEST_CASE("metadata becomes front matter only when there is metadata",
          "[markdown][write]") {
    Document bare;
    bare.blocks.push_back(Block::Of(Paragraph{{Inline::Of(Text{"hi", {}})}, {}, {}, std::nullopt}));
    CHECK_FALSE(Write(bare).starts_with("---"));

    bare.meta.title = "A Title";
    bare.meta.authors = {"Someone"};
    const std::string written = Write(bare);
    CHECK(written.starts_with("---\ntitle: A Title\nauthor: Someone\n"));
}

TEST_CASE("CommonMark reports what it cannot express", "[markdown][fidelity]") {
    // Rule 1: the IR kept the table and the strikethrough. The loss happens at
    // the writer, where it can be reported, and this is that report.
    const Document d = Read(
        "| A | B |\n"
        "| - | - |\n"
        "| 1 | 2 |\n"
        "\n"
        "~~struck~~\n");

    std::string out;
    const Result result = WriteMarkdown(d, WithFlavour("commonmark"), out);

    REQUIRE(result.ok());
    CHECK(result.incurred() == Fidelity::Structural);

    bool table_note = false, strike_note = false;
    for (const FidelityNote& note : result.notes()) {
        table_note |= note.code == "table-dropped";
        strike_note |= note.code == "strikethrough-dropped";
    }
    CHECK(table_note);
    CHECK(strike_note);
    CHECK(out.find('|') == std::string::npos);
}

TEST_CASE("GFM expresses tables and strikethrough with no notes", "[markdown][fidelity]") {
    const Document d = Read("| A |\n| - |\n| 1 |\n\n~~x~~\n");

    std::string out;
    const Result result = WriteMarkdown(d, WithFlavour("gfm"), out);

    REQUIRE(result.ok());
    CHECK(result.notes().empty());
    CHECK(result.incurred() == Fidelity::Lossless);
}

TEST_CASE("dropped character formatting is reported, not silent", "[markdown][fidelity]") {
    Document document;
    Text styled{"coloured", {}};
    styled.style.colour = Colour{0xFF, 0x00, 0x00, 0xFF};
    styled.style.fontSizePt = 18.0f;
    document.blocks.push_back(
        Block::Of(Paragraph{{Inline::Of(std::move(styled))}, {}, {}, std::nullopt}));

    std::string out;
    const Result result = WriteMarkdown(document, {}, out);

    REQUIRE(result.ok());
    CHECK(result.incurred() == Fidelity::Structural);
    REQUIRE_FALSE(result.notes().empty());
    CHECK(result.notes().front().code == "text-style-dropped");
    CHECK(out.find("coloured") != std::string::npos);
}

TEST_CASE("a table with no header row gets one, with a note", "[markdown][fidelity]") {
    // GFM requires a header. Dropping the first data row instead is what a naive
    // writer does and what users notice immediately.
    Document document;
    Table table;
    table.rows.push_back(TableRow{{TableCell{{Block::Of(Paragraph{
        {Inline::Of(Text{"only row", {}})}, {}, {}, std::nullopt})}, 1, 1, std::nullopt, false}}});
    document.blocks.push_back(Block::Of(std::move(table)));

    std::string out;
    const Result result = WriteMarkdown(document, {}, out);

    REQUIRE(result.ok());
    CHECK(out.find("only row") != std::string::npos);
    bool noted = false;
    for (const FidelityNote& note : result.notes()) {
        noted |= note.code == "table-header-synthesised";
    }
    CHECK(noted);
}

TEST_CASE("merged table cells are reported when flattened", "[markdown][fidelity]") {
    Document document;
    Table table;
    TableCell wide;
    wide.colSpan = 2;
    wide.blocks.push_back(
        Block::Of(Paragraph{{Inline::Of(Text{"spans two", {}})}, {}, {}, std::nullopt}));
    TableCell covered;
    covered.covered = true;
    table.rows.push_back(TableRow{{std::move(wide), std::move(covered)}});
    table.headerRows = 1;
    document.blocks.push_back(Block::Of(std::move(table)));

    std::string out;
    const Result result = WriteMarkdown(document, {}, out);

    bool noted = false;
    for (const FidelityNote& note : result.notes()) {
        noted |= note.code == "table-spans-flattened";
    }
    CHECK(noted);
}

// --- Structural limits -------------------------------------------------------

TEST_CASE("deeply nested input is refused rather than overflowing the stack",
          "[markdown][security]") {
    // Section 15.2 caps nesting depth. A crafted document nests block quotes
    // thousands deep and a recursive writer would die on it.
    std::string bomb;
    for (int i = 0; i < 500; ++i) bomb += "> ";
    bomb += "boom\n";

    Document document;
    const Result result = ReadMarkdown(bomb, {}, document);

    CHECK_FALSE(result.ok());
    CHECK(result.status() == Status::kResourceLimit);
    CHECK_FALSE(result.message().empty());
}

TEST_CASE("null bytes become the replacement character", "[markdown][security]") {
    const std::string_view input("before\0after\n", 13);
    const Document d = Read(input);
    const std::string text = PlainText(d.blocks);
    CHECK(text.find('\0') == std::string::npos);
    CHECK(text.find("\xEF\xBF\xBD") != std::string::npos);
}

// --- Front matter -------------------------------------------------------------
//
// The writer emits YAML front matter for document metadata. Without a reader for
// it, md -> docx -> md turned the block into a setext heading reading
// "title: Reportauthor: A. Person". A writer that emits what its own reader
// cannot read is a bug whatever the specification says -- and CommonMark says
// nothing about front matter at all.

TEST_CASE("front matter is metadata, not content", "[markdown][read][regression]") {
    const Document d = Read(
        "---\n"
        "title: Quarterly Report\n"
        "author: A. Person\n"
        "lang: en-GB\n"
        "---\n"
        "\n"
        "# Heading\n");

    CHECK(d.meta.title == "Quarterly Report");
    REQUIRE(d.meta.authors.size() == 1);
    CHECK(d.meta.authors[0] == "A. Person");
    CHECK(d.meta.lang == "en-GB");

    // And none of it leaked into the body.
    REQUIRE(d.blocks.size() == 1);
    CHECK(d.blocks[0].Is<Heading>());
    CHECK(PlainText(d.blocks) == "Heading\n");
}

TEST_CASE("an author sequence is read", "[markdown][read][regression]") {
    const Document d = Read("---\nauthors:\n  - First Person\n  - Second Person\n---\n\nBody.\n");
    REQUIRE(d.meta.authors.size() == 2);
    CHECK(d.meta.authors[0] == "First Person");
    CHECK(d.meta.authors[1] == "Second Person");
}

TEST_CASE("an unknown front matter key is kept rather than discarded",
          "[markdown][read][regression]") {
    // Design rule 1 applied to metadata: keep what we do not understand so it
    // survives a round trip.
    const Document d = Read("---\nkeywords: alpha, beta\n---\n\nBody.\n");
    REQUIRE(d.meta.extra.count("keywords") == 1);
    CHECK(d.meta.extra.at("keywords") == "alpha, beta");
}

TEST_CASE("metadata round-trips through the writer and back",
          "[markdown][roundtrip][regression]") {
    Document original;
    original.meta.title = "The Title";
    original.meta.authors = {"An Author"};
    original.meta.lang = "en";
    original.blocks.push_back(
        Block::Of(Paragraph{{Inline::Of(Text{"Body.", {}})}, {}, {}, std::nullopt}));

    const Document back = Read(Write(original));
    CHECK(back.meta.title == "The Title");
    REQUIRE(back.meta.authors.size() == 1);
    CHECK(back.meta.authors[0] == "An Author");
    CHECK(back.meta.lang == "en");
    CHECK(PlainText(back.blocks) == "Body.\n");
}

TEST_CASE("a horizontal rule is not mistaken for front matter",
          "[markdown][read][regression]") {
    // "---" at the very start with no closing fence is a thematic break followed
    // by prose, and consuming it would delete the document.
    const Document d = Read("---\n\nJust a rule above this paragraph.\n");
    CHECK(d.meta.title.empty());
    CHECK(PlainText(d.blocks).find("Just a rule") != std::string::npos);
}

TEST_CASE("Table cells keep their formatting", "[markdown]") {
    // GFM allows emphasis, code spans and links inside a cell. Writing the cell
    // as plain text loses information the format can hold, which is rule 1 in
    // document.h read backwards.
    Document document;
    Table table;
    table.columns.resize(1);
    table.headerRows = 1;
    for (int row = 0; row < 2; ++row) {
        TableRow line;
        TableCell cell;
        Paragraph paragraph;
        paragraph.inlines.push_back(Inline::Of(Text{"a ", {}}));
        paragraph.inlines.push_back(Inline::Of(Strong{{Inline::Of(Text{"bold", {}})}}));
        paragraph.inlines.push_back(Inline::Of(Text{" and ", {}}));
        paragraph.inlines.push_back(Inline::Of(InlineCode{"code"}));
        cell.blocks.push_back(Block::Of(std::move(paragraph)));
        line.cells.push_back(std::move(cell));
        table.rows.push_back(std::move(line));
    }
    document.blocks.push_back(Block::Of(std::move(table)));

    std::string markdown;
    REQUIRE(WriteMarkdown(document, {}, markdown).ok());
    CHECK(markdown.find("**bold**") != std::string::npos);
    CHECK(markdown.find("`code`") != std::string::npos);

    Document read;
    REQUIRE(ReadMarkdown(markdown, {}, read).ok());
    const Table* back = read.blocks.front().As<Table>();
    REQUIRE(back != nullptr);
    const Inlines& cell = back->rows[0].cells[0].blocks[0].As<Paragraph>()->inlines;
    REQUIRE(cell.size() == 4);
    CHECK(cell[1].Is<Strong>());
    REQUIRE(cell[3].Is<InlineCode>());
    CHECK(cell[3].As<InlineCode>()->text == "code");
}

TEST_CASE("A document may begin with a horizontal rule", "[markdown]") {
    // "---" on the first line is also how YAML front matter opens, and a second
    // rule further down closes it. Treating that as front matter deletes
    // everything between the two rules, which is most of the document.
    Document read;
    REQUIRE(ReadMarkdown("---\n\nfirst\n\n---\n\nsecond\n", {}, read).ok());
    REQUIRE(read.blocks.size() == 4);
    CHECK(read.blocks[0].Is<HorizontalRule>());
    CHECK(PlainText(read.blocks).find("first") != std::string::npos);
    CHECK(PlainText(read.blocks).find("second") != std::string::npos);

    // Real front matter still works.
    Document meta;
    REQUIRE(ReadMarkdown("---\ntitle: Report\n---\n\nbody\n", {}, meta).ok());
    CHECK(meta.meta.title == "Report");
    REQUIRE(meta.blocks.size() == 1);

    // And our own writer does not produce the ambiguity in the first place.
    Document leading;
    leading.blocks.push_back(Block::Of(HorizontalRule{}));
    leading.blocks.push_back(Block::Of(Paragraph{{Inline::Of(Text{"after", {}})}, {}, {}, {}}));
    std::string written;
    REQUIRE(WriteMarkdown(leading, {}, written).ok());
    CHECK(written.starts_with("***"));
}

TEST_CASE("Strikethrough that cannot be written says so", "[markdown]") {
    // "~~" is the one delimiter that does not work intraword: "aa*bb*cc" is
    // emphasis but "aa~~bb~~cc" is four literal tildes. Writing them anyway
    // would put tildes into the user's text.
    Document document;
    Paragraph paragraph;
    paragraph.inlines.push_back(Inline::Of(Text{"runs", {}}));
    paragraph.inlines.push_back(Inline::Of(Strikethrough{{Inline::Of(Text{"gone", {}})}}));
    paragraph.inlines.push_back(Inline::Of(Text{"more", {}}));
    document.blocks.push_back(Block::Of(std::move(paragraph)));

    std::string markdown;
    const Result result = WriteMarkdown(document, {}, markdown);
    REQUIRE(result.ok());
    CHECK(markdown.find("~~") == std::string::npos);
    CHECK(markdown.find("runsgonemore") != std::string::npos);

    bool reported = false;
    for (const auto& note : result.notes()) {
        if (note.code == "strikethrough-intraword") reported = true;
    }
    CHECK(reported);
}

TEST_CASE("A pipe inside a code span flattens its cell", "[markdown]") {
    // Backslash escapes do not apply inside a code span, so an escaped pipe
    // comes back with the backslash still in it -- and an unescaped one splits
    // the row. The cell is flattened to plain text, where the escape works, and
    // the loss is reported rather than silent.
    Document document;
    Table table;
    table.columns.resize(1);
    table.headerRows = 1;
    for (int row = 0; row < 2; ++row) {
        TableRow line;
        TableCell cell;
        Paragraph paragraph;
        paragraph.inlines.push_back(Inline::Of(InlineCode{"a|b"}));
        cell.blocks.push_back(Block::Of(std::move(paragraph)));
        line.cells.push_back(std::move(cell));
        table.rows.push_back(std::move(line));
    }
    document.blocks.push_back(Block::Of(std::move(table)));

    std::string markdown;
    const Result result = WriteMarkdown(document, {}, markdown);
    REQUIRE(result.ok());

    Document read;
    REQUIRE(ReadMarkdown(markdown, {}, read).ok());
    const Table* back = read.blocks.front().As<Table>();
    REQUIRE(back != nullptr);
    // The pipe is still the user's text, which is the part that matters.
    CHECK(PlainText(back->rows[0].cells[0].blocks).find("a|b") != std::string::npos);

    bool reported = false;
    for (const FidelityNote& note : result.notes()) {
        reported |= note.code == "table-cell-flattened";
    }
    CHECK(reported);
}
