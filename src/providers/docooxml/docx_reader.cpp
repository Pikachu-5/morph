// DOCX -> IR. Section 8.4.
//
// The four genuinely tricky mappings, in the order section 8.4 says you hit
// them, and where each lives in this file:
//
//   1. RUNS TO INLINES. Word splits a sentence into arbitrarily many w:r for
//      reasons unrelated to formatting -- spell-check state, revision marks,
//      rsid tracking. Adjacent runs with identical RESOLVED formatting are
//      merged before emitting, or a five-word paragraph becomes eleven IR
//      nodes.  ->  ParagraphBuilder::AddRun
//
//   2. LISTS. There is no list element in WordprocessingML. Reconstructing a
//      tree from a flat sequence of paragraphs sharing a numId, handling level
//      skips and interleaved non-list paragraphs, is the single fiddliest
//      function in the reader.  ->  ListAssembler
//
//   3. TABLES. w:gridSpan is colspan and is easy. w:vMerge is rowspan expressed
//      as a start marker plus continuation markers on later rows, which is not.
//      Build the grid, then resolve merges in a second pass.  ->  ReadTable
//
//   4. NUMBERING RESTARTS. Handled one layer down, in docx_numbering.cpp.

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include <pugixml.hpp>

#include "docx_numbering.h"
#include "docx_styles.h"
#include "docx_xml.h"
#include "morph/providers/docx.h"
#include "morph/providers/opc.h"

namespace morph::docooxml {
namespace {

using namespace morph::doc;

// --- Mapping 1: runs to inlines ---------------------------------------------

// Accumulates inline content for one paragraph, merging adjacent runs that
// resolve to the same formatting.
class ParagraphBuilder {
public:
    void AddRun(std::string text, const RunFormat& format) {
        if (text.empty()) return;

        // The merge that mapping 1 is about. Word emits a new w:r whenever the
        // spell-check state or an rsid changes, so a single typed sentence
        // routinely arrives as five or six runs with identical rPr.
        if (!pending_.empty() && format.SameAs(pendingFormat_)) {
            pending_ += text;
            return;
        }
        Flush();
        pending_ = std::move(text);
        pendingFormat_ = format;
    }

    void AddInline(Inline node) {
        Flush();
        inlines_.push_back(std::move(node));
    }

    [[nodiscard]] Inlines Take() {
        Flush();
        // WordprocessingML has no inline nesting at all -- italic is a property
        // of a run -- so a sentence that is italic throughout arrives as one run
        // per change of anything else, and a code span or a spelling boundary in
        // the middle of it splits the reconstruction into two adjacent spans.
        // Normalise puts them back together (document.h says why it must).
        Normalise(inlines_);
        return std::move(inlines_);
    }

    [[nodiscard]] bool Empty() const { return inlines_.empty() && pending_.empty(); }

private:
    void Flush() {
        if (pending_.empty()) return;

        Inline node = Inline::Of(Text{});
        if (pendingFormat_.code.value_or(false)) {
            // InlineCode carries text and nothing else, which is right: a code
            // span's font IS the code span. What the run also said about family
            // or size is subsumed rather than dropped.
            node = Inline::Of(InlineCode{std::move(pending_)});
        } else {
            Text text;
            text.text = std::move(pending_);
            text.style = pendingFormat_.style;
            node = Inline::Of(std::move(text));
        }

        // Bold, italic and strikethrough become semantic nodes rather than
        // style, because every target format has a construct for them. Nesting
        // order is fixed so that two runs with the same formatting always
        // produce the same tree.
        if (pendingFormat_.strike.value_or(false)) {
            node = Inline::Of(Strikethrough{{std::move(node)}});
        }
        if (pendingFormat_.italic.value_or(false)) {
            node = Inline::Of(Emphasis{{std::move(node)}});
        }
        if (pendingFormat_.bold.value_or(false)) {
            node = Inline::Of(Strong{{std::move(node)}});
        }

        inlines_.push_back(std::move(node));
        pending_.clear();
        pendingFormat_ = RunFormat{};
    }

    Inlines inlines_;
    std::string pending_;
    RunFormat pendingFormat_;
};

// --- Mapping 2: lists --------------------------------------------------------

// Turns a flat run of numbered paragraphs into a tree of ListBlocks.
//
// The invariant: `open_` is the stack of currently open lists, outermost first,
// and open_[i] is at ilvl levels_[i]. A paragraph at level L closes every open
// list deeper than L, opens nested lists if L is deeper than the top, and
// appends its item to whatever is then on top.
class ListAssembler {
public:
    explicit ListAssembler(Blocks& output) : output_(&output) {}

    // A numbered paragraph. `numId` and `level` come from w:numPr.
    void Add(const std::string& numId, int level, bool ordered, int start, Blocks itemBlocks,
             std::optional<bool> checked) {
        // A different numId is a different list, even at the same level. This is
        // how Word says "a new list that looks the same", and merging them
        // numbers the second list continuously from the first.
        if (!open_.empty() && numId != numId_) Close();
        numId_ = numId;

        // Level skips are legal: a document can jump from ilvl 0 to ilvl 2. The
        // intermediate lists have to exist for the tree to be well formed, so
        // they are synthesised with a single empty item to hang the child off.
        while (static_cast<int>(open_.size()) > level + 1) Pop();

        while (static_cast<int>(open_.size()) < level + 1) {
            ListBlock list;
            list.ordered = ordered;
            list.start = start;
            list.tight = true;

            if (open_.empty()) {
                open_.push_back(std::move(list));
            } else {
                // Nest inside the previous item at the level above. If there is
                // no previous item -- a document that opens at ilvl 2 -- an
                // empty one is created to hold it.
                if (open_.back().items.empty()) open_.back().items.push_back(ListItem{});
                pendingNest_.push_back(std::move(list));
                open_.push_back(std::move(pendingNest_.back()));
                pendingNest_.pop_back();
            }
        }

        // The outermost list's ordering is set by its first item; a level that
        // opens as a bullet and continues as a number is not something the IR or
        // any target format expresses.
        ListItem item;
        item.blocks = std::move(itemBlocks);
        item.checked = checked;
        if (item.blocks.size() > 1) open_.back().tight = false;
        open_.back().items.push_back(std::move(item));
    }

    // Any non-list content ends the run of list paragraphs. Word expresses list
    // continuation paragraphs through indentation rather than numbering, and
    // guessing at that is a heuristic the fidelity contract would have to
    // apologise for; ending the list is the honest reading.
    void Close() {
        while (!open_.empty()) Pop();
        numId_.clear();
    }

    [[nodiscard]] bool Open() const { return !open_.empty(); }

private:
    void Pop() {
        ListBlock finished = std::move(open_.back());
        open_.pop_back();

        if (open_.empty()) {
            output_->push_back(Block::Of(std::move(finished)));
            return;
        }
        // Nested lists hang off the last item of the list above.
        if (open_.back().items.empty()) open_.back().items.push_back(ListItem{});
        open_.back().items.back().blocks.push_back(Block::Of(std::move(finished)));
        open_.back().tight = false;
    }

    Blocks* output_;
    std::vector<ListBlock> open_;
    std::vector<ListBlock> pendingNest_;
    std::string numId_;
};

// --- The reader --------------------------------------------------------------

class DocxBody {
public:
    DocxBody(const opc::Package& package, std::string documentPart, Document& into,
             Result& result)
        : package_(package),
          documentPart_(std::move(documentPart)),
          document_(into),
          result_(result) {}

    void Load(const pugi::xml_node& stylesRoot, const pugi::xml_node& numberingRoot) {
        styles_.Load(stylesRoot);
        numbering_.Load(numberingRoot);
        document_.styles = styles_.BuildTable();
    }

    void ReadBody(const pugi::xml_node& body);

private:
    void ReadBlockLevel(const pugi::xml_node& node, Blocks& out, ListAssembler& lists,
                        std::size_t depth);
    void ReadParagraph(const pugi::xml_node& p, Blocks& out, ListAssembler& lists,
                       std::size_t depth);

    // Word has no code block element: a run of paragraphs sharing a code style
    // IS the representation. They are accumulated here and emitted as one
    // CodeBlock when anything else interrupts them, which is the inverse of the
    // writer emitting one paragraph per line.
    void FlushCode(Blocks& out);

    // Word has no block quote element either: a run of paragraphs sharing a
    // quote style IS one. Same shape as FlushCode, and for the same reason --
    // our own writer emits exactly this, so reading it back is what keeps
    // docx -> md -> docx from degrading a quotation into plain prose.
    void FlushQuote(Blocks& out);
    void ReadTable(const pugi::xml_node& tbl, Blocks& out, std::size_t depth);
    void ReadRuns(const pugi::xml_node& parent, ParagraphBuilder& builder,
                  const std::string& paragraphStyleId, std::size_t depth);
    void ReadRun(const pugi::xml_node& r, ParagraphBuilder& builder, const RunFormat& format);
    void ReadDrawing(const pugi::xml_node& drawing, ParagraphBuilder& builder);
    void ReadLegacyPicture(const pugi::xml_node& pict, ParagraphBuilder& builder);

    [[nodiscard]] ResourceId ResolveImage(const std::string& relationshipId);
    [[nodiscard]] std::string MakeAnchor(const Inlines& inlines);

    void Note(std::string code, std::string detail, Fidelity incurred = Fidelity::Structural) {
        // One note per code. A document with four hundred content controls
        // should say so once, not four hundred times.
        if (std::find(noteCodes_.begin(), noteCodes_.end(), code) != noteCodes_.end()) return;
        noteCodes_.push_back(code);
        result_.Note(std::move(code), std::move(detail));
        result_.Incurred(incurred);
    }

    const opc::Package& package_;
    std::string documentPart_;
    Document& document_;
    Result& result_;

    StyleResolver styles_;
    NumberingTable numbering_;
    std::vector<std::string> anchors_;
    std::vector<std::string> noteCodes_;
    std::size_t blockCount_ = 0;

    std::string pendingCode_;
    bool hasPendingCode_ = false;
    Blocks pendingQuote_;
};

void DocxBody::FlushQuote(Blocks& out) {
    if (pendingQuote_.empty()) return;
    out.push_back(Block::Of(BlockQuote{std::move(pendingQuote_)}));
    pendingQuote_.clear();
}

void DocxBody::FlushCode(Blocks& out) {
    if (!hasPendingCode_) return;
    hasPendingCode_ = false;

    // The last line contributed a trailing newline; a CodeBlock's text ends with
    // exactly one.
    while (pendingCode_.size() > 1 && pendingCode_.ends_with("\n\n")) pendingCode_.pop_back();
    out.push_back(Block::Of(CodeBlock{"", std::move(pendingCode_)}));
    pendingCode_.clear();
}

std::string DocxBody::MakeAnchor(const Inlines& inlines) {
    std::string text = PlainText({Block::Of(Paragraph{inlines, {}, {}, std::nullopt})});
    while (!text.empty() && text.back() == '\n') text.pop_back();

    std::string slug;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 'A' && u <= 'Z') slug.push_back(static_cast<char>(u - 'A' + 'a'));
        else if ((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-' || u == '_' ||
                 u >= 0x80) {
            slug.push_back(static_cast<char>(u));
        } else if (u == ' ') {
            slug.push_back('-');
        }
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    if (slug.empty()) slug = "section";

    std::string candidate = slug;
    for (int i = 1; std::find(anchors_.begin(), anchors_.end(), candidate) != anchors_.end();
         ++i) {
        candidate = slug + "-" + std::to_string(i);
    }
    anchors_.push_back(candidate);
    return candidate;
}

ResourceId DocxBody::ResolveImage(const std::string& relationshipId) {
    if (relationshipId.empty()) return {};

    const opc::Relationship* relationship =
        package_.FindRelationship(documentPart_, relationshipId);
    if (relationship == nullptr) return {};

    if (relationship->external) {
        // An image linked rather than embedded. Section 1 is local only, with no
        // network egress at all, so the link is preserved and never followed.
        Note("linked-image-not-embedded",
             "An image is linked from outside the document rather than stored in it. Morph does "
             "not fetch external content, so it was left as a link.");
        return relationship->target;
    }

    const std::string target = opc::Package::ResolveTarget(documentPart_, relationship->target);
    const opc::Part* part = package_.Find(target);
    if (part == nullptr) return {};

    // Content-addressed, so the same logo repeated in forty places is stored
    // once (section 8.2).
    std::string mime = part->contentType;
    if (mime.empty()) mime = std::string(SniffImageMime(part->bytes));
    return document_.res.Add(part->bytes, std::move(mime), target);
}

void DocxBody::ReadDrawing(const pugi::xml_node& drawing, ParagraphBuilder& builder) {
    // wp:inline and wp:anchor differ only in how the image floats, which the IR
    // does not express. Both carry the same graphic underneath.
    for (pugi::xml_node holder = drawing.first_child(); holder;
         holder = holder.next_sibling()) {
        const std::string_view kind = LocalName(holder);
        if (kind != "inline" && kind != "anchor") continue;

        const pugi::xml_node blip =
            Descend(holder, "graphic", "graphicData", "pic", "blipFill", "blip");
        if (!blip) {
            // A chart, a diagram, a shape: recognised structurally, not
            // mappable. Rule 3's territory, and there is nothing to rasterise
            // without a renderer.
            Note("drawing-dropped",
                 "A chart, diagram or drawing could not be converted and was omitted. Only "
                 "pictures are supported.");
            continue;
        }

        InlineImage image;
        image.resource = ResolveImage(Attr(blip, "embed"));
        if (image.resource.empty()) {
            Note("image-missing",
                 "An image referenced by this document is missing from the file and was skipped.");
            continue;
        }

        // The docPr element carries the accessibility description, which is
        // exactly the alt text every other format wants.
        if (const pugi::xml_node properties = Child(holder, "docPr")) {
            image.alt = Attr(properties, "descr");
            if (image.alt.empty()) image.alt = Attr(properties, "title");
            image.title = Attr(properties, "title");
        }

        if (const pugi::xml_node extent = Child(holder, "extent")) {
            if (const auto cx = IntAttr(extent, "cx")) image.widthPt = EmuToPoints(*cx);
            if (const auto cy = IntAttr(extent, "cy")) image.heightPt = EmuToPoints(*cy);
        }

        builder.AddInline(Inline::Of(std::move(image)));
    }
}

void DocxBody::ReadLegacyPicture(const pugi::xml_node& pict, ParagraphBuilder& builder) {
    // The pre-2007 VML shape. Still emitted by some producers and by Word itself
    // for a few shapes, so it is worth the fifteen lines.
    const std::function<pugi::xml_node(const pugi::xml_node&)> find_image_data =
        [&](const pugi::xml_node& node) -> pugi::xml_node {
        for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling()) {
            if (LocalName(child) == "imagedata") return child;
            if (const pugi::xml_node found = find_image_data(child)) return found;
        }
        return {};
    };

    const pugi::xml_node data = find_image_data(pict);
    if (!data) return;

    InlineImage image;
    image.resource = ResolveImage(Attr(data, "id"));
    if (image.resource.empty()) return;
    image.alt = Attr(data, "title");
    builder.AddInline(Inline::Of(std::move(image)));
}

void DocxBody::ReadRun(const pugi::xml_node& r, ParagraphBuilder& builder,
                       const RunFormat& format) {
    for (pugi::xml_node child = r.first_child(); child; child = child.next_sibling()) {
        const std::string_view name = LocalName(child);

        if (name == "t") {
            // xml:space="preserve" is what keeps a run that is a single space
            // from vanishing. pugixml preserves the text either way; what
            // matters is that we do not trim it ourselves.
            builder.AddRun(child.text().as_string(), format);
        } else if (name == "tab") {
            builder.AddRun("\t", format);
        } else if (name == "br") {
            const std::string type = Attr(child, "type");
            // A page break has no IR node and no equivalent in Markdown or
            // HTML; treating it as a hard line break is the closest honest
            // reading.
            builder.AddInline(Inline::Of(LineBreak{/*hard=*/true}));
            if (type == "page") {
                Note("page-break-flattened",
                     "Page breaks became line breaks; the target format has no notion of pages.");
            }
        } else if (name == "cr") {
            builder.AddInline(Inline::Of(LineBreak{/*hard=*/true}));
        } else if (name == "noBreakHyphen") {
            builder.AddRun("‑", format);  // U+2011 NON-BREAKING HYPHEN
        } else if (name == "softHyphen") {
            builder.AddRun("­", format);  // U+00AD SOFT HYPHEN
        } else if (name == "sym") {
            // A symbol-font character. The char attribute is a hex code point in
            // the private use area of a font we do not have, so the glyph is
            // genuinely unknowable; a replacement is more honest than a guess.
            builder.AddRun("�", format);
            Note("symbol-dropped",
                 "A character from a symbol font could not be represented and was replaced.");
        } else if (name == "drawing") {
            ReadDrawing(child, builder);
        } else if (name == "pict") {
            ReadLegacyPicture(child, builder);
        } else if (name == "object") {
            // An embedded OLE object: a spreadsheet, an equation, another
            // document. Rule 3's canonical case, and nothing can be done with it
            // without the application that owns it.
            Note("embedded-object-dropped",
                 "An embedded object (such as a spreadsheet or equation) could not be converted "
                 "and was omitted.");
        }
    }
}

void DocxBody::ReadRuns(const pugi::xml_node& parent, ParagraphBuilder& builder,
                        const std::string& paragraphStyleId, std::size_t depth) {
    if (depth > kMaxBlockDepth) return;

    for (pugi::xml_node child = parent.first_child(); child; child = child.next_sibling()) {
        const std::string_view name = LocalName(child);

        if (name == "r") {
            const std::string characterStyle = Attr(Descend(child, "rPr", "rStyle"), "val");
            RunFormat format = ParseRunProperties(Child(child, "rPr"));
            format.Underlay(styles_.ResolveRun(characterStyle));
            // A code span is a character style, not an element. Matched by the
            // same name table the code-block case uses, so a document from Word,
            // from pandoc or from us all read the same way.
            if (!characterStyle.empty() && styles_.IsCodeStyle(characterStyle)) {
                format.code = true;
            }
            ReadRun(child, builder, format);
            continue;
        }

        if (name == "hyperlink") {
            ParagraphBuilder inner;
            ReadRuns(child, inner, paragraphStyleId, depth + 1);
            Inlines children = inner.Take();
            if (children.empty()) continue;

            Link link;
            if (const std::string id = Attr(child, "id"); !id.empty()) {
                if (const opc::Relationship* relationship =
                        package_.FindRelationship(documentPart_, id)) {
                    link.target = relationship->target;
                }
            }
            if (link.target.empty()) {
                // An internal link to a bookmark.
                if (const std::string anchor = Attr(child, "anchor"); !anchor.empty()) {
                    link.target = "#" + anchor;
                }
            }
            link.title = Attr(child, "tooltip");
            link.children = std::move(children);

            if (link.target.empty()) {
                // A hyperlink with no destination is just text.
                for (Inline& node : link.children) builder.AddInline(std::move(node));
            } else {
                builder.AddInline(Inline::Of(std::move(link)));
            }
            continue;
        }

        if (name == "sdt") {
            // A content control. Section 8.2's rule 3 nominates these for
            // RawBlock, but they are how every Word template expresses a title,
            // an author and a date -- treating one as opaque would make
            // docx -> md silently drop the document's title. So the CONTENT is
            // read normally and the wrapper is reported as flattened. The
            // trade-off is that a DOCX -> IR -> DOCX round trip loses the
            // control itself, which is the conversion nobody asks for.
            Note("content-control-flattened",
                 "Content controls were replaced by the text they currently contain.");
            ReadRuns(Child(child, "sdtContent"), builder, paragraphStyleId, depth + 1);
            continue;
        }

        if (name == "smartTag" || name == "bookmarkStart" || name == "proofErr" ||
            name == "commentRangeStart" || name == "commentRangeEnd") {
            // Wrappers with no content of their own, or markers we do not carry.
            ReadRuns(child, builder, paragraphStyleId, depth + 1);
            continue;
        }

        if (name == "ins") {
            // A tracked insertion. P4 is out of scope, and the inserted text is
            // part of the document as it currently reads, so it is kept.
            ReadRuns(child, builder, paragraphStyleId, depth + 1);
            continue;
        }
        if (name == "del") {
            // A tracked deletion: text the author removed. Keeping it would put
            // deleted text back into the document, which is worse than dropping
            // it and is a genuine confidentiality problem.
            Note("tracked-deletion-dropped",
                 "Text marked as deleted by tracked changes was not included.");
            continue;
        }

        if (name == "fldSimple") {
            // A field. Its children are Word's cached rendering of it, which is
            // exactly what the reader should show: a page number renders as the
            // number, not as "PAGE \\* MERGEFORMAT".
            ReadRuns(child, builder, paragraphStyleId, depth + 1);
            continue;
        }
    }
}

void DocxBody::ReadParagraph(const pugi::xml_node& p, Blocks& out, ListAssembler& lists,
                             std::size_t depth) {
    const pugi::xml_node pPr = Child(p, "pPr");
    const std::string styleId = Attr(Child(pPr, "pStyle"), "val");

    ParagraphBuilder builder;
    ReadRuns(p, builder, styleId, depth);
    Inlines inlines = builder.Take();

    // Numbering can come from the paragraph or from its style.
    std::string numId;
    int level = 0;
    if (const pugi::xml_node numPr = Child(pPr, "numPr")) {
        if (const auto id = IntAttr(Child(numPr, "numId"), "val")) numId = std::to_string(*id);
        if (const auto ilvl = IntAttr(Child(numPr, "ilvl"), "val")) {
            level = static_cast<int>(*ilvl);
        }
    }
    if (numId.empty()) {
        if (const RawStyle* style = styles_.Find(styleId); style != nullptr && style->numId) {
            numId = *style->numId;
            level = style->numLevel.value_or(0);
        }
    }
    // numId 0 means "explicitly not numbered", which is how Word removes a
    // paragraph from a list without removing the style.
    if (numId == "0") numId.clear();

    const std::optional<int> heading = styles_.HeadingLevel(styleId);

    // Word has no code block element, so a run of paragraphs sharing a code
    // style is one. Accumulated rather than emitted, because each paragraph is
    // one LINE -- which is exactly what our own writer produces.
    if (numId.empty() && styles_.IsCodeStyle(styleId)) {
        lists.Close();
        std::string line =
            PlainText({Block::Of(Paragraph{std::move(inlines), {}, {}, std::nullopt})});
        // PlainText terminates a block; a blank line inside a listing has to
        // survive, so the newline is added deliberately rather than inherited.
        while (!line.empty() && line.back() == '\n') line.pop_back();
        pendingCode_ += line;
        pendingCode_ += '\n';
        hasPendingCode_ = true;
        FlushQuote(out);
        return;
    }
    FlushCode(out);

    // An empty paragraph with a bottom border is what Word produces for a
    // horizontal rule, and it is what our own writer emits for one. Without this
    // the rule reads back as an empty paragraph and disappears: the writer had a
    // spelling for the construct and the reader had no ear for it.
    //
    // AFTER FlushCode, not before. A pending listing has not been emitted yet,
    // and a rule pushed ahead of it lands in front of the code block it followed.
    if (inlines.empty() && numId.empty() && Child(Child(pPr, "pBdr"), "bottom")) {
        lists.Close();
        FlushQuote(out);
        out.push_back(Block::Of(HorizontalRule{}));
        return;
    }

    if (numId.empty() && !inlines.empty() && styles_.IsQuoteStyle(styleId)) {
        lists.Close();
        Paragraph paragraph;
        paragraph.inlines = std::move(inlines);
        paragraph.styleRef = styleId;
        pendingQuote_.push_back(Block::Of(std::move(paragraph)));
        return;
    }
    FlushQuote(out);

    if (!numId.empty()) {
        const std::optional<LevelDefinition> definition = numbering_.Find(numId, level);
        // A numId with no definition is malformed. Treating it as a list anyway
        // would produce a bullet list from a paragraph Word renders plain.
        if (definition) {
            level = std::clamp(level, 0, 8);

            Blocks item;
            if (!inlines.empty()) {
                Paragraph paragraph;
                paragraph.inlines = std::move(inlines);
                paragraph.styleRef = styleId;
                paragraph.direct = ParseParagraphProperties(pPr);
                paragraph.list = ListRef{numId, static_cast<std::uint8_t>(level)};
                item.push_back(Block::Of(std::move(paragraph)));
            }
            lists.Add(numId, level, definition->ordered, definition->start, std::move(item),
                      std::nullopt);
            return;
        }
    }

    // Anything that is not a list item ends the current list.
    lists.Close();

    if (inlines.empty()) {
        // An empty paragraph is vertical space, not content. Word documents are
        // full of them and every one becomes a stray blank line downstream.
        return;
    }

    if (heading) {
        Heading node;
        node.level = *heading;
        node.inlines = std::move(inlines);
        node.styleRef = styleId;
        node.anchorId = MakeAnchor(node.inlines);
        out.push_back(Block::Of(std::move(node)));
        return;
    }

    Paragraph paragraph;
    paragraph.inlines = std::move(inlines);
    paragraph.styleRef = styleId;
    paragraph.direct = ParseParagraphProperties(pPr);
    out.push_back(Block::Of(std::move(paragraph)));
}

// --- Mapping 3: tables -------------------------------------------------------

void DocxBody::ReadTable(const pugi::xml_node& tbl, Blocks& out, std::size_t depth) {
    Table table;

    if (const pugi::xml_node grid = Child(tbl, "tblGrid")) {
        for (pugi::xml_node col = grid.first_child(); col; col = col.next_sibling()) {
            if (LocalName(col) != "gridCol") continue;
            TableColumn column;
            if (const auto width = IntAttr(col, "w")) column.widthPt = TwipsToPoints(*width);
            table.columns.push_back(column);
        }
    }

    // FIRST PASS: read the cells as written. A vMerge continuation cell is
    // recorded as covered but keeps its position, because the grid has to stay
    // rectangular for the second pass to work.
    struct RawCell {
        TableCell cell;
        bool mergeStart = false;
        bool mergeContinue = false;
    };
    std::vector<std::vector<RawCell>> grid;
    bool header_row_seen = false;

    for (pugi::xml_node tr = tbl.first_child(); tr; tr = tr.next_sibling()) {
        if (LocalName(tr) != "tr") continue;

        std::vector<RawCell> row;
        for (pugi::xml_node tc = tr.first_child(); tc; tc = tc.next_sibling()) {
            if (LocalName(tc) != "tc") continue;

            const pugi::xml_node tcPr = Child(tc, "tcPr");
            RawCell raw;

            // Colspan: easy, and it is a count of grid columns, not of cells.
            if (const auto span = IntAttr(Child(tcPr, "gridSpan"), "val")) {
                raw.cell.colSpan = static_cast<std::uint32_t>(std::clamp<std::int64_t>(*span, 1, 1000));
            }

            // Rowspan: a start marker plus continuation markers. An absent w:val
            // means "continue", which is the opposite of the usual OOXML
            // convention and is the detail that makes this fiddly.
            if (const pugi::xml_node vMerge = Child(tcPr, "vMerge")) {
                const std::string value = Attr(vMerge, "val");
                if (value == "restart") {
                    raw.mergeStart = true;
                } else {
                    raw.mergeContinue = true;
                }
            }

            if (const pugi::xml_node align = Child(tcPr, "jc")) {
                const std::string value = Attr(align, "val");
                if (value == "center") raw.cell.alignment = Alignment::kCenter;
                else if (value == "right" || value == "end") raw.cell.alignment = Alignment::kEnd;
                else if (value == "left" || value == "start") raw.cell.alignment = Alignment::kStart;
            }

            ListAssembler cellLists(raw.cell.blocks);
            for (pugi::xml_node child = tc.first_child(); child; child = child.next_sibling()) {
                ReadBlockLevel(child, raw.cell.blocks, cellLists, depth + 1);
            }
            FlushCode(raw.cell.blocks);
            FlushQuote(raw.cell.blocks);
            cellLists.Close();

            row.push_back(std::move(raw));
        }
        if (row.empty()) continue;

        // A header row is marked on the row itself. Only a leading run of them
        // counts: the IR's headerRows is a prefix, not a set.
        const bool is_header = OnOff(Child(tr, "trPr"), "tblHeader").value_or(false);
        if (is_header && !header_row_seen && grid.size() == table.headerRows) {
            table.headerRows = static_cast<std::uint32_t>(grid.size() + 1);
        } else if (!is_header) {
            header_row_seen = true;
        }

        grid.push_back(std::move(row));
    }

    if (grid.empty()) return;

    // SECOND PASS: resolve the vertical merges. A continuation cell belongs to
    // the nearest start above it in the same grid column, which means walking
    // columns rather than cells -- a row with a colspan does not have the same
    // number of cells as the row above it.
    const std::size_t width = std::max<std::size_t>(
        table.columns.size(),
        [&grid] {
            std::size_t widest = 0;
            for (const auto& row : grid) {
                std::size_t span = 0;
                for (const RawCell& cell : row) span += cell.cell.colSpan;
                widest = std::max(widest, span);
            }
            return widest;
        }());
    if (table.columns.size() < width) table.columns.resize(width);

    // Where each grid column's open vertical merge started, or npos.
    std::vector<std::size_t> mergeOriginRow(width, static_cast<std::size_t>(-1));
    std::vector<std::size_t> mergeOriginCell(width, 0);

    std::vector<TableRow> rows(grid.size());
    for (std::size_t r = 0; r < grid.size(); ++r) {
        std::size_t column = 0;
        for (RawCell& raw : grid[r]) {
            if (column >= width) break;

            if (raw.mergeContinue && mergeOriginRow[column] != static_cast<std::size_t>(-1)) {
                // Extend the cell that started this merge and mark this one
                // covered. The covered cell stays in the grid so that every
                // writer can index by (row, column) safely.
                const std::size_t origin_row = mergeOriginRow[column];
                const std::size_t origin_cell = mergeOriginCell[column];
                rows[origin_row].cells[origin_cell].rowSpan =
                    static_cast<std::uint32_t>(r - origin_row + 1);
                raw.cell.covered = true;
                raw.cell.blocks.clear();
            } else if (raw.mergeStart) {
                mergeOriginRow[column] = r;
                mergeOriginCell[column] = rows[r].cells.size();
            } else {
                // An ordinary cell ends any merge open in this column.
                mergeOriginRow[column] = static_cast<std::size_t>(-1);
            }

            const std::uint32_t span = raw.cell.colSpan;
            rows[r].cells.push_back(std::move(raw.cell));
            // The columns a colspan covers get their own placeholders, so the
            // row is `width` cells wide however the spans fall.
            for (std::uint32_t extra = 1; extra < span && column + extra < width; ++extra) {
                TableCell filler;
                filler.covered = true;
                rows[r].cells.push_back(std::move(filler));
            }
            column += span;
        }
        while (rows[r].cells.size() < width) rows[r].cells.push_back(TableCell{});
    }

    table.rows = std::move(rows);
    out.push_back(Block::Of(std::move(table)));
}

void DocxBody::ReadBlockLevel(const pugi::xml_node& node, Blocks& out, ListAssembler& lists,
                              std::size_t depth) {
    if (depth > kMaxBlockDepth || ++blockCount_ > kMaxBlockCount) {
        Note("nesting-truncated",
             "Content nested deeper than Morph will process was omitted. This usually means the "
             "file is malformed.");
        return;
    }

    const std::string_view name = LocalName(node);

    if (name == "p") {
        ReadParagraph(node, out, lists, depth);
        return;
    }
    if (name == "tbl") {
        FlushCode(out);
        FlushQuote(out);
        lists.Close();
        ReadTable(node, out, depth);
        return;
    }
    if (name == "sdt") {
        // Block-level content control: same reasoning as the inline case.
        Note("content-control-flattened",
             "Content controls were replaced by the text they currently contain.");
        for (pugi::xml_node child = Child(node, "sdtContent").first_child(); child;
             child = child.next_sibling()) {
            ReadBlockLevel(child, out, lists, depth + 1);
        }
        return;
    }
    if (name == "sectPr") {
        // Section properties: page size, margins, columns. P3, out of scope, and
        // nothing downstream of the IR expresses them anyway.
        return;
    }
}

void DocxBody::ReadBody(const pugi::xml_node& body) {
    ListAssembler lists(document_.blocks);
    for (pugi::xml_node child = body.first_child(); child; child = child.next_sibling()) {
        ReadBlockLevel(child, document_.blocks, lists, 0);
    }
    FlushCode(document_.blocks);
    FlushQuote(document_.blocks);
    lists.Close();
}

// --- Metadata ----------------------------------------------------------------

void ReadCoreProperties(const opc::Package& package, Metadata& meta) {
    const opc::Relationship* relationship =
        package.FindRelationshipByType("", opc::rel::kCoreProperties);
    if (relationship == nullptr) return;

    const opc::Part* part = package.Find(opc::Package::ResolveTarget("/", relationship->target));
    if (part == nullptr) return;

    pugi::xml_document xml;
    if (!xml.load_buffer(part->bytes.data(), part->bytes.size(), kOoxmlParseFlags)) return;

    const pugi::xml_node root = xml.first_child();
    for (pugi::xml_node node = root.first_child(); node; node = node.next_sibling()) {
        const std::string_view name = LocalName(node);
        const std::string value = node.text().as_string();
        if (value.empty()) continue;

        if (name == "title") meta.title = value;
        else if (name == "creator") meta.authors.push_back(value);
        else if (name == "language") meta.lang = value;
        else if (name == "created") meta.created = value;
        else if (name == "modified") meta.modified = value;
        else if (name == "subject") meta.extra.emplace("subject", value);
        else if (name == "description") meta.extra.emplace("description", value);
        else if (name == "keywords") meta.extra.emplace("keywords", value);
    }
}

}  // namespace

Result ReadDocx(std::span<const std::byte> packageBytes, const OptionBag& /*options*/,
                doc::Document& into) {
    opc::Package package;
    if (Result opened = opc::Package::Open(packageBytes, package); !opened) return opened;

    const opc::Relationship* main =
        package.FindRelationshipByType("", opc::rel::kOfficeDocument);
    if (main == nullptr) {
        return Result::Error(Status::kMalformedInput,
                             "This file is a valid package but does not contain a Word document.");
    }

    const std::string documentPart = opc::Package::ResolveTarget("/", main->target);
    const opc::Part* document = package.Find(documentPart);
    if (document == nullptr) {
        return Result::Error(Status::kMalformedInput,
                             "This document's main part is missing from the file.");
    }

    pugi::xml_document xml;
    // pugixml does not expand external entities, which closes XXE by
    // construction rather than by configuration (section 8.4).
    if (!xml.load_buffer(document->bytes.data(), document->bytes.size(), kOoxmlParseFlags)) {
        return Result::Error(Status::kMalformedInput,
                             "This document's main part is not valid XML.");
    }

    Result result = Result::Ok();
    DocxBody body(package, documentPart, into, result);

    // Styles and numbering are separate parts reached through the document's own
    // relationships, not by guessing at their names.
    pugi::xml_document stylesXml;
    if (const opc::Relationship* styles =
            package.FindRelationshipByType(documentPart, opc::rel::kStyles)) {
        if (const opc::Part* part =
                package.Find(opc::Package::ResolveTarget(documentPart, styles->target))) {
            stylesXml.load_buffer(part->bytes.data(), part->bytes.size(), kOoxmlParseFlags);
        }
    }
    pugi::xml_document numberingXml;
    if (const opc::Relationship* numbering =
            package.FindRelationshipByType(documentPart, opc::rel::kNumbering)) {
        if (const opc::Part* part =
                package.Find(opc::Package::ResolveTarget(documentPart, numbering->target))) {
            numberingXml.load_buffer(part->bytes.data(), part->bytes.size(), kOoxmlParseFlags);
        }
    }

    body.Load(stylesXml.first_child(), numberingXml.first_child());
    ReadCoreProperties(package, into.meta);

    const pugi::xml_node root = xml.first_child();
    const pugi::xml_node bodyNode = Child(root, "body");
    if (!bodyNode) {
        return Result::Error(Status::kMalformedInput, "This document has no body.");
    }
    body.ReadBody(bodyNode);

    // Every reader ends here: the IR has a canonical form and producing it is
    // the reader's job, not each writer's (document.h, Normalise).
    doc::Normalise(into.blocks);

    return result;
}

std::vector<MediaProfile> DocxReader::Reads() const {
    return {*MediaProfile::Parse(kDocxProfileId)};
}

EdgeCost DocxReader::ReadCost(const MediaProfile& /*from*/) const {
    EdgeCost cost;
    // Not Lossless, unlike the other readers: P3 and P4 are out of scope, so a
    // document with footnotes or tracked changes genuinely loses something at
    // the READER. Saying Lossless here would be the fidelity contract lying.
    cost.fidelity = Fidelity::High;
    cost.generational = 0;
    cost.cpuPerMiB = 20.0f;
    cost.memPeakFactor = 5.0f;  // the package is inflated whole, plus the IR
    cost.requiresDisk = false;
    return cost;
}

std::optional<MediaProfile> DocxReader::Probe(ISource& source, const PathHint& hint) const {
    const std::vector<std::byte> prefix = ReadAtMost(source, 8 * 1024);
    if (prefix.size() < 4) return std::nullopt;

    // Every OPC package is a ZIP, so the local file header signature is the
    // magic. "PK\x03\x04" only; an empty or spanned archive is not a document.
    if (static_cast<unsigned char>(prefix[0]) != 'P' ||
        static_cast<unsigned char>(prefix[1]) != 'K' ||
        static_cast<unsigned char>(prefix[2]) != 0x03 ||
        static_cast<unsigned char>(prefix[3]) != 0x04) {
        return std::nullopt;
    }

    // A ZIP could equally be an .xlsx, a .pptx, a .jar or a .zip. Word writes
    // "word/" as the first entry's directory, and the local header puts the name
    // at a fixed offset, so 8 KiB is more than enough to tell them apart without
    // parsing the central directory. The extension is corroboration, never the
    // decision (section 5).
    const std::string_view head(reinterpret_cast<const char*>(prefix.data()), prefix.size());
    const bool looks_like_word = head.find("word/") != std::string_view::npos ||
                                 head.find("[Content_Types].xml") != std::string_view::npos;
    if (!looks_like_word) return std::nullopt;

    if (head.find("word/") == std::string_view::npos) {
        // Only the content-type map was visible in the prefix, which every OPC
        // package has. Fall back to the extension to avoid claiming .xlsx files.
        if (hint.extension != ".docx" && hint.extension != ".docm") return std::nullopt;
    }
    return MediaProfile::Parse(kDocxProfileId);
}

Result DocxReader::Read(ConvertContext& context, ISource& source, const MediaProfile& /*from*/,
                        doc::Document& into) {
    if (context.cancelled()) return Result::Cancelled();

    // The ZIP central directory is at the end of the file and the relationship
    // graph dictates a random read order, so there is no useful streaming
    // decomposition here. The OPC limits are what keep that safe.
    constexpr std::uint64_t kMaxBytes = 512ull * 1024 * 1024;
    if (const auto size = source.Size(); size && *size > kMaxBytes) {
        return Result::Error(Status::kResourceLimit, "This document is larger than 512 MB.");
    }

    std::vector<std::byte> bytes;
    std::vector<std::byte> chunk(kStreamChunkBytes);
    while (true) {
        const std::size_t got = source.Read(chunk);
        if (got == 0) break;
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + got);
        if (bytes.size() > kMaxBytes) {
            return Result::Error(Status::kResourceLimit, "This document is larger than 512 MB.");
        }
        if (context.cancelled()) return Result::Cancelled();
    }

    context.progress.Report(0.3, 0);
    Result result = ReadDocx(bytes, context.options, into);
    context.progress.Report(1.0, 0);
    return result;
}

}  // namespace morph::docooxml
