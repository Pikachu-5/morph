// IR -> Markdown. Section 8.3.
//
// "The writer's real work is escaping and reference management: link reference
//  definitions, footnote numbering, and correct escaping of literal *, _, [
//  inside text runs. Get the escaping wrong and you produce Markdown that does
//  not round-trip, which is the one thing a converter must never do."
//
// So the escaping rules below are the most correctness-critical code in this
// provider, and EscapeText is exposed in the header specifically so they can be
// tested directly rather than only through whole documents.

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "morph/providers/markdown.h"

namespace morph::docmd {
namespace {

using namespace morph::doc;

bool IsAsciiDigit(char c) { return c >= '0' && c <= '9'; }

// Could this run of text, placed at the start of a line, be mistaken for a block
// marker? Only these can, so only these need escaping there -- escaping them
// mid-line would be noise.
bool StartsBlockConstruct(std::string_view text, std::size_t at) {
    const char c = text[at];
    if (c == '#' || c == '>') return true;
    if (c == '-' || c == '+' || c == '*') {
        // A bullet marker needs whitespace or end-of-line after it. "-5 degrees"
        // is not a list.
        return at + 1 >= text.size() || text[at + 1] == ' ' || text[at + 1] == '\t';
    }
    if (c == '=') return true;  // setext underline
    if (c == '~' || c == '`') return true;
    if (IsAsciiDigit(c)) {
        // An ordered list marker: digits then '.' or ')' then whitespace.
        std::size_t i = at;
        while (i < text.size() && IsAsciiDigit(text[i])) ++i;
        if (i >= text.size() || (text[i] != '.' && text[i] != ')')) return false;
        ++i;
        return i >= text.size() || text[i] == ' ' || text[i] == '\t';
    }
    return false;
}

}  // namespace

std::string EscapeText(std::string_view text, bool atLineStart, Flavour flavour) {
    std::string out;
    out.reserve(text.size() + text.size() / 8);

    bool at_line_start = atLineStart;

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];

        if (c == '\n') {
            out.push_back(c);
            at_line_start = true;
            continue;
        }

        // Leading whitespace does not end the line-start context; four spaces
        // would start an indented code block, so it is escaped below rather than
        // silently emitted.
        const bool line_start_here = at_line_start && (c == ' ' || c == '\t');

        if (at_line_start && !line_start_here && StartsBlockConstruct(text, i)) {
            out.push_back('\\');
        }

        switch (c) {
            // Always ambiguous, anywhere on the line.
            case '\\':
            case '`':
            case '*':
            case '[':
            case ']':
            case '<':
                out.push_back('\\');
                out.push_back(c);
                break;

            case '_':
                // Intraword underscores are literal in CommonMark and in GFM, so
                // "snake_case_name" needs no escaping and looks far better
                // without it. Portable escapes anyway: some renderers get this
                // wrong and the flavour exists precisely to avoid those.
                if (flavour == Flavour::kPortable || i == 0 || i + 1 >= text.size() ||
                    !(std::isalnum(static_cast<unsigned char>(text[i - 1])) &&
                      std::isalnum(static_cast<unsigned char>(text[i + 1])))) {
                    out.push_back('\\');
                }
                out.push_back(c);
                break;

            case '&':
                // Only ambiguous when it actually looks like an entity or a
                // numeric reference; escaping every ampersand is unreadable.
                {
                    std::size_t j = i + 1;
                    if (j < text.size() && text[j] == '#') ++j;
                    const std::size_t name_start = j;
                    while (j < text.size() &&
                           std::isalnum(static_cast<unsigned char>(text[j]))) {
                        ++j;
                    }
                    if (j > name_start && j < text.size() && text[j] == ';') out.push_back('\\');
                }
                out.push_back(c);
                break;

            case '!':
                // Only ambiguous immediately before a '[', where it would begin
                // an image.
                if (i + 1 < text.size() && text[i + 1] == '[') out.push_back('\\');
                out.push_back(c);
                break;

            case '|':
                // A pipe splits cells in a GFM table. We do not know from here
                // whether we are inside one, and escaping it outside a table is
                // harmless, so escape unconditionally in the flavours that have
                // tables.
                if (flavour != Flavour::kCommonMark) out.push_back('\\');
                out.push_back(c);
                break;

            case '~':
                if (flavour != Flavour::kCommonMark) out.push_back('\\');
                out.push_back(c);
                break;

            default:
                out.push_back(c);
                break;
        }

        if (!line_start_here) at_line_start = false;
    }
    return out;
}

namespace {

// --- The writer --------------------------------------------------------------

class Writer {
public:
    Writer(const Document& document, Flavour flavour, Result& result)
        : document_(document), flavour_(flavour), result_(result) {}

    void WriteDocument();

    [[nodiscard]] std::string Take() { return std::move(out_); }

private:
    // `tight` suppresses the blank line BETWEEN blocks, which is what keeps a
    // tight list tight: CommonMark decides looseness from the source, so a
    // blank line between an item's paragraph and its nested list turns the
    // whole list loose on the way back in.
    void WriteBlocks(const Blocks& blocks, std::size_t depth, bool tight = false);
    void WriteBlock(const Block& block, std::size_t depth);
    void WriteParagraph(const Paragraph& paragraph);
    void WriteHeading(const Heading& heading);
    void WriteCode(const CodeBlock& code);
    void WriteQuote(const BlockQuote& quote, std::size_t depth);
    void WriteList(const ListBlock& list, std::size_t depth);
    void WriteTable(const Table& table);
    void WriteImageBlock(const ImageBlock& image);
    void WriteFootnote(const Footnote& footnote, std::size_t depth);
    void WriteRaw(const RawBlock& raw);

    void WriteInlines(const Inlines& inlines, bool atLineStart);
    void WriteInline(const Inline& node, bool atLineStart);

    // Renders inlines to a string instead of to the output, by lending them the
    // whole emitter and taking it back. Table cells need this: they are laid out
    // as a grid, so their contents have to exist as measurable strings before
    // any of them is written.
    [[nodiscard]] std::string Capture(const Inlines& inlines);

    // Everything written while a prefix is active gets that prefix after each
    // newline. This is how nesting works for quotes and list items without a
    // second pass: "> " for a quote, "  " for a list continuation.
    void PushPrefix(std::string prefix) { prefixes_.push_back(std::move(prefix)); }
    void PopPrefix() { prefixes_.pop_back(); }

    void Emit(std::string_view text);
    void NewLine();
    void BlankLine();

    [[nodiscard]] std::string Prefix() const;

    void Note(std::string code, std::string detail, Fidelity incurred);

    const Document& document_;
    Flavour flavour_;
    Result& result_;

    std::string out_;
    std::vector<std::string> prefixes_;
    bool atLineStart_ = true;
    // TWO SIBLING LISTS ARE ONE LIST in Markdown unless the marker changes: a
    // bullet list followed by a bullet list, blank line or not, parses as a
    // single loose list. CommonMark's rule that a different bullet character
    // starts a new list is the only way to keep them apart, so sibling lists
    // alternate markers. Without this, the two lists merge and the survivor
    // turns loose -- which is exactly what a round trip reports.
    bool alternateListMarker_ = false;
    // Inside an ATX heading, where a line break cannot be written at all.
    bool inHeading_ = false;
    // Whether the inline being written is immediately followed by a word
    // character. Only the strikethrough delimiter cares, and only WriteInlines
    // can see it, so it is passed down here rather than through every signature.
    bool nextIsWord_ = false;
    bool pendingBlankLine_ = false;

    // Footnote definitions are collected as they are referenced and emitted at
    // the end, numbered in reference order. That is what every renderer expects
    // and it keeps the numbering stable across a round trip.
    std::vector<const Footnote*> footnotes_;
    std::vector<std::string> footnoteIds_;
};

std::string Writer::Capture(const Inlines& inlines) {
    std::string saved;
    saved.swap(out_);
    std::vector<std::string> savedPrefixes;
    savedPrefixes.swap(prefixes_);
    const bool savedAtLineStart = atLineStart_;
    atLineStart_ = false;

    WriteInlines(inlines, /*atLineStart=*/false);

    std::string captured;
    captured.swap(out_);
    out_.swap(saved);
    prefixes_.swap(savedPrefixes);
    atLineStart_ = savedAtLineStart;
    return captured;
}

std::string Writer::Prefix() const {
    std::string prefix;
    for (const std::string& part : prefixes_) prefix += part;
    return prefix;
}

void Writer::Emit(std::string_view text) {
    for (const char c : text) {
        if (c == '\n') {
            out_.push_back('\n');
            atLineStart_ = true;
            continue;
        }
        if (atLineStart_) {
            out_ += Prefix();
            atLineStart_ = false;
        }
        out_.push_back(c);
    }
}

void Writer::NewLine() {
    if (!atLineStart_) {
        out_.push_back('\n');
        atLineStart_ = true;
    }
}

void Writer::BlankLine() {
    NewLine();
    // A blank line inside a quote still needs the quote marker, or the quote
    // ends there. Trailing whitespace on that marker is stripped, because it is
    // invisible and some linters complain.
    std::string prefix = Prefix();
    while (!prefix.empty() && (prefix.back() == ' ' || prefix.back() == '\t')) prefix.pop_back();
    out_ += prefix;
    out_.push_back('\n');
    atLineStart_ = true;
}

void Writer::Note(std::string code, std::string detail, Fidelity incurred) {
    result_.Note(std::move(code), std::move(detail));
    result_.Incurred(incurred);
}

void Writer::WriteDocument() {
    // YAML front matter for metadata Markdown has nowhere else to put. Only
    // emitted when there is something to say, because an empty block at the top
    // of every file would be noise.
    const Metadata& meta = document_.meta;
    if (!meta.title.empty() || !meta.authors.empty() || !meta.lang.empty()) {
        Emit("---\n");
        if (!meta.title.empty()) Emit("title: " + meta.title + "\n");
        if (meta.authors.size() == 1) {
            Emit("author: " + meta.authors.front() + "\n");
        } else if (!meta.authors.empty()) {
            Emit("authors:\n");
            for (const std::string& author : meta.authors) Emit("  - " + author + "\n");
        }
        if (!meta.lang.empty()) Emit("lang: " + meta.lang + "\n");
        Emit("---\n\n");
    }

    WriteBlocks(document_.blocks, 0);

    if (!footnotes_.empty()) {
        BlankLine();
        for (std::size_t i = 0; i < footnotes_.size(); ++i) {
            Emit("[^" + std::to_string(i + 1) + "]: ");
            PushPrefix("    ");
            WriteBlocks(footnotes_[i]->blocks, 0);
            PopPrefix();
            NewLine();
        }
    }

    // Exactly one trailing newline. Editors add one, diffs expect one, and
    // "no newline at end of file" is a needless round-trip difference.
    while (!out_.empty() && out_.back() == '\n') out_.pop_back();
    if (!out_.empty()) out_.push_back('\n');
}

void Writer::WriteBlocks(const Blocks& blocks, std::size_t depth, bool tight) {
    if (depth > kMaxBlockDepth) {
        Note("nesting-truncated", "Content nested deeper than 100 levels was omitted.",
             Fidelity::Structural);
        return;
    }
    bool first = true;
    const Block* previous = nullptr;
    for (const Block& block : blocks) {
        if (const auto* list = block.As<ListBlock>()) {
            const auto* before = previous != nullptr ? previous->As<ListBlock>() : nullptr;
            alternateListMarker_ =
                before != nullptr && before->ordered == list->ordered && !alternateListMarker_;
        }
        previous = &block;
        if (!first) {
            // Only before a NESTED LIST. Two paragraphs with no blank line
            // between them are one paragraph, and CommonMark's own rule is that
            // an item directly containing two blocks separated by a blank line
            // makes the list loose -- so that shape cannot be written tight, and
            // pretending otherwise merges the user's paragraphs together.
            if (tight && block.Is<ListBlock>()) {
                NewLine();
            } else {
                BlankLine();
            }
        }
        first = false;
        WriteBlock(block, depth);
    }
}

void Writer::WriteBlock(const Block& block, std::size_t depth) {
    std::visit(Overloaded{
                   [&](const Paragraph& b) { WriteParagraph(b); },
                   [&](const Heading& b) { WriteHeading(b); },
                   [&](const CodeBlock& b) { WriteCode(b); },
                   [&](const BlockQuote& b) { WriteQuote(b, depth + 1); },
                   [&](const ListBlock& b) { WriteList(b, depth + 1); },
                   [&](const Table& b) { WriteTable(b); },
                   [&](const ImageBlock& b) { WriteImageBlock(b); },
                   [&](const HorizontalRule&) {
                       // "***" when this is the first thing in the file. A
                       // leading "---" is how every static-site generator and
                       // most Markdown readers -- ours included until this was
                       // found -- announce YAML front matter, so a document that
                       // opens with a thematic break and has another one further
                       // down loses everything between them. All three spellings
                       // are the same thematic break; only this one is
                       // unambiguous at the top of a file.
                       Emit(out_.empty() ? "***" : "---");
                       NewLine();
                   },
                   [&](const Footnote& b) { WriteFootnote(b, depth); },
                   [&](const RawBlock& b) { WriteRaw(b); },
               },
               block.node);
}

void Writer::WriteParagraph(const Paragraph& paragraph) {
    WriteInlines(paragraph.inlines, /*atLineStart=*/true);
    NewLine();
}

void Writer::WriteHeading(const Heading& heading) {
    // ATX only. Setext can express levels 1 and 2 and nothing else, so using it
    // would make the writer's output depend on the heading level for no gain.
    Emit(std::string(static_cast<std::size_t>(std::clamp(heading.level, 1, 6)), '#'));
    Emit(" ");
    // AN ATX HEADING IS ONE LINE. A line break inside one has no spelling: the
    // backslash we would emit ends the heading and the rest becomes a
    // paragraph, so the document gains a heading it did not have and a stray
    // backslash in the text. DOCX and HTML both produce this shape -- a heading
    // with a <br> in it is ordinary -- so it is handled rather than avoided.
    inHeading_ = true;
    WriteInlines(heading.inlines, /*atLineStart=*/false);
    inHeading_ = false;
    NewLine();
}

void Writer::WriteCode(const CodeBlock& code) {
    // The fence must be longer than the longest backtick run inside, or the
    // block terminates early and the rest of the document becomes code.
    std::size_t longest = 0;
    std::size_t run = 0;
    for (const char c : code.text) {
        run = (c == '`') ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    const std::string fence(std::max<std::size_t>(3, longest + 1), '`');

    Emit(fence);
    if (!code.language.empty()) {
        // An info string cannot contain a backtick, and a language with a space
        // in it would be read as extra info. Neither is worth failing over.
        std::string language = code.language;
        std::erase(language, '`');
        Emit(language);
    }
    NewLine();
    Emit(code.text);
    NewLine();
    Emit(fence);
    NewLine();
}

void Writer::WriteQuote(const BlockQuote& quote, std::size_t depth) {
    PushPrefix("> ");
    WriteBlocks(quote.blocks, depth);
    PopPrefix();
    NewLine();
}

void Writer::WriteList(const ListBlock& list, std::size_t depth) {
    // Read once: writing the items recurses, and a nested list would otherwise
    // consume the flag meant for this one.
    const bool alternate = alternateListMarker_;
    int number = list.start;

    for (std::size_t i = 0; i < list.items.size(); ++i) {
        const ListItem& item = list.items[i];
        if (i > 0 && !list.tight) BlankLine();
        if (i > 0 && list.tight) NewLine();

        std::string marker;
        if (list.ordered) {
            // ')' rather than '.' for an alternating sibling: both are ordered
            // markers and the change is what separates the two lists.
            marker = std::to_string(number++) + (alternate ? ") " : ". ");
        } else {
            // Portable uses '-' throughout: '*' is also emphasis and '+' is
            // handled inconsistently by older renderers. '+' is the alternate
            // anyway, because it is only reached when the alternative is two
            // lists silently becoming one.
            marker = alternate ? "+ " : "- ";
        }
        Emit(marker);

        if (item.checked.has_value()) {
            if (flavour_ == Flavour::kCommonMark) {
                Note("task-list-flattened",
                     "Task list checkboxes are not part of CommonMark and were written as text.",
                     Fidelity::Structural);
                Emit(*item.checked ? "[x] " : "[ ] ");
            } else {
                Emit(*item.checked ? "[x] " : "[ ] ");
            }
        }

        // Continuation lines line up under the text, not under the marker.
        PushPrefix(std::string(marker.size(), ' '));
        WriteBlocks(item.blocks, depth, list.tight);
        PopPrefix();
        NewLine();
    }
}

void Writer::WriteTable(const Table& table) {
    if (flavour_ == Flavour::kCommonMark) {
        Note("table-dropped",
             "CommonMark has no table syntax, so a table was written as plain paragraphs.",
             Fidelity::Structural);
        for (const TableRow& row : table.rows) {
            for (const TableCell& cell : row.cells) {
                if (cell.covered) continue;
                WriteBlocks(cell.blocks, 0);
                NewLine();
            }
        }
        return;
    }

    if (table.rows.empty()) return;

    // GFM tables require a header row. When the source had none we emit an empty
    // one rather than dropping the first data row, which is what a naive writer
    // does and what users notice immediately.
    const bool synthetic_header = table.headerRows == 0;
    if (synthetic_header) {
        Note("table-header-synthesised",
             "This table had no header row; an empty one was added because GitHub Markdown "
             "requires it.",
             Fidelity::Structural);
    }

    // Cell text, rendered once so column widths can be measured. Markdown tables
    // cannot express spans, so a covered cell becomes an empty one.
    std::vector<std::vector<std::string>> grid;
    std::size_t columns = 0;
    bool lost_spans = false;
    bool lost_blocks = false;

    for (const TableRow& row : table.rows) {
        std::vector<std::string> cells;
        for (const TableCell& cell : row.cells) {
            if (cell.colSpan > 1 || cell.rowSpan > 1) lost_spans = true;
            if (cell.covered) {
                cells.emplace_back();
                continue;
            }
            if (cell.blocks.size() > 1 ||
                (cell.blocks.size() == 1 && !cell.blocks.front().Is<Paragraph>())) {
                lost_blocks = true;
            }

            // A cell holding one paragraph keeps its formatting. GFM allows
            // emphasis, code spans and links inside a cell, and flattening them
            // to plain text loses information the format can hold -- which is
            // rule 1 in document.h read backwards. Anything more complicated
            // than a paragraph still flattens, and says so above.
            // A PIPE INSIDE A CODE SPAN cannot be escaped. Backslash escapes
            // do not apply inside a code span, so "`a\|b`" is read back with
            // the backslash still in it -- and an unescaped pipe would split the
            // row. GFM's table extension unescapes it before inline parsing;
            // md4c, which is our reader, does not. A writer that emits something
            // its own reader cannot read is a bug whatever the specification
            // says, so such a cell is flattened to plain text, where the escape
            // does work, and the loss is reported.
            bool unescapablePipe = false;
            if (cell.blocks.size() == 1 && cell.blocks.front().Is<Paragraph>()) {
                ForEachInline(cell.blocks.front().As<Paragraph>()->inlines,
                              [&](const Inline& node) {
                                  const auto* code = node.As<InlineCode>();
                                  if (code != nullptr &&
                                      code->text.find('|') != std::string::npos) {
                                      unescapablePipe = true;
                                  }
                              });
            }
            if (unescapablePipe) lost_blocks = true;

            std::string text;
            if (!unescapablePipe && cell.blocks.size() == 1 &&
                cell.blocks.front().Is<Paragraph>()) {
                text = Capture(cell.blocks.front().As<Paragraph>()->inlines);
            } else {
                text = EscapeText(PlainText(cell.blocks), /*atLineStart=*/false, flavour_);
            }

            // A table cell is a single line by construction.
            std::replace(text.begin(), text.end(), '\n', ' ');
            while (!text.empty() && text.back() == ' ') text.pop_back();

            // EscapeText already escaped the pipes it wrote, but a code span
            // does not go through it: `a|b` inside a cell would split the row.
            // Escaping what is not already escaped covers both paths.
            std::string guarded;
            guarded.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '\\' && i + 1 < text.size()) {
                    guarded.push_back(text[i]);
                    guarded.push_back(text[++i]);
                    continue;
                }
                if (text[i] == '|') guarded.push_back('\\');
                guarded.push_back(text[i]);
            }
            cells.push_back(std::move(guarded));
        }
        columns = std::max(columns, cells.size());
        grid.push_back(std::move(cells));
    }
    for (std::vector<std::string>& row : grid) row.resize(columns);

    if (lost_spans) {
        Note("table-spans-flattened",
             "Merged table cells were split apart; Markdown tables cannot express row or "
             "column spans.",
             Fidelity::Structural);
    }
    if (lost_blocks) {
        Note("table-cell-flattened",
             "Table cells containing lists, code with a pipe in it, or multiple paragraphs "
             "were flattened to a single line.",
             Fidelity::Structural);
    }

    std::vector<std::size_t> widths(columns, 3);
    if (flavour_ == Flavour::kPortable) {
        for (const std::vector<std::string>& row : grid) {
            for (std::size_t c = 0; c < columns; ++c) {
                widths[c] = std::max(widths[c], row[c].size());
            }
        }
    }

    const auto write_row = [&](const std::vector<std::string>& cells) {
        Emit("|");
        for (std::size_t c = 0; c < columns; ++c) {
            Emit(" " + cells[c]);
            if (flavour_ == Flavour::kPortable && cells[c].size() < widths[c]) {
                Emit(std::string(widths[c] - cells[c].size(), ' '));
            }
            Emit(" |");
        }
        NewLine();
    };

    const auto alignment_of = [&](std::size_t column) -> std::optional<Alignment> {
        if (column < table.columns.size() && table.columns[column].alignment) {
            return table.columns[column].alignment;
        }
        // Fall back to the header cell's own alignment, which is where a
        // Markdown source put it in the first place.
        const std::size_t header = synthetic_header ? 0 : 0;
        if (header < table.rows.size() && column < table.rows[header].cells.size()) {
            return table.rows[header].cells[column].alignment;
        }
        return std::nullopt;
    };

    std::size_t body_start = 0;
    if (synthetic_header) {
        write_row(std::vector<std::string>(columns));
    } else {
        write_row(grid.front());
        body_start = 1;
    }

    Emit("|");
    for (std::size_t c = 0; c < columns; ++c) {
        const std::size_t dashes = std::max<std::size_t>(3, widths[c]);
        const std::optional<Alignment> align = alignment_of(c);
        std::string rule;
        if (align == Alignment::kStart) {
            rule = ":" + std::string(dashes - 1, '-');
        } else if (align == Alignment::kEnd) {
            rule = std::string(dashes - 1, '-') + ":";
        } else if (align == Alignment::kCenter) {
            rule = ":" + std::string(dashes - 2, '-') + ":";
        } else {
            rule = std::string(dashes, '-');
        }
        Emit(" " + rule + " |");
    }
    NewLine();

    for (std::size_t r = body_start; r < grid.size(); ++r) write_row(grid[r]);
}

void Writer::WriteImageBlock(const ImageBlock& image) {
    Emit("![");
    Emit(EscapeText(image.alt, false, flavour_));
    Emit("](");
    Emit(image.resource);
    if (!image.title.empty()) Emit(" \"" + image.title + "\"");
    Emit(")");
    NewLine();

    if (image.widthPt > 0.0f || image.heightPt > 0.0f) {
        Note("image-size-dropped",
             "Markdown cannot express a display size, so the image will render at its natural "
             "size.",
             Fidelity::Structural);
    }
}

void Writer::WriteFootnote(const Footnote& footnote, std::size_t /*depth*/) {
    // A footnote block reached directly rather than through a reference. Record
    // it so it is emitted with the others at the end.
    if (std::find(footnoteIds_.begin(), footnoteIds_.end(), footnote.id) == footnoteIds_.end()) {
        footnoteIds_.push_back(footnote.id);
        footnotes_.push_back(&footnote);
    }
}

void Writer::WriteRaw(const RawBlock& raw) {
    if (raw.format == "html") {
        // HTML passes through Markdown untouched; that is a feature of the
        // format, not a compromise.
        Emit(std::string(reinterpret_cast<const char*>(raw.bytes.data()), raw.bytes.size()));
        NewLine();
        return;
    }
    Note("raw-block-dropped",
         "A " + (raw.description.empty() ? raw.format : raw.description) +
             " could not be represented in Markdown and was omitted.",
         Fidelity::Structural);
}

namespace {

// The first character this inline would contribute, or 0 when it contributes
// none. Only used to decide whether a delimiter would land inside a word.
char FirstCharacter(const Inline& node) {
    if (const auto* text = node.As<Text>()) return text->text.empty() ? '\0' : text->text.front();
    if (const auto* code = node.As<InlineCode>()) return '`';
    if (const auto* e = node.As<Emphasis>()) {
        return e->children.empty() ? '\0' : '*';
    }
    if (const auto* s = node.As<Strong>()) {
        return s->children.empty() ? '\0' : '*';
    }
    return '\0';
}

bool IsWordCharacter(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

}  // namespace

void Writer::WriteInlines(const Inlines& inlines, bool atLineStart) {
    bool line_start = atLineStart;
    for (std::size_t i = 0; i < inlines.size(); ++i) {
        const Inline& node = inlines[i];
        nextIsWord_ =
            i + 1 < inlines.size() && IsWordCharacter(FirstCharacter(inlines[i + 1]));

        // A hard break with nothing after it is not a break. CommonMark requires
        // something on the next line, so the backslash we would emit is read
        // back as a literal backslash -- which is how a paragraph ending in
        // Shift+Enter grew a stray character on every round trip.
        if (i + 1 == inlines.size()) {
            if (const auto* br = node.As<LineBreak>(); br != nullptr && br->hard) continue;
        }

        WriteInline(node, line_start);
        line_start = node.Is<LineBreak>();
    }
}

void Writer::WriteInline(const Inline& node, bool atLineStart) {
    std::visit(
        Overloaded{
            [&](const Text& n) {
                Emit(EscapeText(n.text, atLineStart, flavour_));
                if (!n.style.Empty()) {
                    // Rule 1 in action: the IR kept the colour and the font, and
                    // the loss is reported HERE, at the writer, rather than
                    // having been dropped silently by the reader.
                    Note("text-style-dropped",
                         "Character formatting such as colour, font and size has no Markdown "
                         "equivalent and was dropped.",
                         Fidelity::Structural);
                }
            },
            [&](const Emphasis& n) {
                Emit("*");
                WriteInlines(n.children, false);
                Emit("*");
            },
            [&](const Strong& n) {
                Emit("**");
                WriteInlines(n.children, false);
                Emit("**");
            },
            [&](const Strikethrough& n) {
                if (flavour_ == Flavour::kCommonMark) {
                    Note("strikethrough-dropped",
                         "CommonMark has no strikethrough, so the text was written unmarked.",
                         Fidelity::Structural);
                    WriteInlines(n.children, false);
                    return;
                }
                // "~~" IS NOT VALID INTRAWORD, and this is the one delimiter
                // where that is true: "aa*bb*cc" is emphasis but "aa~~bb~~cc"
                // is four literal tildes. Writing them anyway does not merely
                // lose the strikethrough -- it INSERTS TILDES INTO THE TEXT, so
                // the round trip corrupts the words themselves, which is the
                // worst outcome available.
                //
                // When either side sits against a word character the marks come
                // off and the loss is reported instead. A fidelity note is
                // something a user can see; a stray "~~" is something they file
                // a bug about.
                const bool followedByWord = nextIsWord_;
                const bool precededByWord = !out_.empty() && IsWordCharacter(out_.back());
                if (precededByWord || followedByWord) {
                    Note("strikethrough-intraword",
                         "Strikethrough that starts or ends in the middle of a word cannot be "
                         "written in Markdown, so the text was written unmarked.",
                         Fidelity::Structural);
                    WriteInlines(n.children, false);
                    return;
                }
                Emit("~~");
                WriteInlines(n.children, false);
                Emit("~~");
            },
            [&](const InlineCode& n) {
                // Same fence rule as a code block: the delimiter must be longer
                // than any backtick run inside.
                std::size_t longest = 0;
                std::size_t run = 0;
                for (const char c : n.text) {
                    run = (c == '`') ? run + 1 : 0;
                    longest = std::max(longest, run);
                }
                const std::string fence(longest + 1, '`');
                // A space is required when the content starts or ends with a
                // backtick, and is stripped by the reader.
                const bool pad = !n.text.empty() && (n.text.front() == '`' || n.text.back() == '`');
                Emit(fence);
                if (pad) Emit(" ");
                Emit(n.text);
                if (pad) Emit(" ");
                Emit(fence);
            },
            [&](const Link& n) {
                Emit("[");
                WriteInlines(n.children, false);
                Emit("](");
                Emit(n.target);
                if (!n.title.empty()) Emit(" \"" + n.title + "\"");
                Emit(")");
            },
            [&](const InlineImage& n) {
                Emit("![");
                Emit(EscapeText(n.alt, false, flavour_));
                Emit("](");
                Emit(n.resource);
                if (!n.title.empty()) Emit(" \"" + n.title + "\"");
                Emit(")");
            },
            [&](const LineBreak& n) {
                if (inHeading_) {
                    Note("heading-line-break-flattened",
                         "A line break inside a heading became a space; a Markdown heading is "
                         "a single line.",
                         Fidelity::Structural);
                    Emit(" ");
                    return;
                }
                // A backslash is the unambiguous hard break. Two trailing spaces
                // also work and are invisible, which is why they are a bug
                // magnet and why we do not emit them.
                if (n.hard) Emit("\\");
                NewLine();
            },
            [&](const FootnoteRef& n) {
                std::size_t index = 0;
                const auto it = std::find(footnoteIds_.begin(), footnoteIds_.end(), n.id);
                if (it != footnoteIds_.end()) {
                    index = static_cast<std::size_t>(std::distance(footnoteIds_.begin(), it));
                } else {
                    index = footnoteIds_.size();
                    footnoteIds_.push_back(n.id);
                    const Footnote* body = nullptr;
                    ForEachBlock(document_.blocks, [&](const Block& block) {
                        if (const auto* f = block.As<Footnote>(); f != nullptr && f->id == n.id) {
                            body = f;
                        }
                    });
                    footnotes_.push_back(body);
                }
                Emit("[^" + std::to_string(index + 1) + "]");
            },
            [&](const Math& n) {
                Emit(n.display ? "$$" : "$");
                Emit(n.tex);
                Emit(n.display ? "$$" : "$");
            },
            [&](const RawInline& n) {
                if (n.format == "html") {
                    Emit(n.content);
                    return;
                }
                Note("raw-inline-dropped",
                     "Inline " + n.format + " content was omitted.", Fidelity::Structural);
            },
        },
        node.node);
}

}  // namespace

Result WriteMarkdown(const doc::Document& document, const OptionBag& options, std::string& into) {
    const Flavour flavour = FlavourFromString(options.GetString(opt::kMarkdownFlavour, "gfm"));

    Result result = Result::Ok();
    Writer writer(document, flavour, result);
    writer.WriteDocument();
    into = writer.Take();
    return result;
}

std::vector<MediaProfile> MarkdownWriter::Writes() const {
    return {*MediaProfile::Parse(kMarkdownProfileId)};
}

EdgeCost MarkdownWriter::WriteCost(const MediaProfile& /*to*/) const {
    EdgeCost cost;
    // STRUCTURAL, not HIGH: content and hierarchy survive, visual layout does
    // not. Section 8.8's example for this rating is exactly DOCX -> MD.
    cost.fidelity = Fidelity::Structural;
    cost.generational = 0;
    cost.cpuPerMiB = 2.0f;
    cost.memPeakFactor = 3.0f;
    cost.requiresDisk = false;
    return cost;
}

Result MarkdownWriter::Write(ConvertContext& context, const doc::Document& document,
                             const MediaProfile& /*to*/, ISink& sink) {
    if (context.cancelled()) return Result::Cancelled();

    std::string text;
    Result result = WriteMarkdown(document, context.options, text);
    if (!result) return result;

    if (context.cancelled()) return Result::Cancelled();

    WriteAll(sink, text);
    sink.Commit();
    context.progress.Report(1.0, text.size());
    return result;
}

}  // namespace morph::docmd
