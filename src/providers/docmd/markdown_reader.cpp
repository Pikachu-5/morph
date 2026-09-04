// md4c -> IR. Section 8.3.
//
// md4c is a SAX-style parser: it emits enter/leave callbacks for blocks and
// spans and hands text through in runs. So this file is a stack machine. Each
// open container is a Frame; leaving one folds it into its parent. There is no
// intermediate AST, which is most of why the reader is fast.

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <md4c.h>

#include "front_matter.h"
#include "morph/providers/markdown.h"

namespace morph::docmd {
namespace {

using namespace morph::doc;

// --- Entity handling ---------------------------------------------------------
//
// md4c hands entities back as MD_TEXT_ENTITY runs rather than decoding them, so
// the decision of what "&amp;" means is ours. Numeric references and the common
// named ones are decoded; anything else is passed through verbatim, which is
// both honest and better for a Markdown round trip than guessing.

struct NamedEntity {
    std::string_view name;
    std::string_view utf8;
};

// Sorted by name so the lookup can binary-search. This is the practical subset:
// the full HTML5 table is 2231 entries and 100 KiB, and none of the rest appear
// in real documents often enough to justify carrying it.
constexpr NamedEntity kEntities[] = {
    {"AMP", "&"},     {"COPY", "©"}, {"GT", ">"},      {"LT", "<"},
    {"QUOT", "\""},   {"REG", "®"},  {"amp", "&"},     {"apos", "'"},
    {"bull", "•"}, {"copy", "©"}, {"dagger", "†"}, {"deg", "°"},
    {"divide", "÷"}, {"euro", "€"}, {"gt", ">"},  {"half", "½"},
    {"hellip", "…"}, {"iexcl", "¡"}, {"iquest", "¿"}, {"laquo", "«"},
    {"ldquo", "“"}, {"lsquo", "‘"}, {"lt", "<"},  {"mdash", "—"},
    {"micro", "µ"}, {"middot", "·"}, {"minus", "−"}, {"nbsp", " "},
    {"ndash", "–"}, {"para", "¶"}, {"plusmn", "±"}, {"pound", "£"},
    {"quot", "\""},   {"raquo", "»"}, {"rdquo", "”"}, {"reg", "®"},
    {"rsquo", "’"}, {"sect", "§"}, {"sup2", "²"}, {"sup3", "³"},
    {"times", "×"}, {"trade", "™"}, {"yen", "¥"},
};

void AppendUtf8(std::string& out, std::uint32_t codepoint) {
    // Lone surrogates and out-of-range values become U+FFFD rather than invalid
    // UTF-8. The IR is UTF-8 by contract and every writer downstream assumes it.
    if (codepoint > 0x10FFFFu || (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
        codepoint = 0xFFFDu;
    }
    if (codepoint < 0x80u) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else if (codepoint < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    }
}

void AppendEntity(std::string& out, std::string_view entity) {
    // Arrives complete, including the '&' and the ';'.
    if (entity.size() < 3 || entity.front() != '&' || entity.back() != ';') {
        out.append(entity);
        return;
    }
    const std::string_view body = entity.substr(1, entity.size() - 2);

    if (body.front() == '#') {
        const bool hex = body.size() > 1 && (body[1] == 'x' || body[1] == 'X');
        const std::string_view digits = body.substr(hex ? 2 : 1);
        if (digits.empty()) {
            out.append(entity);
            return;
        }
        std::uint32_t value = 0;
        for (const char c : digits) {
            unsigned digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<unsigned>(c - '0');
            } else if (hex && c >= 'a' && c <= 'f') {
                digit = static_cast<unsigned>(c - 'a' + 10);
            } else if (hex && c >= 'A' && c <= 'F') {
                digit = static_cast<unsigned>(c - 'A' + 10);
            } else {
                out.append(entity);
                return;
            }
            value = value * (hex ? 16u : 10u) + digit;
            if (value > 0x10FFFFu) {  // stop before overflowing on a long run
                value = 0xFFFDu;
                break;
            }
        }
        AppendUtf8(out, value == 0 ? 0xFFFDu : value);
        return;
    }

    const auto* found = std::lower_bound(
        std::begin(kEntities), std::end(kEntities), body,
        [](const NamedEntity& e, std::string_view name) { return e.name < name; });
    if (found != std::end(kEntities) && found->name == body) {
        out.append(found->utf8);
        return;
    }
    // Unknown: keep it exactly as written. Re-emitting "&foo;" round-trips.
    out.append(entity);
}

std::string Flatten(const MD_ATTRIBUTE& attribute) {
    std::string out;
    if (attribute.text == nullptr) return out;

    for (int i = 0; attribute.substr_offsets[i] < attribute.size; ++i) {
        const MD_SIZE begin = attribute.substr_offsets[i];
        const MD_SIZE end = attribute.substr_offsets[i + 1];
        const std::string_view piece(attribute.text + begin, end - begin);

        switch (attribute.substr_types[i]) {
            case MD_TEXT_ENTITY: AppendEntity(out, piece); break;
            case MD_TEXT_NULLCHAR: AppendUtf8(out, 0xFFFDu); break;
            default: out.append(piece); break;
        }
    }
    return out;
}

// --- The stack machine -------------------------------------------------------

enum class FrameKind {
    kDocument,
    kQuote,
    kList,
    kItem,
    kParagraph,
    kHeading,
    kCode,
    kHtml,
    kTable,
    kTableHead,
    kTableBody,
    kRow,
    kCell,
};

struct Frame {
    FrameKind kind = FrameKind::kDocument;

    Blocks blocks;    // finished child blocks
    Inlines inlines;  // inline run being accumulated

    // Per-kind payload. A variant would be tidier and would cost a visit at
    // every callback; a document with a million text runs makes that measurable.
    ListBlock list;
    ListItem item;
    int headingLevel = 1;
    std::string codeLanguage;
    std::string codeText;
    std::string htmlText;
    Table table;
    TableRow row;
    TableCell cell;
    bool inHeadSection = false;
};

// An open inline span, waiting for its children.
struct SpanFrame {
    MD_SPANTYPE type = MD_SPAN_EM;
    Inlines children;
    Link link;
    InlineImage image;
};

class Builder {
public:
    Builder(Document& document, const OptionBag& options)
        : document_(document), options_(options) {
        frames_.push_back(Frame{});  // the document frame
    }

    [[nodiscard]] Result Finish();

    int EnterBlock(MD_BLOCKTYPE type, void* detail);
    int LeaveBlock(MD_BLOCKTYPE type, void* detail);
    int EnterSpan(MD_SPANTYPE type, void* detail);
    int LeaveSpan(MD_SPANTYPE type, void* detail);
    int AddText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size);

private:
    Frame& Top() { return frames_.back(); }

    // Where inline nodes currently go: into the innermost open span if there is
    // one, otherwise into the innermost block frame.
    Inlines& InlineTarget() {
        return spans_.empty() ? frames_.back().inlines : spans_.back().children;
    }

    // Where finished blocks go.
    Blocks& BlockTarget() { return frames_.back().blocks; }

    void Push(FrameKind kind) {
        if (frames_.size() > kMaxBlockDepth) {
            overflowed_ = true;
            return;
        }
        frames_.push_back(Frame{});
        frames_.back().kind = kind;
    }

    Frame Pop() {
        Frame frame = std::move(frames_.back());
        frames_.pop_back();
        return frame;
    }

    void Emit(Block block) {
        if (++blockCount_ > kMaxBlockCount) {
            overflowed_ = true;
            return;
        }
        BlockTarget().push_back(std::move(block));
    }

    // A heading's anchor, matching the GitHub slug rules so that intra-document
    // links written against a rendered page keep working.
    [[nodiscard]] std::string MakeAnchor(const Inlines& inlines);

    Document& document_;
    const OptionBag& options_;
    std::vector<Frame> frames_;
    std::vector<SpanFrame> spans_;
    std::vector<std::string> anchors_;
    std::size_t blockCount_ = 0;
    bool overflowed_ = false;
};

std::string Builder::MakeAnchor(const Inlines& inlines) {
    std::string text = PlainText({Block::Of(Paragraph{inlines, {}, {}, std::nullopt})});

    std::string slug;
    slug.reserve(text.size());
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 'A' && u <= 'Z') {
            slug.push_back(static_cast<char>(u - 'A' + 'a'));
        } else if ((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-' || u == '_') {
            slug.push_back(static_cast<char>(u));
        } else if (u == ' ') {
            slug.push_back('-');
        } else if (u >= 0x80) {
            slug.push_back(static_cast<char>(u));  // keep non-ASCII, as GitHub does
        }
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    if (slug.empty()) slug = "section";

    // Duplicate headings get -1, -2, ... exactly as a renderer would.
    std::string candidate = slug;
    for (int suffix = 1;
         std::find(anchors_.begin(), anchors_.end(), candidate) != anchors_.end(); ++suffix) {
        candidate = slug + "-" + std::to_string(suffix);
    }
    anchors_.push_back(candidate);
    return candidate;
}

int Builder::EnterBlock(MD_BLOCKTYPE type, void* detail) {
    switch (type) {
        case MD_BLOCK_DOC:
            break;

        case MD_BLOCK_QUOTE:
            Push(FrameKind::kQuote);
            break;

        case MD_BLOCK_UL: {
            const auto* d = static_cast<const MD_BLOCK_UL_DETAIL*>(detail);
            Push(FrameKind::kList);
            Top().list.ordered = false;
            Top().list.tight = d->is_tight != 0;
            break;
        }
        case MD_BLOCK_OL: {
            const auto* d = static_cast<const MD_BLOCK_OL_DETAIL*>(detail);
            Push(FrameKind::kList);
            Top().list.ordered = true;
            Top().list.start = static_cast<int>(d->start);
            Top().list.tight = d->is_tight != 0;
            break;
        }
        case MD_BLOCK_LI: {
            const auto* d = static_cast<const MD_BLOCK_LI_DETAIL*>(detail);
            Push(FrameKind::kItem);
            if (d->is_task) {
                Top().item.checked = (d->task_mark == 'x' || d->task_mark == 'X');
            }
            break;
        }

        case MD_BLOCK_HR:
            Emit(Block::Of(HorizontalRule{}));
            break;

        case MD_BLOCK_H: {
            const auto* d = static_cast<const MD_BLOCK_H_DETAIL*>(detail);
            Push(FrameKind::kHeading);
            Top().headingLevel = std::clamp(static_cast<int>(d->level), 1, 6);
            break;
        }

        case MD_BLOCK_CODE: {
            const auto* d = static_cast<const MD_BLOCK_CODE_DETAIL*>(detail);
            Push(FrameKind::kCode);
            Top().codeLanguage = Flatten(d->lang);
            break;
        }

        case MD_BLOCK_HTML:
            Push(FrameKind::kHtml);
            break;

        case MD_BLOCK_P:
            Push(FrameKind::kParagraph);
            break;

        case MD_BLOCK_TABLE: {
            const auto* d = static_cast<const MD_BLOCK_TABLE_DETAIL*>(detail);
            Push(FrameKind::kTable);
            Top().table.columns.resize(d->col_count);
            break;
        }
        case MD_BLOCK_THEAD:
            Push(FrameKind::kTableHead);
            break;
        case MD_BLOCK_TBODY:
            Push(FrameKind::kTableBody);
            break;
        case MD_BLOCK_TR:
            Push(FrameKind::kRow);
            break;

        case MD_BLOCK_TH:
        case MD_BLOCK_TD: {
            const auto* d = static_cast<const MD_BLOCK_TD_DETAIL*>(detail);
            Push(FrameKind::kCell);
            switch (d->align) {
                case MD_ALIGN_LEFT:    Top().cell.alignment = Alignment::kStart; break;
                case MD_ALIGN_CENTER:  Top().cell.alignment = Alignment::kCenter; break;
                case MD_ALIGN_RIGHT:   Top().cell.alignment = Alignment::kEnd; break;
                case MD_ALIGN_DEFAULT: break;
            }
            break;
        }
    }
    return overflowed_ ? -1 : 0;
}

int Builder::LeaveBlock(MD_BLOCKTYPE type, void* /*detail*/) {
    switch (type) {
        case MD_BLOCK_DOC:
            break;

        case MD_BLOCK_QUOTE: {
            Frame frame = Pop();
            Emit(Block::Of(BlockQuote{std::move(frame.blocks)}));
            break;
        }

        case MD_BLOCK_UL:
        case MD_BLOCK_OL: {
            Frame frame = Pop();
            Emit(Block::Of(std::move(frame.list)));
            break;
        }

        case MD_BLOCK_LI: {
            Frame frame = Pop();

            // md4c does not emit MD_BLOCK_P inside a TIGHT list item: the text
            // arrives as inline content directly on the item, and only a loose
            // item gets a paragraph. Wrapping it here gives every item the same
            // block shape, so writers decide tightness from ListBlock::tight
            // rather than from whether a paragraph happens to be present.
            if (!frame.inlines.empty()) {
                Paragraph paragraph;
                paragraph.inlines = std::move(frame.inlines);
                frame.item.blocks.push_back(Block::Of(std::move(paragraph)));
            }
            frame.item.blocks.insert(frame.item.blocks.end(),
                                     std::make_move_iterator(frame.blocks.begin()),
                                     std::make_move_iterator(frame.blocks.end()));

            Top().list.items.push_back(std::move(frame.item));
            break;
        }

        case MD_BLOCK_HR:
            break;

        case MD_BLOCK_H: {
            Frame frame = Pop();
            Heading heading;
            heading.level = frame.headingLevel;
            heading.inlines = std::move(frame.inlines);
            heading.anchorId = MakeAnchor(heading.inlines);
            Emit(Block::Of(std::move(heading)));
            break;
        }

        case MD_BLOCK_CODE: {
            Frame frame = Pop();
            Emit(Block::Of(CodeBlock{std::move(frame.codeLanguage), std::move(frame.codeText)}));
            break;
        }

        case MD_BLOCK_HTML: {
            // Rule 3: recognised structurally, not mappable. Raw HTML inside
            // Markdown round-trips byte-exact through the IR and is dropped with
            // a note by writers that cannot host it.
            Frame frame = Pop();
            RawBlock raw;
            raw.format = "html";
            raw.description = "inline HTML block";
            raw.bytes.assign(reinterpret_cast<const std::byte*>(frame.htmlText.data()),
                             reinterpret_cast<const std::byte*>(frame.htmlText.data()) +
                                 frame.htmlText.size());
            Emit(Block::Of(std::move(raw)));
            break;
        }

        case MD_BLOCK_P: {
            Frame frame = Pop();
            Paragraph paragraph;
            paragraph.inlines = std::move(frame.inlines);
            Emit(Block::Of(std::move(paragraph)));
            break;
        }

        case MD_BLOCK_TABLE: {
            Frame frame = Pop();
            Emit(Block::Of(std::move(frame.table)));
            break;
        }

        case MD_BLOCK_THEAD:
            // The head section's rows were already folded into the table by the
            // MD_BLOCK_TR handler; all that is left is to record how many of
            // them were header rows.
            Pop();
            Top().table.headerRows = static_cast<std::uint32_t>(Top().table.rows.size());
            break;
        case MD_BLOCK_TBODY:
            Pop();
            break;

        case MD_BLOCK_TR: {
            Frame frame = Pop();
            // A row belongs to the table, which may be one or two frames up
            // depending on whether md4c wrapped it in a head or body section.
            for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
                if (it->kind == FrameKind::kTable) {
                    it->table.rows.push_back(std::move(frame.row));
                    break;
                }
            }
            break;
        }

        case MD_BLOCK_TH:
        case MD_BLOCK_TD: {
            Frame frame = Pop();
            TableCell cell = std::move(frame.cell);
            // Cell content is inline in Markdown; wrap it so every writer sees
            // the same shape regardless of which format produced the table.
            if (!frame.inlines.empty()) {
                Paragraph paragraph;
                paragraph.inlines = std::move(frame.inlines);
                cell.blocks.push_back(Block::Of(std::move(paragraph)));
            }
            Top().row.cells.push_back(std::move(cell));
            break;
        }
    }
    return overflowed_ ? -1 : 0;
}

int Builder::EnterSpan(MD_SPANTYPE type, void* detail) {
    if (spans_.size() > kMaxBlockDepth) {
        overflowed_ = true;
        return -1;
    }

    SpanFrame span;
    span.type = type;

    switch (type) {
        case MD_SPAN_A: {
            const auto* d = static_cast<const MD_SPAN_A_DETAIL*>(detail);
            span.link.target = Flatten(d->href);
            span.link.title = Flatten(d->title);
            break;
        }
        case MD_SPAN_IMG: {
            const auto* d = static_cast<const MD_SPAN_IMG_DETAIL*>(detail);
            // The IR addresses images by content hash, but Markdown references
            // them by URL and we do not fetch anything (section 1: local only,
            // no network egress at all). So the URL is preserved as the alt
            // carrier's target and the resource id stays empty until a reader
            // that has the actual bytes -- DOCX, HTML with a local file -- fills
            // it in. Writers emit the URL unchanged.
            span.image.title = Flatten(d->title);
            span.link.target = Flatten(d->src);
            break;
        }
        default:
            break;
    }

    spans_.push_back(std::move(span));
    return 0;
}

int Builder::LeaveSpan(MD_SPANTYPE type, void* /*detail*/) {
    if (spans_.empty()) return -1;
    SpanFrame span = std::move(spans_.back());
    spans_.pop_back();

    Inlines& target = InlineTarget();

    switch (type) {
        case MD_SPAN_EM:
            target.push_back(Inline::Of(Emphasis{std::move(span.children)}));
            break;
        case MD_SPAN_STRONG:
            target.push_back(Inline::Of(Strong{std::move(span.children)}));
            break;
        case MD_SPAN_DEL:
            target.push_back(Inline::Of(Strikethrough{std::move(span.children)}));
            break;
        case MD_SPAN_U:
            // Underline has no semantic inline node; it is presentational, so it
            // lands on a Text style exactly as rule 2 intends.
            for (Inline& child : span.children) {
                if (auto* text = std::get_if<Text>(&child.node)) {
                    text->style.underline = UnderlineStyle::kSingle;
                }
            }
            target.insert(target.end(), std::make_move_iterator(span.children.begin()),
                          std::make_move_iterator(span.children.end()));
            break;
        case MD_SPAN_A:
            span.link.children = std::move(span.children);
            target.push_back(Inline::Of(std::move(span.link)));
            break;
        case MD_SPAN_IMG: {
            // Alt text arrives as the span's children.
            std::string alt =
                PlainText({Block::Of(Paragraph{std::move(span.children), {}, {}, std::nullopt})});
            while (!alt.empty() && alt.back() == '\n') alt.pop_back();

            InlineImage image;
            image.alt = std::move(alt);
            image.title = std::move(span.image.title);
            // No bytes to hash, so the URL rides in the resource field. A later
            // reader that resolves it replaces this with a real content hash.
            image.resource = std::move(span.link.target);
            target.push_back(Inline::Of(std::move(image)));
            break;
        }
        case MD_SPAN_CODE: {
            std::string text;
            for (const Inline& child : span.children) {
                if (const auto* t = child.As<Text>()) text += t->text;
            }
            target.push_back(Inline::Of(InlineCode{std::move(text)}));
            break;
        }
        case MD_SPAN_LATEXMATH:
        case MD_SPAN_LATEXMATH_DISPLAY: {
            std::string tex;
            for (const Inline& child : span.children) {
                if (const auto* t = child.As<Text>()) tex += t->text;
            }
            target.push_back(
                Inline::Of(Math{std::move(tex), type == MD_SPAN_LATEXMATH_DISPLAY}));
            break;
        }
        case MD_SPAN_WIKILINK: {
            Link link;
            link.target = PlainText(
                {Block::Of(Paragraph{span.children, {}, {}, std::nullopt})});
            while (!link.target.empty() && link.target.back() == '\n') link.target.pop_back();
            link.children = std::move(span.children);
            target.push_back(Inline::Of(std::move(link)));
            break;
        }
    }
    return 0;
}

int Builder::AddText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size) {
    const std::string_view piece(text, size);

    // Code and HTML blocks accumulate raw text on their frame rather than as
    // inline nodes.
    if (!frames_.empty() && spans_.empty()) {
        if (Top().kind == FrameKind::kCode) {
            if (type == MD_TEXT_NULLCHAR) {
                AppendUtf8(Top().codeText, 0xFFFDu);
            } else {
                Top().codeText.append(piece);
            }
            return 0;
        }
        if (Top().kind == FrameKind::kHtml) {
            Top().htmlText.append(piece);
            return 0;
        }
    }

    Inlines& target = InlineTarget();

    switch (type) {
        case MD_TEXT_NORMAL:
        case MD_TEXT_CODE:
        case MD_TEXT_LATEXMATH:
            // Merge into the previous run when it is an unstyled Text node.
            // Without this a five-word paragraph becomes eleven IR nodes, which
            // is the same trap section 8.4 warns about for DOCX runs.
            if (!target.empty()) {
                if (auto* last = std::get_if<Text>(&target.back().node);
                    last != nullptr && last->style.Empty()) {
                    last->text.append(piece);
                    return 0;
                }
            }
            target.push_back(Inline::Of(Text{std::string(piece), {}}));
            break;

        case MD_TEXT_NULLCHAR: {
            std::string replacement;
            AppendUtf8(replacement, 0xFFFDu);
            target.push_back(Inline::Of(Text{std::move(replacement), {}}));
            break;
        }

        case MD_TEXT_BR:
            target.push_back(Inline::Of(LineBreak{/*hard=*/true}));
            break;

        case MD_TEXT_SOFTBR:
            target.push_back(Inline::Of(LineBreak{/*hard=*/false}));
            break;

        case MD_TEXT_ENTITY: {
            std::string decoded;
            AppendEntity(decoded, piece);
            if (!target.empty()) {
                if (auto* last = std::get_if<Text>(&target.back().node);
                    last != nullptr && last->style.Empty()) {
                    last->text.append(decoded);
                    return 0;
                }
            }
            target.push_back(Inline::Of(Text{std::move(decoded), {}}));
            break;
        }

        case MD_TEXT_HTML:
            // Inline HTML inside a paragraph. Rule 3 again, at inline scale.
            target.push_back(Inline::Of(RawInline{"html", std::string(piece)}));
            break;
    }
    return 0;
}

Result Builder::Finish() {
    if (overflowed_) {
        return Result::Error(Status::kResourceLimit,
                             "The document nests deeper or contains more blocks than Morph will "
                             "process. This usually means the file is malformed.");
    }
    if (frames_.size() != 1) {
        return Result::Error(Status::kInternal, "Unbalanced Markdown block structure.");
    }
    document_.blocks = std::move(frames_.front().blocks);
    if (document_.meta.lang.empty()) document_.meta.lang = options_.GetString("lang", "");
    return Result::Ok();
}

// --- md4c trampolines --------------------------------------------------------

int OnEnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    return static_cast<Builder*>(userdata)->EnterBlock(type, detail);
}
int OnLeaveBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    return static_cast<Builder*>(userdata)->LeaveBlock(type, detail);
}
int OnEnterSpan(MD_SPANTYPE type, void* detail, void* userdata) {
    return static_cast<Builder*>(userdata)->EnterSpan(type, detail);
}
int OnLeaveSpan(MD_SPANTYPE type, void* detail, void* userdata) {
    return static_cast<Builder*>(userdata)->LeaveSpan(type, detail);
}
int OnText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    return static_cast<Builder*>(userdata)->AddText(type, text, size);
}

unsigned ParserFlags(Flavour flavour) {
    unsigned flags = MD_FLAG_NOHTMLBLOCKS * 0;  // HTML is kept; see rule 3

    switch (flavour) {
        case Flavour::kCommonMark:
            flags |= MD_DIALECT_COMMONMARK;
            break;
        case Flavour::kGitHub:
        case Flavour::kPortable:
            // Portable differs only in how we WRITE. Reading should always
            // accept as much as possible -- refusing to parse a GFM table
            // because the user picked a conservative output flavour would be
            // surprising and would lose data.
            flags |= MD_DIALECT_GITHUB;
            break;
    }
    // Useful everywhere and cheap: LaTeX math and underline have IR nodes, so
    // parsing them is strictly better than treating them as literal text.
    flags |= MD_FLAG_LATEXMATHSPANS | MD_FLAG_UNDERLINE;
    return flags;
}

}  // namespace

Flavour FlavourFromString(std::string_view name) {
    if (name == "commonmark") return Flavour::kCommonMark;
    if (name == "portable") return Flavour::kPortable;
    return Flavour::kGitHub;
}

std::string_view ToString(Flavour flavour) {
    switch (flavour) {
        case Flavour::kCommonMark: return "commonmark";
        case Flavour::kPortable:   return "portable";
        case Flavour::kGitHub:     return "gfm";
    }
    return "gfm";
}


Result ReadMarkdown(std::string_view text, const OptionBag& options, doc::Document& into) {
    // A byte order mark is not content. Leaving it in produces a document whose
    // first paragraph begins with an invisible character that survives every
    // conversion and confuses every diff.
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);

    // Before md4c sees it: md4c knows nothing about front matter and would
    // read the block as a setext heading.
    text = StripFrontMatter(text, into.meta);

    const Flavour flavour =
        FlavourFromString(options.GetString(opt::kMarkdownFlavour, "gfm"));

    Builder builder(into, options);

    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = ParserFlags(flavour);
    parser.enter_block = OnEnterBlock;
    parser.leave_block = OnLeaveBlock;
    parser.enter_span = OnEnterSpan;
    parser.leave_span = OnLeaveSpan;
    parser.text = OnText;
    parser.debug_log = nullptr;
    parser.syntax = nullptr;

    const int rc = md_parse(text.data(), static_cast<MD_SIZE>(text.size()), &parser, &builder);

    // md4c returns non-zero only when a callback asked it to stop, which for us
    // means a resource limit. Markdown itself has no parse errors: every byte
    // sequence is a valid document.
    Result result = builder.Finish();
    doc::Normalise(into.blocks);
    if (rc != 0 && result.ok()) {
        return Result::Error(Status::kResourceLimit,
                             "Stopped reading the Markdown document at a structural limit.");
    }
    return result;
}

std::vector<MediaProfile> MarkdownReader::Reads() const {
    return {*MediaProfile::Parse(kMarkdownProfileId)};
}

EdgeCost MarkdownReader::ReadCost(const MediaProfile& /*from*/) const {
    EdgeCost cost;
    // Rule 1 makes this lossless by construction: the IR is a superset of what
    // Markdown expresses, so the reader has nothing to drop.
    cost.fidelity = Fidelity::Lossless;
    cost.generational = 0;
    cost.cpuPerMiB = 3.0f;
    cost.memPeakFactor = 6.0f;  // IR nodes are much larger than the source text
    cost.requiresDisk = false;
    return cost;
}

std::optional<MediaProfile> MarkdownReader::Probe(ISource& source, const PathHint& hint) const {
    // Markdown has no magic bytes; anything is valid Markdown. So this must be
    // the LAST provider consulted, and it recognises a file only when the
    // extension suggests it AND the content is plausible text. Section 5 forbids
    // trusting the extension, and this does not: a .md file full of NUL bytes is
    // rejected here.
    static constexpr std::string_view kExtensions[] = {".md", ".markdown", ".mdown", ".mkd",
                                                       ".mdtext", ".text"};
    if (std::find(std::begin(kExtensions), std::end(kExtensions), hint.extension) ==
        std::end(kExtensions)) {
        return std::nullopt;
    }

    const std::vector<std::byte> prefix = ReadAtMost(source, 8 * 1024);
    for (const std::byte b : prefix) {
        const auto c = static_cast<unsigned char>(b);
        // Control characters other than tab, newline and carriage return do not
        // appear in text. One is enough to say this is not a Markdown document.
        if (c < 0x09 || (c > 0x0D && c < 0x20)) return std::nullopt;
    }
    return MediaProfile::Parse(kMarkdownProfileId);
}

Result MarkdownReader::Read(ConvertContext& context, ISource& source,
                            const MediaProfile& /*from*/, doc::Document& into) {
    if (context.cancelled()) return Result::Cancelled();

    // Markdown is a whole-document format: md4c needs the entire text in memory
    // and there is no useful streaming decomposition. The size cap comes from
    // the job budget, which section 11.2 derived from memPeakFactor above.
    constexpr std::uint64_t kMaxBytes = 256ull * 1024 * 1024;
    if (const auto size = source.Size(); size && *size > kMaxBytes) {
        return Result::Error(Status::kResourceLimit,
                             "This Markdown file is larger than 256 MB.");
    }

    std::string text;
    if (const auto size = source.Size()) text.reserve(static_cast<std::size_t>(*size));

    std::vector<std::byte> chunk(kStreamChunkBytes);
    while (true) {
        const std::size_t got = source.Read(chunk);
        if (got == 0) break;
        text.append(reinterpret_cast<const char*>(chunk.data()), got);
        if (text.size() > kMaxBytes) {
            return Result::Error(Status::kResourceLimit,
                                 "This Markdown file is larger than 256 MB.");
        }
        if (context.cancelled()) return Result::Cancelled();
    }

    context.progress.Report(0.5, 0);
    Result result = ReadMarkdown(text, context.options, into);
    context.progress.Report(1.0, 0);
    return result;
}

}  // namespace morph::docmd
