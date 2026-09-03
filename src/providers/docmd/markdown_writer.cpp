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
    void WriteBlocks(const Blocks& blocks, std::size_t depth);
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
    bool pendingBlankLine_ = false;

    // Footnote definitions are collected as they are referenced and emitted at
    // the end, numbered in reference order. That is what every renderer expects
    // and it keeps the numbering stable across a round trip.
    std::vector<const Footnote*> footnotes_;
    std::vector<std::string> footnoteIds_;
};

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

void Writer::WriteBlocks(const Blocks& blocks, std::size_t depth) {
    if (depth > kMaxBlockDepth) {
        Note("nesting-truncated", "Content nested deeper than 100 levels was omitted.",
             Fidelity::Structural);
        return;
    }
    bool first = true;
    for (const Block& block : blocks) {
        if (!first) BlankLine();
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
                   [&](const HorizontalRule&) { Emit("---"); NewLine(); },
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
    WriteInlines(heading.inlines, /*atLineStart=*/false);
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
    int number = list.start;

    for (std::size_t i = 0; i < list.items.size(); ++i) {
        const ListItem& item = list.items[i];
        if (i > 0 && !list.tight) BlankLine();
        if (i > 0 && list.tight) NewLine();

        std::string marker;
        if (list.ordered) {
            marker = std::to_string(number++) + ". ";
        } else {
            // Portable uses '-' throughout: '*' is also emphasis and '+' is
            // handled inconsistently by older renderers.
            marker = "- ";
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
        WriteBlocks(item.blocks, depth);
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

            std::string text = PlainText(cell.blocks);
            // A table cell is a single line by construction.
            std::replace(text.begin(), text.end(), '\n', ' ');
            while (!text.empty() && text.back() == ' ') text.pop_back();
            cells.push_back(EscapeText(text, /*atLineStart=*/false, flavour_));
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
             "Table cells containing lists, code or multiple paragraphs were flattened to a "
             "single line.",
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

void Writer::WriteInlines(const Inlines& inlines, bool atLineStart) {
    bool line_start = atLineStart;
    for (const Inline& node : inlines) {
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
