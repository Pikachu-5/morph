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

// The same three traversals again, non-const, for Normalise. Duplicated rather
// than made generic because a const/non-const template pair over std::visit with
// Overloaded needs more machinery than the six lines it would save, and the
// comment above -- one place per container kind -- is what actually matters.
template <typename Fn>
void ForEachChildBlocksMutable(Block& block, Fn&& fn) {
    std::visit(Overloaded{
                   [&](BlockQuote& b) { fn(b.blocks); },
                   [&](Footnote& b) { fn(b.blocks); },
                   [&](ListBlock& b) {
                       for (ListItem& item : b.items) fn(item.blocks);
                   },
                   [&](Table& b) {
                       for (TableRow& row : b.rows) {
                           for (TableCell& cell : row.cells) fn(cell.blocks);
                       }
                   },
                   [](auto&) {},
               },
               block.node);
}

template <typename Fn>
void ForEachChildInlinesMutable(Inline& node, Fn&& fn) {
    std::visit(Overloaded{
                   [&](Emphasis& n) { fn(n.children); },
                   [&](Strong& n) { fn(n.children); },
                   [&](Strikethrough& n) { fn(n.children); },
                   [&](Link& n) { fn(n.children); },
                   [](auto&) {},
               },
               node.node);
}

template <typename Fn>
void ForEachOwnInlinesMutable(Block& block, Fn&& fn) {
    std::visit(Overloaded{
                   [&](Paragraph& b) { fn(b.inlines); },
                   [&](Heading& b) { fn(b.inlines); },
                   [&](Table& b) { fn(b.caption); },
                   [](auto&) {},
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

void Normalise(Inlines& inlines) {
    const auto childrenOf = [](Inline& node) -> Inlines* {
        if (auto* e = std::get_if<Emphasis>(&node.node)) return &e->children;
        if (auto* s = std::get_if<Strong>(&node.node)) return &s->children;
        if (auto* d = std::get_if<Strikethrough>(&node.node)) return &d->children;
        return nullptr;
    };

    // Depth first, so that the sibling pass below sees children that are already
    // normalised and a span merged out of two does not need a second visit.
    for (Inline& node : inlines) {
        ForEachChildInlinesMutable(node, [](Inlines& children) { Normalise(children); });
    }

    // BOLD INSIDE BOLD IS BOLD. No format here can express the nesting -- Word
    // has one b property per run, and "**a**b**c**" in Markdown closes the outer
    // span rather than opening an inner one -- so a wrapper directly inside the
    // same kind of wrapper is spliced into its parent. The alternative is a
    // writer that emits delimiters which reopen instead of nesting, and a round
    // trip that reshuffles the document every time.
    for (std::size_t i = 0; i < inlines.size(); ++i) {
        Inlines* mine = childrenOf(inlines[i]);
        if (mine == nullptr) continue;
        for (std::size_t j = 0; j < mine->size();) {
            if ((*mine)[j].node.index() != inlines[i].node.index()) {
                ++j;
                continue;
            }
            Inlines* nested = childrenOf((*mine)[j]);
            Inlines lifted = std::move(*nested);
            mine->erase(mine->begin() + static_cast<std::ptrdiff_t>(j));
            mine->insert(mine->begin() + static_cast<std::ptrdiff_t>(j),
                         std::make_move_iterator(lifted.begin()),
                         std::make_move_iterator(lifted.end()));
            // j is not advanced: what was spliced in may itself need merging
            // with what follows, and the sibling pass below handles that.
        }
        Normalise(*mine);
    }

    for (std::size_t i = 0; i + 1 < inlines.size();) {
        // Same alternative is the precondition for both merges, and the variant
        // index answers that without naming eleven types.
        if (inlines[i].node.index() != inlines[i + 1].node.index()) {
            ++i;
            continue;
        }

        if (auto* first = std::get_if<Text>(&inlines[i].node)) {
            auto* second = std::get_if<Text>(&inlines[i + 1].node);
            // Different styling is a real boundary: two runs in different
            // colours are two runs, and merging them would lose one colour.
            if (!(first->style == second->style)) {
                ++i;
                continue;
            }
            first->text += second->text;
        } else if (auto* firstCode = std::get_if<InlineCode>(&inlines[i].node)) {
            // Two code spans touching are one code span. Markdown cannot spell
            // the difference at all -- "`a``b`" is not two spans, it is a
            // backtick soup that parses as neither -- and Word merges the runs
            // on the way in. HTML can technically hold the distinction, and it
            // is a distinction without a meaning.
            firstCode->text += std::get<InlineCode>(inlines[i + 1].node).text;
        } else if (Inlines* mine = childrenOf(inlines[i])) {
            Inlines* theirs = childrenOf(inlines[i + 1]);
            mine->insert(mine->end(), std::make_move_iterator(theirs->begin()),
                         std::make_move_iterator(theirs->end()));
        } else {
            // Two links side by side are two links: they have different
            // targets, and merging them would silently drop one.
            ++i;
            continue;
        }
        inlines.erase(inlines.begin() + static_cast<std::ptrdiff_t>(i) + 1);
    }

    // Once more over the children, because merging two spans into one produces a
    // child list nobody has looked at yet: "*one*" and "*two*" become one span
    // whose children are two text runs, and those are exactly what the first
    // rule above says must be joined.
    for (Inline& node : inlines) {
        ForEachChildInlinesMutable(node, [](Inlines& children) { Normalise(children); });
    }
}

void Normalise(Blocks& blocks) {
    for (Block& block : blocks) {
        ForEachOwnInlinesMutable(block, [](Inlines& inlines) { Normalise(inlines); });
        ForEachChildBlocksMutable(block, [](Blocks& children) { Normalise(children); });
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
