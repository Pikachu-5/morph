#include "morph/core/doc/document.h"

#include <algorithm>

namespace morph::doc {
namespace {

// Child blocks of a block, wherever they hide. Kept in one place so that adding
// a new container node cannot be half-supported by some traversals.
template <typename Fn>
void ForEachChildBlocks(const Block& block, Fn&& fn) {
    std::visit(Overloaded{
                   [&](const BlockQuote& b) { fn(b.blocks); },
                   [&](const Footnote& b) { fn(b.blocks); },
                   [&](const ListBlock& b) {
                       for (const ListItem& item : b.items) fn(item.blocks);
                   },
                   [&](const Table& b) {
                       for (const TableRow& row : b.rows) {
                           for (const TableCell& cell : row.cells) fn(cell.blocks);
                       }
                   },
                   [](const auto&) {},
               },
               block.node);
}

template <typename Fn>
void ForEachChildInlines(const Inline& node, Fn&& fn) {
    std::visit(Overloaded{
                   [&](const Emphasis& n) { fn(n.children); },
                   [&](const Strong& n) { fn(n.children); },
                   [&](const Strikethrough& n) { fn(n.children); },
                   [&](const Link& n) { fn(n.children); },
                   [](const auto&) {},
               },
               node.node);
}

// Inline runs a block owns directly.
template <typename Fn>
void ForEachOwnInlines(const Block& block, Fn&& fn) {
    std::visit(Overloaded{
                   [&](const Paragraph& b) { fn(b.inlines); },
                   [&](const Heading& b) { fn(b.inlines); },
                   [&](const Table& b) { fn(b.caption); },
                   [](const auto&) {},
               },
               block.node);
}

void AppendPlainText(const Inlines& inlines, std::string& out);

void AppendPlainText(const Inline& node, std::string& out) {
    std::visit(Overloaded{
                   [&](const Text& n) { out += n.text; },
                   [&](const InlineCode& n) { out += n.text; },
                   [&](const Math& n) { out += n.tex; },
                   [&](const LineBreak&) { out += '\n'; },
                   [&](const InlineImage& n) { out += n.alt; },
                   [&](const Emphasis& n) { AppendPlainText(n.children, out); },
                   [&](const Strong& n) { AppendPlainText(n.children, out); },
                   [&](const Strikethrough& n) { AppendPlainText(n.children, out); },
                   [&](const Link& n) { AppendPlainText(n.children, out); },
                   // A footnote reference and raw inline contribute no text: the
                   // footnote's own body is a block and is visited separately.
                   [](const auto&) {},
               },
               node.node);
}

void AppendPlainText(const Inlines& inlines, std::string& out) {
    for (const Inline& node : inlines) AppendPlainText(node, out);
}

void AppendPlainText(const Blocks& blocks, std::string& out) {
    for (const Block& block : blocks) {
        std::visit(Overloaded{
                       [&](const CodeBlock& b) { out += b.text; },
                       [&](const ImageBlock& b) { out += b.alt; },
                       [](const auto&) {},
                   },
                   block.node);

        ForEachOwnInlines(block, [&](const Inlines& inlines) { AppendPlainText(inlines, out); });
        if (!out.empty() && out.back() != '\n') out += '\n';
        ForEachChildBlocks(block, [&](const Blocks& children) { AppendPlainText(children, out); });
    }
}

}  // namespace

void ForEachBlock(const Blocks& blocks, const std::function<void(const Block&)>& visit) {
    for (const Block& block : blocks) {
        visit(block);
        ForEachChildBlocks(block, [&](const Blocks& children) { ForEachBlock(children, visit); });
    }
}

void ForEachInline(const Inlines& inlines, const std::function<void(const Inline&)>& visit) {
    for (const Inline& node : inlines) {
        visit(node);
        ForEachChildInlines(node,
                            [&](const Inlines& children) { ForEachInline(children, visit); });
    }
}

std::vector<ResourceId> ReferencedResources(const Document& document) {
    std::vector<ResourceId> out;
    const auto remember = [&out](const ResourceId& id) {
        if (id.empty()) return;
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    };

    ForEachBlock(document.blocks, [&](const Block& block) {
        if (const auto* image = block.As<ImageBlock>()) remember(image->resource);

        ForEachOwnInlines(block, [&](const Inlines& inlines) {
            ForEachInline(inlines, [&](const Inline& node) {
                if (const auto* image = node.As<InlineImage>()) remember(image->resource);
            });
        });
    });
    return out;
}

std::string PlainText(const Blocks& blocks) {
    std::string out;
    AppendPlainText(blocks, out);
    return out;
}

std::size_t CountBlocks(const Blocks& blocks) {
    std::size_t count = 0;
    ForEachBlock(blocks, [&count](const Block&) { ++count; });
    return count;
}

std::size_t MaxDepth(const Blocks& blocks) {
    std::size_t deepest = 0;
    for (const Block& block : blocks) {
        std::size_t below = 0;
        ForEachChildBlocks(block, [&below](const Blocks& children) {
            below = std::max(below, MaxDepth(children));
        });
        deepest = std::max(deepest, below + 1);
    }
    return deepest;
}

}  // namespace morph::doc
