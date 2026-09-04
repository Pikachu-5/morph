// HTML -> IR. Section 8.5.
//
// "lexbor (Apache-2.0) -- a genuinely spec-compliant HTML5 tokenizer and tree
//  builder in C, which matters because real-world HTML is malformed and a naive
//  parser will disagree with browsers about where elements nest. Map its DOM to
//  IR through a SEMANTIC WHITELIST; ignore presentational markup rather than
//  trying to interpret it."
//
// The whitelist is the design. An element we recognise becomes the IR node it
// means; an element we do not recognise is TRANSPARENT -- its children are
// processed as if it were not there. That is deliberately not the same as
// dropping it: <div><span><p>text</p></span></div> must still yield a paragraph.
//
// What we never do is interpret presentation. <font color>, <center>, a
// style="font-weight:bold" attribute: these are how 2003 said "bold", and
// guessing at them produces a document that is subtly wrong in ways nobody can
// explain. <b> and <i> ARE mapped, because HTML5 gives them meaning and every
// real document uses them.

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include "morph/providers/html.h"

namespace morph::dochtml {
namespace {

using namespace morph::doc;

std::string_view ToView(const lxb_char_t* text, std::size_t size) {
    if (text == nullptr || size == 0) return {};
    return std::string_view(reinterpret_cast<const char*>(text), size);
}

std::string TagName(lxb_dom_element_t* element) {
    std::size_t size = 0;
    const lxb_char_t* name = lxb_dom_element_local_name(element, &size);
    std::string lowered(ToView(name, size));
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered;
}

std::string Attribute(lxb_dom_element_t* element, std::string_view name) {
    std::size_t size = 0;
    const lxb_char_t* value = lxb_dom_element_get_attribute(
        element, reinterpret_cast<const lxb_char_t*>(name.data()), name.size(), &size);
    return std::string(ToView(value, size));
}

bool HasAttribute(lxb_dom_element_t* element, std::string_view name) {
    return lxb_dom_element_has_attribute(
               element, reinterpret_cast<const lxb_char_t*>(name.data()), name.size()) != false;
}

// Collapses runs of whitespace to a single space, as HTML rendering does.
// Without this, source indentation becomes content and every paragraph read from
// a pretty-printed file is full of newlines and tabs.
std::string CollapseWhitespace(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool in_space = false;
    for (const char c : text) {
        const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
        if (space) {
            in_space = true;
            continue;
        }
        if (in_space && !out.empty()) out.push_back(' ');
        if (in_space && out.empty()) {
            // Leading whitespace in a run is dropped; the caller decides whether
            // a boundary space is needed, because that depends on what came
            // before in the inline sequence.
        }
        in_space = false;
        out.push_back(c);
    }
    if (in_space && !out.empty()) out.push_back(' ');
    return out;
}

std::optional<Alignment> AlignmentFromAttribute(const std::string& value) {
    if (value == "left" || value == "start") return Alignment::kStart;
    if (value == "center" || value == "centre") return Alignment::kCenter;
    if (value == "right" || value == "end") return Alignment::kEnd;
    if (value == "justify") return Alignment::kJustify;
    return std::nullopt;
}

// The GFM-compatible slug, same rules as the Markdown reader so that anchors
// survive a format change in either direction.
std::string Slug(std::string_view text, std::vector<std::string>& taken) {
    std::string slug;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 'A' && u <= 'Z') {
            slug.push_back(static_cast<char>(u - 'A' + 'a'));
        } else if ((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-' || u == '_' ||
                   u >= 0x80) {
            slug.push_back(static_cast<char>(u));
        } else if (u == ' ') {
            slug.push_back('-');
        }
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    if (slug.empty()) slug = "section";

    std::string candidate = slug;
    for (int i = 1; std::find(taken.begin(), taken.end(), candidate) != taken.end(); ++i) {
        candidate = slug + "-" + std::to_string(i);
    }
    taken.push_back(candidate);
    return candidate;
}

// --- The whitelist -----------------------------------------------------------

enum class Handling {
    kTransparent,  // not recognised: process children in place
    kSkip,         // recognised as non-content: drop entirely (script, style)
    kBlock,        // becomes a block node
    kInline,       // becomes an inline node
};

struct Recognised {
    Handling handling = Handling::kTransparent;
};

Handling Classify(const std::string& tag) {
    // Non-content. Dropping these is not a fidelity loss; keeping them would put
    // JavaScript source into a Word document.
    static constexpr std::string_view kSkipped[] = {"script", "style",   "noscript", "template",
                                                    "head",   "meta",    "link",     "base",
                                                    "title",  "iframe",  "object",   "embed",
                                                    "svg",    "canvas",  "audio",    "video",
                                                    "form",   "button",  "select",   "textarea"};
    if (std::find(std::begin(kSkipped), std::end(kSkipped), tag) != std::end(kSkipped)) {
        return Handling::kSkip;
    }

    static constexpr std::string_view kBlocks[] = {
        "p",  "h1", "h2", "h3", "h4",     "h5",         "h6",  "blockquote", "pre",
        "ul", "ol", "li", "hr", "table",  "thead",      "tbody", "tfoot",    "tr",
        "th", "td", "figure", "figcaption", "caption", "dl",  "dt",         "dd"};
    if (std::find(std::begin(kBlocks), std::end(kBlocks), tag) != std::end(kBlocks)) {
        return Handling::kBlock;
    }

    static constexpr std::string_view kInlines[] = {"em", "i",   "strong", "b",  "del", "s",
                                                    "strike", "code", "a",  "img", "br",
                                                    "sup", "sub", "u",      "mark", "kbd",
                                                    "samp", "var", "q",     "cite", "abbr"};
    if (std::find(std::begin(kInlines), std::end(kInlines), tag) != std::end(kInlines)) {
        return Handling::kInline;
    }

    // div, span, section, article, main, header, footer, nav, aside, font,
    // center, and everything else: transparent. Their children are the content.
    return Handling::kTransparent;
}

// --- The reader --------------------------------------------------------------

class Reader {
public:
    Reader(Document& document, Result& result) : document_(document), result_(result) {}

    void ReadNode(lxb_dom_node_t* node, Blocks& blocks, Inlines& inlines, std::size_t depth);

    void Flush(Blocks& blocks, Inlines& inlines);

private:
    void ReadElement(lxb_dom_element_t* element, const std::string& tag, Blocks& blocks,
                     Inlines& inlines, std::size_t depth);
    void ReadChildren(lxb_dom_node_t* node, Blocks& blocks, Inlines& inlines, std::size_t depth);
    [[nodiscard]] Inlines ReadInlineChildren(lxb_dom_node_t* node, std::size_t depth);
    [[nodiscard]] Blocks ReadBlockChildren(lxb_dom_node_t* node, std::size_t depth);
    void ReadTable(lxb_dom_element_t* element, Blocks& blocks, std::size_t depth);
    [[nodiscard]] std::string TextOf(lxb_dom_node_t* node);

    void Note(std::string code, std::string detail) {
        result_.Note(std::move(code), std::move(detail));
    }

    Document& document_;
    Result& result_;
    std::vector<std::string> anchors_;
    bool truncated_ = false;
};

std::string Reader::TextOf(lxb_dom_node_t* node) {
    std::size_t size = 0;
    lxb_char_t* text = lxb_dom_node_text_content(node, &size);
    std::string out(ToView(text, size));
    if (text != nullptr) lxb_dom_document_destroy_text(node->owner_document, text);
    return out;
}

// Trims the whitespace at the edges of an inline run. HTML collapses interior
// whitespace and drops it at a block boundary; without this, every paragraph
// read from a pretty-printed file carries the source's indentation as content.
void TrimEdges(Inlines& inlines) {
    if (!inlines.empty()) {
        if (auto* first = std::get_if<Text>(&inlines.front().node)) {
            const std::size_t at = first->text.find_first_not_of(' ');
            first->text.erase(0, at == std::string::npos ? first->text.size() : at);
            if (first->text.empty() && first->style.Empty()) inlines.erase(inlines.begin());
        }
    }
    if (!inlines.empty()) {
        if (auto* last = std::get_if<Text>(&inlines.back().node)) {
            const std::size_t at = last->text.find_last_not_of(' ');
            last->text.erase(at == std::string::npos ? 0 : at + 1);
            if (last->text.empty() && last->style.Empty()) inlines.pop_back();
        }
    }
}

// Appends a text run, merging with the previous unstyled run so a sentence split
// across three source lines does not become three IR nodes.
void AppendText(Inlines& inlines, std::string text) {
    if (text.empty()) return;
    if (!inlines.empty()) {
        if (auto* last = std::get_if<Text>(&inlines.back().node);
            last != nullptr && last->style.Empty()) {
            last->text += text;
            return;
        }
    }
    inlines.push_back(Inline::Of(Text{std::move(text), {}}));
}

void Reader::Flush(Blocks& blocks, Inlines& inlines) {
    // Loose inline content that never got wrapped in a block element. HTML
    // permits it and real documents are full of it.
    if (inlines.empty()) return;

    // Whitespace-only run: not content, just source formatting.
    bool has_content = false;
    ForEachInline(inlines, [&](const Inline& node) {
        if (const auto* text = node.As<Text>()) {
            has_content |= text->text.find_first_not_of(" \t\n\r") != std::string::npos;
        } else if (!node.Is<LineBreak>()) {
            has_content = true;
        }
    });

    if (has_content) {
        Paragraph paragraph;
        paragraph.inlines = std::move(inlines);
        TrimEdges(paragraph.inlines);
        blocks.push_back(Block::Of(std::move(paragraph)));
    }
    inlines.clear();
}

Inlines Reader::ReadInlineChildren(lxb_dom_node_t* node, std::size_t depth) {
    Blocks stray;
    Inlines inlines;
    ReadChildren(node, stray, inlines, depth + 1);
    // Block content inside an inline context is malformed HTML; lexbor will
    // usually have restructured it already, and anything left is flattened.
    for (const Block& block : stray) {
        if (const auto* paragraph = block.As<Paragraph>()) {
            inlines.insert(inlines.end(), paragraph->inlines.begin(), paragraph->inlines.end());
        }
    }
    return inlines;
}

Blocks Reader::ReadBlockChildren(lxb_dom_node_t* node, std::size_t depth) {
    Blocks blocks;
    Inlines inlines;
    ReadChildren(node, blocks, inlines, depth + 1);
    Flush(blocks, inlines);
    return blocks;
}

void Reader::ReadChildren(lxb_dom_node_t* node, Blocks& blocks, Inlines& inlines,
                          std::size_t depth) {
    for (lxb_dom_node_t* child = node->first_child; child != nullptr; child = child->next) {
        ReadNode(child, blocks, inlines, depth);
    }
}

void Reader::ReadTable(lxb_dom_element_t* element, Blocks& blocks, std::size_t depth) {
    Table table;

    // Walk rows wherever they are: directly under <table>, or inside thead,
    // tbody or tfoot. lexbor has already inserted the tbody the spec requires,
    // so this handles both shapes without caring which the source used.
    std::vector<std::pair<lxb_dom_element_t*, bool>> rows;  // element, is_header_section

    const std::function<void(lxb_dom_node_t*, bool)> collect = [&](lxb_dom_node_t* node,
                                                                   bool in_head) {
        for (lxb_dom_node_t* child = node->first_child; child != nullptr; child = child->next) {
            if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            auto* child_element = lxb_dom_interface_element(child);
            const std::string tag = TagName(child_element);

            if (tag == "tr") {
                rows.emplace_back(child_element, in_head);
            } else if (tag == "thead") {
                collect(child, true);
            } else if (tag == "tbody" || tag == "tfoot") {
                collect(child, false);
            } else if (tag == "caption") {
                table.caption = ReadInlineChildren(child, depth);
            } else if (tag == "colgroup") {
                for (lxb_dom_node_t* col = child->first_child; col != nullptr; col = col->next) {
                    if (col->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
                    TableColumn column;
                    if (const auto align = AlignmentFromAttribute(
                            Attribute(lxb_dom_interface_element(col), "align"))) {
                        column.alignment = align;
                    }
                    table.columns.push_back(column);
                }
            }
        }
    };
    collect(lxb_dom_interface_node(element), false);

    // Header rows must come first for the IR's headerRows count to mean anything.
    std::stable_partition(rows.begin(), rows.end(),
                          [](const auto& row) { return row.second; });

    for (const auto& [row_element, in_head] : rows) {
        TableRow row;
        bool all_header_cells = true;

        for (lxb_dom_node_t* cell = lxb_dom_interface_node(row_element)->first_child;
             cell != nullptr; cell = cell->next) {
            if (cell->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            auto* cell_element = lxb_dom_interface_element(cell);
            const std::string tag = TagName(cell_element);
            if (tag != "td" && tag != "th") continue;
            if (tag != "th") all_header_cells = false;

            TableCell out;
            const std::string colspan = Attribute(cell_element, "colspan");
            const std::string rowspan = Attribute(cell_element, "rowspan");
            if (!colspan.empty()) {
                out.colSpan = static_cast<std::uint32_t>(
                    std::clamp(std::atoi(colspan.c_str()), 1, 1000));
            }
            if (!rowspan.empty()) {
                out.rowSpan = static_cast<std::uint32_t>(
                    std::clamp(std::atoi(rowspan.c_str()), 1, 1000));
            }
            out.alignment = AlignmentFromAttribute(Attribute(cell_element, "align"));
            out.blocks = ReadBlockChildren(cell, depth);
            row.cells.push_back(std::move(out));
        }

        if (in_head || (table.rows.empty() && all_header_cells && !row.cells.empty())) {
            table.headerRows = static_cast<std::uint32_t>(table.rows.size() + 1);
        }
        table.rows.push_back(std::move(row));
    }

    // Insert the covered placeholders the IR expects, so that every writer can
    // index the grid by (row, column) without re-deriving spans. Section 8.4
    // describes the same second pass for OOXML's vMerge.
    std::size_t width = 0;
    for (const TableRow& row : table.rows) {
        std::size_t span = 0;
        for (const TableCell& cell : row.cells) span += cell.colSpan;
        width = std::max(width, span);
    }
    if (table.columns.size() < width) table.columns.resize(width);

    std::vector<std::vector<bool>> occupied(table.rows.size(), std::vector<bool>(width, false));
    for (std::size_t r = 0; r < table.rows.size(); ++r) {
        std::vector<TableCell> laid_out;
        std::size_t column = 0;

        for (TableCell& cell : table.rows[r].cells) {
            // Skip past columns a cell in an earlier row is still spanning down
            // into, emitting a placeholder for each.
            while (column < width && occupied[r][column]) {
                TableCell filler;
                filler.covered = true;
                laid_out.push_back(std::move(filler));
                ++column;
            }

            // Reserve the rows below for a vertical span. The columns this cell
            // covers in its OWN row are handled by the placeholders emitted just
            // after it, not by the occupancy map.
            for (std::uint32_t dr = 1; dr < cell.rowSpan && r + dr < table.rows.size(); ++dr) {
                for (std::uint32_t dc = 0; dc < cell.colSpan && column + dc < width; ++dc) {
                    occupied[r + dr][column + dc] = true;
                }
            }

            const std::uint32_t span = cell.colSpan;
            laid_out.push_back(std::move(cell));
            for (std::uint32_t dc = 1; dc < span && column + dc < width; ++dc) {
                TableCell filler;
                filler.covered = true;
                laid_out.push_back(std::move(filler));
            }
            column += span;
        }
        while (column < width) {
            TableCell filler;
            filler.covered = occupied[r][column];
            laid_out.push_back(std::move(filler));
            ++column;
        }
        table.rows[r].cells = std::move(laid_out);
    }

    if (!table.rows.empty()) blocks.push_back(Block::Of(std::move(table)));
}

void Reader::ReadElement(lxb_dom_element_t* element, const std::string& tag, Blocks& blocks,
                         Inlines& inlines, std::size_t depth) {
    lxb_dom_node_t* node = lxb_dom_interface_node(element);

    // --- Blocks --------------------------------------------------------------
    if (tag == "p") {
        Flush(blocks, inlines);
        Paragraph paragraph;
        paragraph.inlines = ReadInlineChildren(node, depth);
        TrimEdges(paragraph.inlines);
        paragraph.direct.alignment = AlignmentFromAttribute(Attribute(element, "align"));
        if (!paragraph.inlines.empty()) blocks.push_back(Block::Of(std::move(paragraph)));
        return;
    }

    if (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') {
        Flush(blocks, inlines);
        Heading heading;
        heading.level = tag[1] - '0';
        heading.inlines = ReadInlineChildren(node, depth);
        TrimEdges(heading.inlines);
        // An id in the source wins: links elsewhere in the document point at it.
        heading.anchorId = Attribute(element, "id");
        if (heading.anchorId.empty()) {
            heading.anchorId = Slug(PlainText({Block::Of(Paragraph{heading.inlines, {}, {},
                                                                   std::nullopt})}),
                                    anchors_);
        } else {
            anchors_.push_back(heading.anchorId);
        }
        blocks.push_back(Block::Of(std::move(heading)));
        return;
    }

    if (tag == "pre") {
        Flush(blocks, inlines);
        CodeBlock code;
        // <pre><code class="language-x"> is the universal convention.
        for (lxb_dom_node_t* child = node->first_child; child != nullptr; child = child->next) {
            if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            auto* child_element = lxb_dom_interface_element(child);
            if (TagName(child_element) != "code") continue;

            const std::string classes = Attribute(child_element, "class");
            const std::size_t at = classes.find("language-");
            if (at != std::string::npos) {
                const std::size_t begin = at + 9;
                const std::size_t end = classes.find(' ', begin);
                code.language = classes.substr(begin, end - begin);
            }
            break;
        }
        // Verbatim: <pre> is the one place where collapsing whitespace would
        // destroy the content rather than tidy it.
        code.text = TextOf(node);
        blocks.push_back(Block::Of(std::move(code)));
        return;
    }

    if (tag == "blockquote") {
        Flush(blocks, inlines);
        blocks.push_back(Block::Of(BlockQuote{ReadBlockChildren(node, depth)}));
        return;
    }

    if (tag == "ul" || tag == "ol" || tag == "dl") {
        Flush(blocks, inlines);
        ListBlock list;
        list.ordered = tag == "ol";
        if (list.ordered) {
            const std::string start = Attribute(element, "start");
            if (!start.empty()) list.start = std::atoi(start.c_str());
        }

        for (lxb_dom_node_t* child = node->first_child; child != nullptr; child = child->next) {
            if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            auto* child_element = lxb_dom_interface_element(child);
            const std::string child_tag = TagName(child_element);
            if (child_tag != "li" && child_tag != "dt" && child_tag != "dd") continue;

            ListItem item;
            item.blocks = ReadBlockChildren(child, depth);

            // A disabled checkbox as the first child is how every renderer emits
            // a GFM task list, so reading it back keeps the round trip intact.
            for (lxb_dom_node_t* grand = child->first_child; grand != nullptr;
                 grand = grand->next) {
                if (grand->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
                auto* grand_element = lxb_dom_interface_element(grand);
                if (TagName(grand_element) != "input") break;
                if (Attribute(grand_element, "type") == "checkbox") {
                    item.checked = HasAttribute(grand_element, "checked");
                }
                break;
            }
            list.items.push_back(std::move(item));
        }

        // Tight when no item wrapped its text in a paragraph, which is how a
        // renderer signals it.
        list.tight = std::all_of(list.items.begin(), list.items.end(), [](const ListItem& item) {
            return item.blocks.size() <= 1;
        });
        if (!list.items.empty()) blocks.push_back(Block::Of(std::move(list)));
        return;
    }

    if (tag == "table") {
        Flush(blocks, inlines);
        ReadTable(element, blocks, depth);
        return;
    }

    if (tag == "hr") {
        Flush(blocks, inlines);
        blocks.push_back(Block::Of(HorizontalRule{}));
        return;
    }

    if (tag == "figure") {
        Flush(blocks, inlines);
        Blocks inner = ReadBlockChildren(node, depth);
        // A figure is a wrapper. Its image and caption are the content, and the
        // caption becomes the image's title so the pairing survives.
        std::string caption;
        for (const Block& block : inner) {
            if (block.Is<Paragraph>()) {
                const std::string text = PlainText({block});
                if (!text.empty() && caption.empty()) caption = text;
            }
        }
        while (!caption.empty() && caption.back() == '\n') caption.pop_back();

        for (Block& block : inner) {
            if (auto* image = block.As<ImageBlock>(); image != nullptr && image->title.empty()) {
                image->title = caption;
            }
        }
        for (Block& block : inner) {
            if (block.Is<ImageBlock>()) blocks.push_back(std::move(block));
        }
        return;
    }

    // --- Inlines -------------------------------------------------------------
    if (tag == "em" || tag == "i" || tag == "cite" || tag == "var" || tag == "q") {
        inlines.push_back(Inline::Of(Emphasis{ReadInlineChildren(node, depth)}));
        return;
    }
    if (tag == "strong" || tag == "b" || tag == "mark") {
        inlines.push_back(Inline::Of(Strong{ReadInlineChildren(node, depth)}));
        return;
    }
    if (tag == "del" || tag == "s" || tag == "strike") {
        inlines.push_back(Inline::Of(Strikethrough{ReadInlineChildren(node, depth)}));
        return;
    }
    if (tag == "code" || tag == "kbd" || tag == "samp") {
        inlines.push_back(Inline::Of(InlineCode{TextOf(node)}));
        return;
    }
    if (tag == "br") {
        inlines.push_back(Inline::Of(LineBreak{true}));
        return;
    }
    if (tag == "a") {
        const std::string href = Attribute(element, "href");
        Inlines children = ReadInlineChildren(node, depth);
        if (href.empty()) {
            // An anchor with no href is a link target, not a link.
            inlines.insert(inlines.end(), std::make_move_iterator(children.begin()),
                           std::make_move_iterator(children.end()));
            return;
        }
        inlines.push_back(Inline::Of(Link{href, Attribute(element, "title"), std::move(children)}));
        return;
    }
    if (tag == "img") {
        InlineImage image;
        // The src stays a URL: we do not fetch anything (section 1, local only,
        // no network egress). A data: URI is decoded into the ResourceStore
        // below, because those bytes are already in hand.
        const std::string src = Attribute(element, "src");
        image.alt = Attribute(element, "alt");
        image.title = Attribute(element, "title");
        image.resource = src;

        if (src.starts_with("data:")) {
            const std::size_t comma = src.find(',');
            const std::size_t semi = src.find(';');
            if (comma != std::string::npos && semi != std::string::npos && semi < comma &&
                src.compare(semi, comma - semi, ";base64") == 0) {
                const std::string_view encoded(src.data() + comma + 1, src.size() - comma - 1);
                std::vector<std::byte> bytes;
                bytes.reserve(encoded.size() * 3 / 4);

                std::uint32_t accumulator = 0;
                int bits = 0;
                for (const char c : encoded) {
                    int value = -1;
                    if (c >= 'A' && c <= 'Z') value = c - 'A';
                    else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
                    else if (c >= '0' && c <= '9') value = c - '0' + 52;
                    else if (c == '+') value = 62;
                    else if (c == '/') value = 63;
                    else continue;  // '=' and whitespace

                    accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
                    bits += 6;
                    if (bits >= 8) {
                        bits -= 8;
                        bytes.push_back(static_cast<std::byte>((accumulator >> bits) & 0xFF));
                    }
                }
                const std::string mime(src.substr(5, semi - 5));
                image.resource = document_.res.Add(bytes, mime);
            }
        }
        inlines.push_back(Inline::Of(std::move(image)));
        return;
    }
    if (tag == "u") {
        Inlines children = ReadInlineChildren(node, depth);
        for (Inline& child : children) {
            if (auto* text = std::get_if<Text>(&child.node)) {
                text->style.underline = UnderlineStyle::kSingle;
            }
        }
        inlines.insert(inlines.end(), std::make_move_iterator(children.begin()),
                       std::make_move_iterator(children.end()));
        return;
    }
    if (tag == "sup" || tag == "sub") {
        Inlines children = ReadInlineChildren(node, depth);
        for (Inline& child : children) {
            if (auto* text = std::get_if<Text>(&child.node)) {
                text->style.vertical =
                    tag == "sup" ? VerticalAlign::kSuperscript : VerticalAlign::kSubscript;
            }
        }
        inlines.insert(inlines.end(), std::make_move_iterator(children.begin()),
                       std::make_move_iterator(children.end()));
        return;
    }
    if (tag == "abbr") {
        // The expansion is the useful part; the abbreviation element itself has
        // no IR node and inventing one for a single format would be wrong.
        ReadChildren(node, blocks, inlines, depth);
        return;
    }

    // Recognised as a block container we handle elsewhere (li, td, tr, ...) or
    // not recognised at all: transparent.
    ReadChildren(node, blocks, inlines, depth);
}

void Reader::ReadNode(lxb_dom_node_t* node, Blocks& blocks, Inlines& inlines, std::size_t depth) {
    if (depth > kMaxBlockDepth) {
        if (!truncated_) {
            truncated_ = true;
            Note("nesting-truncated",
                 "Content nested deeper than 100 levels was omitted. This usually means the file "
                 "is malformed.");
            result_.Incurred(Fidelity::Structural);
        }
        return;
    }

    switch (node->type) {
        case LXB_DOM_NODE_TYPE_TEXT: {
            auto* text_node = lxb_dom_interface_text(node);
            const std::string_view raw =
                ToView(text_node->char_data.data.data, text_node->char_data.data.length);
            AppendText(inlines, CollapseWhitespace(raw));
            return;
        }

        case LXB_DOM_NODE_TYPE_ELEMENT: {
            auto* element = lxb_dom_interface_element(node);
            const std::string tag = TagName(element);

            if (Classify(tag) == Handling::kSkip) return;
            ReadElement(element, tag, blocks, inlines, depth + 1);
            return;
        }

        case LXB_DOM_NODE_TYPE_DOCUMENT:
        case LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT:
            ReadChildren(node, blocks, inlines, depth);
            return;

        default:
            // Comments, processing instructions, doctype. Not content.
            return;
    }
}

}  // namespace

Result ReadHtml(std::string_view html, const OptionBag& /*options*/, doc::Document& into) {
    if (html.starts_with("\xEF\xBB\xBF")) html.remove_prefix(3);

    lxb_html_document_t* document = lxb_html_document_create();
    if (document == nullptr) {
        return Result::Error(Status::kResourceLimit,
                             "Could not allocate an HTML document to parse into.");
    }
    // lexbor owns the DOM until we destroy it, and every early return below has
    // to go through here.
    struct Guard {
        lxb_html_document_t* document;
        ~Guard() { lxb_html_document_destroy(document); }
    } guard{document};

    const lxb_status_t status = lxb_html_document_parse(
        document, reinterpret_cast<const lxb_char_t*>(html.data()), html.size());
    if (status != LXB_STATUS_OK) {
        // Genuinely rare: HTML5 parsing is defined for every byte sequence, so
        // this means allocation failure rather than bad input.
        return Result::Error(Status::kMalformedInput,
                             "The HTML parser could not process this file.");
    }

    Result result = Result::Ok();
    Reader reader(into, result);

    // Title and language come from the head, which the body walk deliberately
    // skips (<head> is on the skip list: its children are not content).
    std::size_t title_size = 0;
    if (const lxb_char_t* text = lxb_html_document_title(document, &title_size)) {
        into.meta.title = std::string(ToView(text, title_size));
    }

    // The language lives on <html lang="...">, which is the document element:
    // the first element child of the document node.
    for (lxb_dom_node_t* child =
             lxb_dom_interface_node(lxb_dom_interface_document(document))->first_child;
         child != nullptr; child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        into.meta.lang = Attribute(lxb_dom_interface_element(child), "lang");
        break;
    }

    lxb_html_body_element_t* body = lxb_html_document_body_element(document);
    if (body == nullptr) {
        // A fragment with no body element. Walk the whole document instead of
        // returning an empty result.
        Blocks blocks;
        Inlines inlines;
        reader.ReadNode(lxb_dom_interface_node(lxb_dom_interface_document(document)), blocks,
                        inlines, 0);
        reader.Flush(blocks, inlines);
        into.blocks = std::move(blocks);
        doc::Normalise(into.blocks);
        return result;
    }

    Blocks blocks;
    Inlines inlines;
    reader.ReadNode(lxb_dom_interface_node(body), blocks, inlines, 0);
    reader.Flush(blocks, inlines);
    into.blocks = std::move(blocks);
    // See document.h: adjacent same-kind spans are one span. The HTML reader
    // produces them routinely, because <em>a</em><em>b</em> is legal markup.
    doc::Normalise(into.blocks);
    return result;
}

std::vector<MediaProfile> HtmlReader::Reads() const {
    return {*MediaProfile::Parse(kHtmlProfileId)};
}

EdgeCost HtmlReader::ReadCost(const MediaProfile& /*from*/) const {
    EdgeCost cost;
    // Lossless into the IR only in the sense rule 1 means it: everything the
    // semantic whitelist recognises is kept. Presentational markup is not
    // "lost", it was never content.
    cost.fidelity = Fidelity::Lossless;
    cost.generational = 0;
    cost.cpuPerMiB = 9.0f;
    cost.memPeakFactor = 8.0f;  // a DOM is expensive relative to its source text
    cost.requiresDisk = false;
    return cost;
}

std::optional<MediaProfile> HtmlReader::Probe(ISource& source, const PathHint& hint) const {
    const std::vector<std::byte> prefix = ReadAtMost(source, 8 * 1024);
    if (prefix.empty()) return std::nullopt;

    std::string head(reinterpret_cast<const char*>(prefix.data()),
                     std::min<std::size_t>(prefix.size(), 2048));
    std::transform(head.begin(), head.end(), head.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Structural evidence, not the extension. A file claiming to be .html that
    // contains no markup at all is not HTML, and a .txt file that opens with a
    // doctype is.
    static constexpr std::string_view kMarkers[] = {"<!doctype html", "<html", "<head", "<body",
                                                    "<meta", "<div", "<p>", "<span"};
    const bool looks_like_html =
        std::any_of(std::begin(kMarkers), std::end(kMarkers),
                    [&head](std::string_view marker) { return head.find(marker) != std::string::npos; });
    if (!looks_like_html) return std::nullopt;

    for (const std::byte b : prefix) {
        const auto c = static_cast<unsigned char>(b);
        if (c < 0x09 || (c > 0x0D && c < 0x20)) return std::nullopt;  // binary
    }
    (void)hint;
    return MediaProfile::Parse(kHtmlProfileId);
}

Result HtmlReader::Read(ConvertContext& context, ISource& source, const MediaProfile& /*from*/,
                        doc::Document& into) {
    if (context.cancelled()) return Result::Cancelled();

    constexpr std::uint64_t kMaxBytes = 128ull * 1024 * 1024;
    std::string html;
    if (const auto size = source.Size()) {
        if (*size > kMaxBytes) {
            return Result::Error(Status::kResourceLimit, "This HTML file is larger than 128 MB.");
        }
        html.reserve(static_cast<std::size_t>(*size));
    }

    std::vector<std::byte> chunk(kStreamChunkBytes);
    while (true) {
        const std::size_t got = source.Read(chunk);
        if (got == 0) break;
        html.append(reinterpret_cast<const char*>(chunk.data()), got);
        if (html.size() > kMaxBytes) {
            return Result::Error(Status::kResourceLimit, "This HTML file is larger than 128 MB.");
        }
        if (context.cancelled()) return Result::Cancelled();
    }

    context.progress.Report(0.4, 0);
    Result result = ReadHtml(html, context.options, into);
    context.progress.Report(1.0, 0);
    return result;
}

}  // namespace morph::dochtml
