// IR round-trip property tests (architecture section 16, first row).
//
// "Generate random valid IR documents, write to DOCX, read back, assert
//  structural equality modulo declared-lossy fields. This catches more real bugs
//  than any other single test."
//
// It does. The example-based tests next door assert things somebody thought of;
// this one asserts a property over documents nobody thought of, and the first
// afternoon it ran it found eleven defects in code that had been read carefully
// several times -- among them a Markdown writer that flattened every table cell
// to plain text, a code span that could not survive a Word round trip, and a
// document beginning with a horizontal rule whose entire contents were eaten as
// YAML front matter.
//
// FOUR DECISIONS SHAPE THIS FILE.
//
// It is SEEDED AND DETERMINISTIC. Every case runs a fixed span of seeds and a
// failure names the one that produced it, so a reproduction is a number rather
// than "run it again and hope". A property test that cannot be replayed is a
// flake generator; tests/fuzz makes the same argument about its corpus.
//
// The generator produces DOCUMENTS THE READERS COULD HAVE PRODUCED, not
// arbitrary variant trees. A heading at level nine, a table row shorter than its
// grid, a list item holding nothing -- the IR type system permits all of these
// and no reader emits any of them. Whether a writer SURVIVES such a thing is a
// different question with a different answer, and tests/fuzz is where it is
// asked.
//
// It also declines to generate shapes THE TARGET FORMAT CANNOT SPELL. Word has
// no quote element and no list element: both are properties of a paragraph, and
// a paragraph has one style, so a heading inside a quote is either a heading or
// quoted and never both. Markdown cannot nest links, cannot mark a strikethrough
// that begins mid-word, and defines a tight list as one whose items are not
// separated by blank lines. Generating any of that would assert that a format
// can hold something it defines as impossible. Every such exclusion is written
// down where it is made, with the reason -- and each one was learned by watching
// this test fail rather than assumed in advance.
//
// Comparison is against WHAT THE FORMAT CAN HOLD, which is the "modulo
// declared-lossy fields" half of section 16's sentence. Expressible says what
// each format can express; the DOCX comparison flattens the inline tree to a run
// list, because WordprocessingML has no inline tree at all -- bold and italic
// are properties of a run, so Strikethrough{Text, Emphasis{Text}} and its
// flattened equivalent are the same document and no file distinguishes them.
//
// The dump is a normalised text tree rather than a recursive operator==, because
// when a round trip loses one table cell's alignment, `a == b` says false and a
// diff says where.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "morph/providers/docx.h"
#include "morph/providers/markdown.h"

using namespace morph;
using namespace morph::doc;

namespace {

// --- The generator -----------------------------------------------------------

// splitmix64: two lines, no state beyond a seed, and identical on every platform
// and every standard library. <random>'s engines are portable but its
// distributions are not, and a corpus that reproduces only on one machine is
// not a corpus.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed + 0x9E3779B97F4A7C15ull) {}

    std::uint64_t Next() {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // Half-open [0, bound).
    std::size_t Below(std::size_t bound) {
        return bound == 0 ? 0 : static_cast<std::size_t>(Next() % bound);
    }
    bool Chance(int percent) { return static_cast<int>(Below(100)) < percent; }

private:
    std::uint64_t state_;
};

// Words rather than random bytes. Text that survives a round trip through XML
// escaping is not the property under test here -- test_docx_reader covers the
// escaping -- and a random byte string makes every failure look like an encoding
// bug until you rule it out.
const char* const kWords[] = {"morph", "converts", "documents", "without", "losing",
                              "structure", "the", "planner", "chooses", "a", "route",
                              "fidelity", "is", "reported", "before", "anything", "runs"};

std::string Words(Rng& rng, std::size_t count) {
    std::string text;
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0) text += ' ';
        text += kWords[rng.Below(std::size(kWords))];
    }
    return text;
}

Inlines MakeInlines(Rng& rng, int depth);

Inline MakeInline(Rng& rng, int depth) {
    // Text dominates, as it does in a real document. A generator that produces
    // an even mix of eleven inline kinds spends every case on constructs that
    // appear once a chapter and never exercises a plain paragraph.
    const std::size_t roll = rng.Below(depth >= 2 ? 60 : 100);
    if (roll < 55) return Inline::Of(Text{Words(rng, 1 + rng.Below(6)), {}});
    if (roll < 62) return Inline::Of(InlineCode{Words(rng, 1 + rng.Below(2))});
    if (roll < 70) return Inline::Of(LineBreak{true});
    if (roll < 78) return Inline::Of(Emphasis{MakeInlines(rng, depth + 1)});
    if (roll < 86) return Inline::Of(Strong{MakeInlines(rng, depth + 1)});
    if (roll < 92) return Inline::Of(Strikethrough{MakeInlines(rng, depth + 1)});
    // A LINK INSIDE A LINK is not a document. HTML forbids nested anchors, a
    // Word hyperlink field cannot contain another, and Markdown's
    // "[a [b](x) c](y)" parses as neither -- so no reader can produce one and
    // generating it would test a shape that cannot exist.
    if (depth > 0) return Inline::Of(Text{Words(rng, 1 + rng.Below(4)), {}});
    Link link;
    link.target = "https://example.com/" + Words(rng, 1);
    link.children = MakeInlines(rng, depth + 1);
    return Inline::Of(std::move(link));
}

Inlines MakeInlines(Rng& rng, int depth) {
    // Text at both ends, always. A paragraph whose entire content is a line
    // break has no spelling in Markdown -- it comes out as an empty line, which
    // reads back as nothing at all -- and no reader produces one, so generating
    // it only manufactures failures about a document that cannot exist.
    //
    // Spans are also separated by a space rather than butted together. That is
    // not tidiness either: "~~" is the one Markdown delimiter that does not work
    // intraword, so a strikethrough with a letter against it cannot be written
    // at all -- the writer reports the loss and drops the marks, correctly, and
    // generating that shape would be asserting that Markdown can express
    // something it cannot.
    Inlines out;
    out.push_back(Inline::Of(Text{Words(rng, 1 + rng.Below(4)), {}}));
    const std::size_t count = rng.Below(depth >= 2 ? 2 : 4);
    for (std::size_t i = 0; i < count; ++i) {
        Inline node = MakeInline(rng, depth);
        // A space on each side of a strikethrough, and only there. "~~" is the
        // one Markdown delimiter that does not work intraword -- "aa*bb*cc" is
        // emphasis, "aa~~bb~~cc" is four literal tildes -- so a strikethrough
        // with a letter against it cannot be written at all. The writer reports
        // that loss and drops the marks, correctly; generating the shape would
        // be asserting Markdown can express something it cannot.
        const bool needsAir = node.Is<Strikethrough>();
        if (needsAir) out.push_back(Inline::Of(Text{" ", {}}));
        out.push_back(std::move(node));
        if (needsAir) out.push_back(Inline::Of(Text{" ", {}}));
    }
    if (out.size() > 1) out.push_back(Inline::Of(Text{Words(rng, 1 + rng.Below(3)), {}}));

    // A space touching a line break is not a space: Markdown strips leading and
    // trailing whitespace on every line, so the separators above have to give
    // way where a break already provides the separation.
    for (std::size_t i = 0; i < out.size();) {
        const auto* text = out[i].As<Text>();
        const bool lone = text != nullptr && text->text == " ";
        const bool nextBreak = i + 1 < out.size() && out[i + 1].Is<LineBreak>();
        const bool prevBreak = i > 0 && out[i - 1].Is<LineBreak>();
        if (lone && (nextBreak || prevBreak)) {
            out.erase(out.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
    return out;
}

// Where a block is being generated. Both flags exist because Word represents
// both containers as a PARAGRAPH PROPERTY -- numPr for a list item, a style
// name for a quote -- and a paragraph has one style, so some combinations have
// no spelling at all.
// What the lists at one depth of one top-level list tree have already been
// generated as. Word keeps both the marker format and the first number on the
// numbering definition for the LEVEL, which every sub-list at that depth shares,
// so two sub-lists under one parent that disagree are not a document any DOCX
// can hold -- the writer says so with a fidelity note rather than pretending.
// The generator claims a kind and a start per depth and reuses them, which is
// the shape a real document has.
struct LadderLevel {
    bool ordered = false;
    int start = 1;
};
using LevelLadder = std::map<int, LadderLevel>;

struct Where {
    bool insideItem = false;
    bool insideQuote = false;
    // The enclosing top-level list's ladder, and how deep in it this block sits.
    // Both meaningful only when insideItem is set; a top-level list starts a
    // ladder of its own.
    std::shared_ptr<LevelLadder> ladder;
    int listLevel = 0;
};

Blocks MakeBlocks(Rng& rng, int depth, std::size_t count, Where where = {});

// WHAT THIS DELIBERATELY DOES NOT GENERATE, and why each one is a scope decision
// rather than a bug being hidden:
//
//   A TABLE below the top level. Only the HTML reader produces one, and GFM has
//   no syntax for a table inside a list item at all, so both writers lift it
//   out. Whether they SAY they did is a fidelity question with its own tests.
//
//   A CODE BLOCK, QUOTE or RULE inside a list item. WordprocessingML represents
//   a list item as a paragraph carrying w:numPr, and a fenced block or a quote
//   is a different paragraph style -- it cannot be inside the item, only after
//   it. That is section 8.4's P3 territory and is explicitly out of scope.
//
// Both are asymmetries between the formats rather than failures to preserve
// what a format can hold, which is what this file is about.
Block MakeBlock(Rng& rng, int depth, Where where) {
    // Past depth 2 only leaves, so a run of unlucky rolls cannot generate a
    // document that takes a second to write. The nesting the readers actually
    // produce is shallow.
    std::size_t ceiling = 100;
    if (depth >= 2) ceiling = 55;
    else if (depth > 0) ceiling = 88;
    std::size_t roll = rng.Below(ceiling);
    // INSIDE A QUOTE, ONLY PARAGRAPHS. Word has no quote element: a quote is a
    // paragraph STYLE, and a paragraph carries exactly one style. So a heading
    // inside a quote is either a heading or quoted and never both; a list item
    // inside one collides with the ListParagraph style the writer gives items;
    // and a quote inside a quote has no second level to be at, because there is
    // only the one style name. All three leave the quote on the way through
    // DOCX. That is section 8.4's model rather than a defect in the writer, and
    // none of it is worth generating here.
    if (where.insideQuote) {
        roll = 10;  // a paragraph
    } else if (where.insideItem) {
        // A paragraph, a heading, or a nested list: the three things a numbered
        // paragraph in Word can also be. The bands below are 0-39, 40-51 and
        // 66-79 respectively.
        // The nested-list option disappears past depth 2. Without a ceiling the
        // recursion is bounded only by luck: a run of unlucky rolls builds a
        // list nested a hundred deep, which the readers refuse as malformed --
        // correctly, under section 15.2's nesting cap. Real documents are
        // shallow and the readers say so.
        // 10 is a paragraph, 70 a nested list. Chosen from this table rather
        // than by falling through the guards below, because a roll that skips
        // one branch lands in the next one -- which is how a fenced code block
        // ended up inside a list item while this was being written.
        //
        // A HEADING IS NOT HERE for the same reason it is not inside a quote:
        // "Heading 1" and the "ListParagraph" style the writer gives items are
        // both paragraph styles, and a paragraph has one.
        const std::size_t inItem[] = {10, 70};
        roll = inItem[rng.Below(depth < 2 ? 2 : 1)];
    }
    if (roll < 40) {
        Paragraph paragraph;
        paragraph.inlines = MakeInlines(rng, depth);
        return Block::Of(std::move(paragraph));
    }
    if (roll < 52) {
        Heading heading;
        heading.level = 1 + static_cast<int>(rng.Below(6));
        heading.inlines = MakeInlines(rng, depth + 1);
        return Block::Of(std::move(heading));
    }
    if (roll < 60) {
        return Block::Of(CodeBlock{rng.Chance(50) ? "cpp" : "", Words(rng, 4) + "\n"});
    }
    if (roll < 66) return Block::Of(HorizontalRule{});
    if (roll < 80) {
        ListBlock list;
        // A NESTED LIST IS FREE TO BE ITS OWN KIND and to start at its own
        // number -- a numbered list inside a bulleted one is written with an
        // abstractNum whose ladder of formats matches the tree, and a sub-list
        // beginning at 3 with a startOverride against that level. What it is not
        // free to do is DISAGREE WITH ITS COUSINS: the level is one w:lvl for the
        // whole tree, so the first list at a depth fixes both for the rest.
        auto ladder = where.ladder ? where.ladder : std::make_shared<LevelLadder>();
        auto claimed = ladder->find(where.listLevel);
        if (claimed == ladder->end()) {
            LadderLevel level;
            level.ordered = rng.Chance(50);
            level.start = level.ordered ? 1 + static_cast<int>(rng.Below(3)) : 1;
            claimed = ladder->emplace(where.listLevel, level).first;
        }
        list.ordered = claimed->second.ordered;
        list.start = claimed->second.start;
        const std::size_t items = 1 + rng.Below(4);
        // A single-item list cannot be loose: with one block in one item there
        // is nowhere to put the blank line that would make it so.
        list.tight = items == 1 ? true : rng.Chance(70);
        for (std::size_t i = 0; i < items; ++i) {
            ListItem item;
            // ONE BLOCK PER ITEM, for two separate reasons.
            //
            // CommonMark defines looseness from the source -- a list is loose
            // when its items are separated by blank lines, or when an item
            // directly contains two blocks with one between them -- so "tight
            // with two paragraphs" is not a document that can be spelled.
            //
            // And a second paragraph inside a Word list item is a CONTINUATION
            // PARAGRAPH: it carries the list style and the indentation but not
            // the numbering, because numbering it would render a second bullet.
            // Our reader does not reconstruct that yet -- it sees an unnumbered
            // paragraph and closes the list -- so a two-block item comes back as
            // an item, a paragraph, and a new list. That is a known limitation
            // rather than something to assert away here.
            const std::size_t blocks = 1;
            item.blocks = MakeBlocks(rng, depth + 1, blocks,
                                     Where{/*insideItem=*/true, where.insideQuote, ladder,
                                           where.listLevel + 1});
            list.items.push_back(std::move(item));
        }
        return Block::Of(std::move(list));
    }
    if (roll < 88) {
        return Block::Of(BlockQuote{
            MakeBlocks(rng, depth + 1, 1 + rng.Below(2), Where{false, /*insideQuote=*/true})});
    }

    // A rectangular table with a header row, which is what every reader
    // produces and what every writer can express.
    Table table;
    const std::size_t columns = 1 + rng.Below(3);
    const std::size_t rows = 1 + rng.Below(3);
    table.columns.resize(columns);
    table.headerRows = 1;
    for (std::size_t r = 0; r <= rows; ++r) {
        TableRow row;
        for (std::size_t c = 0; c < columns; ++c) {
            TableCell cell;
            cell.blocks.push_back(Block::Of(Paragraph{MakeInlines(rng, depth + 2), {}, {}, {}}));
            row.cells.push_back(std::move(cell));
        }
        table.rows.push_back(std::move(row));
    }
    return Block::Of(std::move(table));
}

Blocks MakeBlocks(Rng& rng, int depth, std::size_t count, Where where) {
    Blocks out;
    for (std::size_t i = 0; i < count; ++i) {
        Block block = MakeBlock(rng, depth, where);
        // TWO QUOTES IN A ROW ARE ONE QUOTE in Word, and so are two code
        // blocks. Both are paragraph STYLES rather than containers: a run of
        // consecutive paragraphs carrying the style IS the block, so where one
        // ends and the next begins is not written down anywhere. Markdown can
        // tell them apart -- a blank line ends a quote, a fence ends a listing --
        // but the IR has to survive both formats, and this file is not the place
        // to assert that Word can do something it cannot.
        const bool repeatsStyleRun =
            !out.empty() && ((block.Is<BlockQuote>() && out.back().Is<BlockQuote>()) ||
                             (block.Is<CodeBlock>() && out.back().Is<CodeBlock>()));
        if (repeatsStyleRun) {
            block = Block::Of(Paragraph{MakeInlines(rng, depth), {}, {}, {}});
        }
        out.push_back(std::move(block));
    }
    return out;
}

Document MakeDocument(std::uint64_t seed) {
    Rng rng(seed);
    Document document;
    document.blocks = MakeBlocks(rng, 0, 1 + rng.Below(8));
    // The generator stands in for a reader, so it owes what a reader owes: the
    // IR's canonical form (document.h). Without this it produces documents no
    // reader would, and the round trip fails on the normalisation rather than on
    // anything either provider did.
    Normalise(document.blocks);
    return document;
}

// --- The comparison ----------------------------------------------------------

// What a writer is allowed to lose, declared per format instead of assumed. This
// is the "modulo declared-lossy fields" half of section 16's sentence, and
// writing it as a struct rather than as a pile of `if (format == ...)` is what
// keeps the dump function readable.
struct Expressible {
    bool headingLevelsAboveSix = true;
    bool listStart = true;   // Markdown writes "1." for a list starting at 3
    bool tightness = true;   // DOCX has no concept of a tight list
    bool codeLanguage = true;
    // WordprocessingML HAS NO INLINE TREE. Bold and italic are properties of a
    // run, so Strikethrough{Text, Emphasis{Text}} and its flattened equivalent
    // are the same document as far as the format is concerned -- there is no
    // file that distinguishes them. Comparing a DOCX round trip as a tree tests
    // the reader's reconstruction preferences, not whether anything was lost;
    // comparing it as a run list tests the actual question.
    bool inlineNesting = true;
    // A Markdown heading is one line, so a break inside one becomes a space and
    // the writer says so. Word has no such limit.
    bool headingLineBreaks = true;
};

constexpr Expressible kDocx{.headingLevelsAboveSix = true,
                            .listStart = true,
                            .tightness = false,
                            .codeLanguage = false,
                            .inlineNesting = false};
constexpr Expressible kMarkdown{.headingLevelsAboveSix = true,
                                .listStart = true,
                                .tightness = true,
                                .codeLanguage = true,
                                .inlineNesting = true,
                                .headingLineBreaks = false};

void DumpBlocks(const Blocks& blocks, const Expressible& can, int indent, std::ostream& out);

// The inline tree as a flat list of runs, which is the model WordprocessingML
// actually has: a run is some text plus a set of marks, and there is no nesting
// to preserve or lose. Collecting first and printing second is what lets two
// runs that ended up with the same marks be merged, exactly as a reader merges
// them.
struct Run {
    std::vector<std::string> marks;  // sorted
    std::string text;
    // A code span and a link are their own kinds of run rather than marks: they
    // carry content (a target) or a different escaping discipline, so merging
    // them into a neighbour would lose something real.
    std::string kind = "run";
    std::string detail;
};

void CollectRuns(const Inlines& inlines, std::vector<std::string> marks,
                 std::vector<Run>& into) {
    std::sort(marks.begin(), marks.end());

    const auto append = [&](Run run) {
        if (run.kind == "run" && !into.empty() && into.back().kind == "run" &&
            into.back().marks == run.marks) {
            into.back().text += run.text;
            return;
        }
        into.push_back(std::move(run));
    };

    for (const Inline& one : inlines) {
        if (const auto* text = one.As<Text>()) {
            append(Run{marks, text->text});
        } else if (const auto* code = one.As<InlineCode>()) {
            append(Run{marks, code->text, "code"});
        } else if (one.Is<LineBreak>()) {
            // NO MARKS ON A BREAK. A line break has no content to be emphasised,
            // and Word puts the <w:br/> inside a run that carries the run's
            // properties -- so "an italic break" is a distinction the format can
            // represent and no reader, writer or human can act on.
            append(Run{{}, "", "break"});
        } else if (const auto* e = one.As<Emphasis>()) {
            std::vector<std::string> inner = marks;
            inner.emplace_back("emphasis");
            CollectRuns(e->children, std::move(inner), into);
        } else if (const auto* s = one.As<Strong>()) {
            std::vector<std::string> inner = marks;
            inner.emplace_back("strong");
            CollectRuns(s->children, std::move(inner), into);
        } else if (const auto* d = one.As<Strikethrough>()) {
            std::vector<std::string> inner = marks;
            inner.emplace_back("strike");
            CollectRuns(d->children, std::move(inner), into);
        } else if (const auto* link = one.As<Link>()) {
            append(Run{marks, "", "link", link->target});
            CollectRuns(link->children, marks, into);
            append(Run{marks, "", "/link"});
        } else {
            append(Run{marks, "", "other-inline"});
        }
    }
}

void DumpRuns(const Inlines& inlines, int indent, std::ostream& out) {
    std::vector<Run> runs;
    CollectRuns(inlines, {}, runs);
    const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
    for (const Run& run : runs) {
        out << pad << run.kind << " [";
        for (const std::string& mark : run.marks) out << mark << " ";
        out << "]";
        if (!run.detail.empty()) out << " " << run.detail;
        if (!run.text.empty()) out << " " << run.text;
        out << "\n";
    }
}

// Dispatches on what the target format can hold: a tree when it has one, a run
// list when it does not.
void DumpContent(const Inlines& inlines, const Expressible& can, int indent,
                 std::ostream& out);

void DumpInlines(const Inlines& inlines, int indent, std::ostream& out) {
    const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');

    // ADJACENT TEXT RUNS ARE ONE RUN. Every reader coalesces them -- no format
    // here has syntax for "two text nodes touching" -- so a generator that emits
    // two is asking a question the formats cannot answer. Normalising in the
    // comparison rather than in the generator keeps the generator free to
    // produce anything the IR permits.
    std::string pending;
    const auto flush = [&] {
        if (pending.empty()) return;
        out << pad << "text " << pending << "\n";
        pending.clear();
    };

    for (const Inline& one : inlines) {
        if (const auto* text = one.As<Text>()) {
            pending += text->text;
            continue;
        }
        flush();
        if (const auto* code = one.As<InlineCode>()) {
            out << pad << "code " << code->text << "\n";
        } else if (one.Is<LineBreak>()) {
            out << pad << "break\n";
        } else if (one.Is<Emphasis>() || one.Is<Strong>() || one.Is<Strikethrough>()) {
            // NESTING ORDER IS NOT INFORMATION. Bold-inside-italic and
            // italic-inside-bold render identically and mean the same thing, and
            // WordprocessingML cannot tell them apart at all: they are two
            // properties on one run. So a chain of wrappers is emitted in a
            // canonical order rather than the order it was built in.
            std::vector<std::string> chain;
            const Inline* node = &one;
            const Inlines* children = nullptr;
            for (;;) {
                if (const auto* e = node->As<Emphasis>()) {
                    chain.emplace_back("emphasis");
                    children = &e->children;
                } else if (const auto* s = node->As<Strong>()) {
                    chain.emplace_back("strong");
                    children = &s->children;
                } else if (const auto* d = node->As<Strikethrough>()) {
                    chain.emplace_back("strike");
                    children = &d->children;
                } else {
                    break;
                }
                // Only an unbranched chain can be reordered. A wrapper with two
                // children is a real tree and its shape is the document's.
                if (children->size() != 1) break;
                node = &children->front();
            }
            std::sort(chain.begin(), chain.end());
            int at = indent;
            for (const std::string& kind : chain) {
                out << std::string(static_cast<std::size_t>(at) * 2, ' ') << kind << "\n";
                ++at;
            }
            DumpInlines(*children, at, out);
        } else if (const auto* link = one.As<Link>()) {
            out << pad << "link " << link->target << "\n";
            DumpInlines(link->children, indent + 1, out);
        } else {
            out << pad << "other-inline\n";
        }
    }
    flush();
}

void DumpBlock(const Block& block, const Expressible& can, int indent, std::ostream& out) {
    const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
    if (const auto* paragraph = block.As<Paragraph>()) {
        out << pad << "paragraph\n";
        DumpContent(paragraph->inlines, can, indent + 1, out);
    } else if (const auto* heading = block.As<Heading>()) {
        out << pad << "heading " << heading->level << "\n";
        if (can.headingLineBreaks) {
            DumpContent(heading->inlines, can, indent + 1, out);
        } else {
            // Compared against what the format can hold. Asserting the break
            // survives would be asserting that Markdown has multi-line headings.
            Inlines flattened = heading->inlines;
            for (Inline& node : flattened) {
                if (node.Is<LineBreak>()) node = Inline::Of(Text{" ", {}});
            }
            Normalise(flattened);
            DumpContent(flattened, can, indent + 1, out);
        }
    } else if (const auto* code = block.As<CodeBlock>()) {
        out << pad << "codeblock";
        if (can.codeLanguage) out << " lang=" << code->language;
        out << "\n" << pad << "  " << code->text;
    } else if (block.Is<HorizontalRule>()) {
        out << pad << "rule\n";
    } else if (const auto* quote = block.As<BlockQuote>()) {
        out << pad << "quote\n";
        DumpBlocks(quote->blocks, can, indent + 1, out);
    } else if (const auto* list = block.As<ListBlock>()) {
        out << pad << "list" << (list->ordered ? " ordered" : " bullet");
        if (can.listStart && list->ordered) out << " start=" << list->start;
        if (can.tightness) out << (list->tight ? " tight" : " loose");
        out << "\n";
        for (const ListItem& item : list->items) {
            out << pad << "  item\n";
            DumpBlocks(item.blocks, can, indent + 2, out);
        }
    } else if (const auto* table = block.As<Table>()) {
        out << pad << "table " << table->columns.size() << "x" << table->rows.size()
            << " header=" << table->headerRows << "\n";
        for (const TableRow& row : table->rows) {
            out << pad << "  row\n";
            for (const TableCell& cell : row.cells) {
                if (cell.covered) continue;
                out << pad << "    cell\n";
                DumpBlocks(cell.blocks, can, indent + 3, out);
            }
        }
    } else {
        out << pad << "other-block\n";
    }
}

void DumpBlocks(const Blocks& blocks, const Expressible& can, int indent, std::ostream& out) {
    for (const Block& block : blocks) DumpBlock(block, can, indent, out);
}

void DumpContent(const Inlines& inlines, const Expressible& can, int indent,
                 std::ostream& out) {
    if (can.inlineNesting) {
        DumpInlines(inlines, indent, out);
    } else {
        DumpRuns(inlines, indent, out);
    }
}

std::string Dump(const Document& document, const Expressible& can) {
    std::ostringstream out;
    DumpBlocks(document.blocks, can, 0, out);
    return out.str();
}

// The first place two dumps diverge, with a few lines either side. Catch2 prints
// what it is given, and what it was given used to be four hundred lines of two
// nearly identical trees -- which is a way of hiding a one-line difference in
// plain sight.
std::string FirstDifference(const std::string& before, const std::string& after) {
    const auto split = [](const std::string& text) {
        std::vector<std::string> lines;
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line)) lines.push_back(line);
        return lines;
    };
    const std::vector<std::string> a = split(before);
    const std::vector<std::string> b = split(after);

    std::size_t at = 0;
    while (at < a.size() && at < b.size() && a[at] == b[at]) ++at;

    std::ostringstream out;
    out << "first difference at line " << (at + 1) << "\n";
    const std::size_t from = at > 3 ? at - 3 : 0;
    for (std::size_t i = from; i < at; ++i) out << "    " << a[i] << "\n";
    out << "  wrote:     " << (at < a.size() ? a[at] : "(end)") << "\n";
    out << "  read back: " << (at < b.size() ? b[at] : "(end)") << "\n";
    for (std::size_t i = at + 1; i < std::min(a.size(), at + 4); ++i) {
        out << "    " << a[i] << "  |  " << (i < b.size() ? b[i] : "(end)") << "\n";
    }
    return out.str();
}

// --- The round trips ---------------------------------------------------------

// Every case runs this span. Wide enough that a rare shape shows up, short
// enough that the suite stays under a second: the fuzz corpus is where breadth
// belongs, and this is where the shapes readers produce belong.
constexpr std::uint64_t kSeeds = 200;

// The two shapes the generator used to be forbidden to produce, counted so the
// round trips below cannot quietly stop exercising them. A generator restriction
// removed in the writer but left in place here would look exactly like a passing
// test.
struct NestedShapes {
    int differentKind = 0;  // a numbered list inside a bulleted one, or vice versa
    int ownStart = 0;       // a sub-list beginning at something other than 1
};

void CountNested(const Blocks& blocks, const ListBlock* parent, NestedShapes& into);

void CountNestedInList(const ListBlock& list, const ListBlock* parent, NestedShapes& into) {
    if (parent != nullptr) {
        if (list.ordered != parent->ordered) ++into.differentKind;
        if (list.start != 1) ++into.ownStart;
    }
    for (const ListItem& item : list.items) CountNested(item.blocks, &list, into);
}

void CountNested(const Blocks& blocks, const ListBlock* parent, NestedShapes& into) {
    for (const Block& block : blocks) {
        if (const auto* list = block.As<ListBlock>()) {
            CountNestedInList(*list, parent, into);
        } else if (const auto* quote = block.As<BlockQuote>()) {
            CountNested(quote->blocks, nullptr, into);
        }
    }
}

}  // namespace

TEST_CASE("The seed span reaches the nested lists that used to be excluded",
          "[property][docx]") {
    NestedShapes shapes;
    for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
        CountNested(MakeDocument(seed).blocks, nullptr, shapes);
    }
    INFO("different kind: " << shapes.differentKind << ", own start: " << shapes.ownStart);
    CHECK(shapes.differentKind > 0);
    CHECK(shapes.ownStart > 0);
}

TEST_CASE("A document survives a DOCX round trip", "[property][docx]") {
    for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
        const Document original = MakeDocument(seed);

        std::vector<std::byte> package;
        const Result written = docooxml::WriteDocx(original, {}, package);
        INFO("seed " << seed << ", writer said: " << written.message());
        REQUIRE(written.ok());

        Document read;
        const Result parsed = docooxml::ReadDocx(package, {}, read);
        INFO("seed " << seed << ", reader said: " << parsed.message());
        REQUIRE(parsed.ok());

        const std::string before = Dump(original, kDocx);
        const std::string after = Dump(read, kDocx);
        REQUIRE(before == after);
    }
}

TEST_CASE("A document survives a Markdown round trip", "[property][markdown]") {
    for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
        const Document original = MakeDocument(seed);

        std::string markdown;
        const Result written = docmd::WriteMarkdown(original, {}, markdown);
        INFO("seed " << seed << ", writer said: " << written.message());
        REQUIRE(written.ok());

        Document read;
        const Result parsed = docmd::ReadMarkdown(markdown, {}, read);
        INFO("seed " << seed << ", reader said: " << parsed.message());
        REQUIRE(parsed.ok());

        const std::string before = Dump(original, kMarkdown);
        const std::string after = Dump(read, kMarkdown);
        INFO("seed " << seed << "\n" << FirstDifference(before, after));
        REQUIRE(before == after);
    }
}

TEST_CASE("A second hop loses nothing the first one kept", "[property][docx][markdown]") {
    // The graph's whole argument (section 8.2) is that a route through the IR
    // composes: md -> IR -> docx -> IR -> md must not be worse than
    // md -> IR -> md, or a two-hop plan is quietly destructive and the fidelity
    // contract is promising something it cannot honour.
    //
    // Compared as structure under the DOCX capability set rather than as
    // Markdown text. The two are not the same question: Word cannot record a
    // code block's language, so the second pass legitimately writes a fence
    // without "cpp" on it, and comparing the strings would call that a failure.
    for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
        const Document original = MakeDocument(seed);

        std::string once;
        REQUIRE(docmd::WriteMarkdown(original, {}, once).ok());
        Document afterOnce;
        REQUIRE(docmd::ReadMarkdown(once, {}, afterOnce).ok());

        std::vector<std::byte> package;
        REQUIRE(docooxml::WriteDocx(afterOnce, {}, package).ok());
        Document afterDocx;
        REQUIRE(docooxml::ReadDocx(package, {}, afterDocx).ok());

        const std::string before = Dump(afterOnce, kDocx);
        const std::string after = Dump(afterDocx, kDocx);
        INFO("seed " << seed << "\n" << FirstDifference(before, after));
        REQUIRE(before == after);
    }
}
