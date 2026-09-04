// IR -> DOCX. Section 8.4, WRITING.
//
// "Emit the minimum valid package: [Content_Types].xml, _rels/.rels,
//  word/document.xml, word/styles.xml, word/_rels/document.xml.rels, plus
//  word/numbering.xml and word/media/* when needed. Write parts in deterministic
//  order with fixed ZIP timestamps so output is byte-reproducible."
//
// The interesting half is that two of the reader's tricky mappings run backwards
// here, and both are lossy in the other direction:
//
//   LISTS. The IR has a tree; WordprocessingML has no list element. Every item
//   is flattened back into a paragraph carrying w:numPr, and a numbering
//   definition has to be synthesised to give those paragraphs something to point
//   at.  ->  ListNumbering, WriteList
//
//   TABLES. The IR keeps covered placeholder cells so writers can index the grid
//   by (row, column). Word wants the covering cell to carry gridSpan and the
//   cells below a vertical merge to be present but marked. So the placeholders
//   are dropped horizontally and re-emitted vertically.  ->  WriteTable

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "docx_xml.h"
#include "morph/providers/docx.h"
#include "morph/providers/opc.h"

namespace morph::docooxml {
namespace {

using namespace morph::doc;

constexpr std::string_view kDocumentPart = "/word/document.xml";

// The style ids the writer emits and styles.xml defines. Word recognises these
// names, so a converted document opens with its outline pane populated and its
// headings in the navigation view rather than as anonymous bold text.
constexpr std::string_view kHeadingStyles[] = {"Heading1", "Heading2", "Heading3",
                                               "Heading4", "Heading5", "Heading6"};
constexpr std::string_view kQuoteStyle = "Quote";
constexpr std::string_view kCodeStyle = "SourceCode";
// A CHARACTER style, distinct from the paragraph style above: a code block and
// a code span are different things in every format, and Word will not apply a
// paragraph style to a run. The name is Word's own, so a document opened there
// looks the way a Word user expects.
constexpr std::string_view kCodeSpanStyle = "HTMLCode";
constexpr std::string_view kListStyle = "ListParagraph";
constexpr std::string_view kHyperlinkStyle = "Hyperlink";

// Run formatting as it flows down the inline tree. Strong wrapping Emphasis
// wrapping Text has to arrive at the Text node as "bold and italic".
struct WriteFormat {
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool code = false;
    bool hyperlink = false;
    TextStyle style;
};

std::string_view AlignmentValue(Alignment alignment) {
    switch (alignment) {
        case Alignment::kStart:   return "start";
        case Alignment::kCenter:  return "center";
        case Alignment::kEnd:     return "end";
        case Alignment::kJustify: return "both";
    }
    return "start";
}

// --- List numbering ----------------------------------------------------------

// Collects the numbering definitions the document needs. One abstractNum per
// distinct (ordered, start) shape at the top level, and one num per list, so
// that two sibling ordered lists restart independently rather than continuing
// each other.
class ListNumbering {
public:
    // Returns the numId for a new top-level list.
    [[nodiscard]] std::string Allocate(bool ordered, int start) {
        const int numId = nextNumId_++;
        Instance instance{numId, ordered, {}};
        instance.starts[0] = start;
        instances_.push_back(std::move(instance));
        return std::to_string(numId);
    }

    // A nested list's own starting number. WordprocessingML says it with a
    // startOverride on that level, and without it a sub-list beginning at 2
    // silently begins at 1 -- the numbering is a property of the definition, not
    // of the paragraphs, so there is nowhere else to put it.
    void SetStart(std::string_view numId, int level, int start) {
        for (Instance& instance : instances_) {
            if (std::to_string(instance.numId) != numId) continue;
            instance.starts[level] = start;
            return;
        }
    }

    [[nodiscard]] bool Empty() const noexcept { return instances_.empty(); }

    [[nodiscard]] std::string BuildXml() const {
        std::string xml =
            R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
            "\n"
            R"(<w:numbering xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">)";

        // Two abstract definitions cover everything the IR can express: a bullet
        // ladder and a decimal ladder. Nine levels each, which is Word's maximum
        // and more than the IR's depth cap allows anyone to reach.
        for (int abstractId = 0; abstractId < 2; ++abstractId) {
            const bool ordered = abstractId == 1;
            xml += R"(<w:abstractNum w:abstractNumId=")" + std::to_string(abstractId) + R"(">)";
            for (int level = 0; level < 9; ++level) {
                // Each level indents half an inch further, which is what Word's
                // own built-in list styles do.
                const int indent = 720 * (level + 1);
                xml += R"(<w:lvl w:ilvl=")" + std::to_string(level) + R"(">)";
                xml += R"(<w:start w:val="1"/>)";
                xml += ordered ? R"(<w:numFmt w:val="decimal"/>)"
                               : R"(<w:numFmt w:val="bullet"/>)";
                xml += ordered
                           ? R"(<w:lvlText w:val="%)" + std::to_string(level + 1) + R"(."/>)"
                           : R"(<w:lvlText w:val="&#8226;"/>)";
                xml += R"(<w:lvlJc w:val="start"/>)";
                xml += R"(<w:pPr><w:ind w:left=")" + std::to_string(indent) +
                       R"(" w:hanging="360"/></w:pPr>)";
                if (!ordered) {
                    xml += R"(<w:rPr><w:rFonts w:ascii="Symbol" w:hAnsi="Symbol"/></w:rPr>)";
                }
                xml += R"(</w:lvl>)";
            }
            xml += R"(</w:abstractNum>)";
        }

        for (const Instance& instance : instances_) {
            xml += R"(<w:num w:numId=")" + std::to_string(instance.numId) + R"(">)";
            xml += R"(<w:abstractNumId w:val=")" + std::string(instance.ordered ? "1" : "0") +
                   R"("/>)";
            // Every instance gets an explicit startOverride, which is what makes
            // two sibling ordered lists restart rather than continue -- the
            // reader's mapping 4, written from the other side.
            if (instance.ordered) {
                for (const auto& [level, start] : instance.starts) {
                    xml += R"(<w:lvlOverride w:ilvl=")" + std::to_string(level) +
                           R"("><w:startOverride w:val=")" + std::to_string(start) +
                           R"("/></w:lvlOverride>)";
                }
            }
            xml += R"(</w:num>)";
        }

        xml += "</w:numbering>";
        return xml;
    }

private:
    struct Instance {
        int numId;
        bool ordered;
        // Level -> first number. Level 0 is always present; deeper levels appear
        // when a nested list starts at something other than 1.
        std::map<int, int> starts;
    };
    std::vector<Instance> instances_;
    int nextNumId_ = 1;
};

// --- The writer --------------------------------------------------------------

class Writer {
public:
    Writer(const Document& document, Result& result) : document_(document), result_(result) {}

    [[nodiscard]] Result Build(std::vector<std::byte>& into);

private:
    void WriteBlocks(const Blocks& blocks, std::size_t depth);
    void WriteBlock(const Block& block, std::size_t depth);
    void WriteParagraph(const Paragraph& paragraph);
    void WriteHeading(const Heading& heading);
    void WriteCode(const CodeBlock& code);
    void WriteQuote(const BlockQuote& quote, std::size_t depth);
    void WriteList(const ListBlock& list, const std::string& numId, int level,
                   std::size_t depth);
    void WriteTable(const Table& table, std::size_t depth);
    void WriteImageParagraph(const ImageBlock& image);

    void WriteInlines(const Inlines& inlines, const WriteFormat& format);
    void WriteInline(const Inline& node, const WriteFormat& format);
    void WriteTextRun(std::string_view text, const WriteFormat& format);
    void WriteImageRun(const ResourceId& resource, const std::string& alt, float widthPt,
                       float heightPt);

    // Opens a paragraph with the given style and properties. Returns the XML for
    // the properties element so callers can extend it.
    [[nodiscard]] std::string ParagraphProperties(std::string_view styleId,
                                                  const ParagraphStyle& direct,
                                                  const std::string& extra = {}) const;
    [[nodiscard]] std::string RunProperties(const WriteFormat& format) const;

    // Registers an image part and returns the relationship id, or empty when the
    // resource is a URL we hold no bytes for.
    [[nodiscard]] std::string EmbedImage(const ResourceId& resource);
    [[nodiscard]] std::string LinkRelationship(const std::string& target);

    void Note(std::string code, std::string detail, Fidelity incurred = Fidelity::Structural) {
        if (std::find(noteCodes_.begin(), noteCodes_.end(), code) != noteCodes_.end()) return;
        noteCodes_.push_back(code);
        result_.Note(std::move(code), std::move(detail));
        result_.Incurred(incurred);
    }

    [[nodiscard]] std::string BuildStylesXml() const;

    const Document& document_;
    Result& result_;

    std::string body_;
    opc::PackageWriter package_;
    ListNumbering numbering_;

    std::map<ResourceId, std::string, std::less<>> imageRelationships_;
    std::map<std::string, std::string, std::less<>> linkRelationships_;
    std::vector<std::string> noteCodes_;
    int nextRelationshipId_ = 1;
    int nextDrawingId_ = 1;
};

std::string Writer::ParagraphProperties(std::string_view styleId, const ParagraphStyle& direct,
                                        const std::string& extra) const {
    std::string properties;
    if (!styleId.empty()) {
        properties += R"(<w:pStyle w:val=")" + std::string(styleId) + R"("/>)";
    }
    properties += extra;

    if (direct.alignment) {
        properties += R"(<w:jc w:val=")" + std::string(AlignmentValue(*direct.alignment)) +
                      R"("/>)";
    }
    if (direct.indentLeftPt || direct.indentFirstLinePt) {
        properties += "<w:ind";
        if (direct.indentLeftPt) {
            properties += R"( w:left=")" + std::to_string(PointsToTwips(*direct.indentLeftPt)) +
                          R"(")";
        }
        if (direct.indentFirstLinePt) {
            // A negative first-line indent is a hanging indent, and Word spells
            // that as a different attribute rather than as a negative number.
            const float first = *direct.indentFirstLinePt;
            properties += first < 0 ? R"( w:hanging=")" + std::to_string(PointsToTwips(-first)) +
                                          R"(")"
                                    : R"( w:firstLine=")" + std::to_string(PointsToTwips(first)) +
                                          R"(")";
        }
        properties += "/>";
    }
    if (direct.spaceBeforePt || direct.spaceAfterPt) {
        properties += "<w:spacing";
        if (direct.spaceBeforePt) {
            properties += R"( w:before=")" + std::to_string(PointsToTwips(*direct.spaceBeforePt)) +
                          R"(")";
        }
        if (direct.spaceAfterPt) {
            properties += R"( w:after=")" + std::to_string(PointsToTwips(*direct.spaceAfterPt)) +
                          R"(")";
        }
        properties += "/>";
    }

    return properties.empty() ? std::string() : "<w:pPr>" + properties + "</w:pPr>";
}

std::string Writer::RunProperties(const WriteFormat& format) const {
    std::string properties;

    // A RUN HAS ONE CHARACTER STYLE. Writing two w:rStyle elements is not
    // valid, and a reader takes the first -- which is how a code span inside a
    // link stopped being a code span: the Hyperlink style got there first and
    // the HTMLCode style was ignored.
    //
    // Code wins where they collide, because the link is already stated by the
    // w:hyperlink element around the run and only its APPEARANCE lives in the
    // style. That appearance is written out directly instead.
    if (format.code) {
        // The style AND the font. The style is what makes this readable back --
        // a monospace font is a guess, a character style is a statement -- and
        // the font is what makes it look right in Word, which resolves the style
        // only if it knows it.
        properties += R"(<w:rStyle w:val=")" + std::string(kCodeSpanStyle) + R"("/>)";
        properties += R"(<w:rFonts w:ascii="Consolas" w:hAnsi="Consolas" w:cs="Consolas"/>)";
        if (format.hyperlink) {
            properties += R"(<w:color w:val="0563C1"/><w:u w:val="single"/>)";
        }
    } else if (format.hyperlink) {
        properties += R"(<w:rStyle w:val=")" + std::string(kHyperlinkStyle) + R"("/>)";
        if (format.style.fontFamily) {
            const std::string family = XmlEscape(*format.style.fontFamily);
            properties +=
                R"(<w:rFonts w:ascii=")" + family + R"(" w:hAnsi=")" + family + R"("/>)";
        }
    } else if (format.style.fontFamily) {
        const std::string family = XmlEscape(*format.style.fontFamily);
        properties += R"(<w:rFonts w:ascii=")" + family + R"(" w:hAnsi=")" + family + R"("/>)";
    }

    if (format.bold) properties += "<w:b/>";
    if (format.italic) properties += "<w:i/>";
    if (format.strike) properties += "<w:strike/>";

    if (format.style.fontSizePt) {
        // Half-points on the way out, the same trap as on the way in.
        properties += R"(<w:sz w:val=")" +
                      std::to_string(PointsToHalfPoints(*format.style.fontSizePt)) + R"("/>)";
    }
    if (format.style.colour) {
        std::string hex = format.style.colour->ToHex();
        hex.erase(0, 1);  // OOXML writes RRGGBB with no leading hash
        if (hex.size() > 6) hex.resize(6);
        properties += R"(<w:color w:val=")" + hex + R"("/>)";
    }
    if (format.style.underline && *format.style.underline != UnderlineStyle::kNone) {
        properties += R"(<w:u w:val="single"/>)";
    }
    if (format.style.smallCaps && *format.style.smallCaps) properties += "<w:smallCaps/>";
    if (format.style.vertical == VerticalAlign::kSuperscript) {
        properties += R"(<w:vertAlign w:val="superscript"/>)";
    } else if (format.style.vertical == VerticalAlign::kSubscript) {
        properties += R"(<w:vertAlign w:val="subscript"/>)";
    }

    return properties.empty() ? std::string() : "<w:rPr>" + properties + "</w:rPr>";
}

std::string Writer::EmbedImage(const ResourceId& resource) {
    if (const auto it = imageRelationships_.find(resource); it != imageRelationships_.end()) {
        return it->second;
    }

    const Resource* found = document_.res.Find(resource);
    if (found == nullptr) {
        // The resource id is a URL carried through from Markdown or HTML. Word
        // can link to it, but section 1 is local only and a linked image that
        // silently fails to load in Word is worse than an honest note.
        Note("linked-image-dropped",
             "An image referenced by URL rather than embedded could not be included, because "
             "Morph does not download external content.");
        return {};
    }

    const std::string_view extension = ExtensionForMime(found->mime);
    // The part name is derived from the content hash, so the package stays
    // byte-reproducible and identical images share one part.
    const std::string fileName = "image" + resource + std::string(extension);
    const std::string partName = "/word/media/" + fileName;

    package_.AddPart(partName, found->mime, found->bytes);
    package_.AddDefault(std::string(extension.substr(1)), found->mime);

    const std::string id = "rId" + std::to_string(nextRelationshipId_++);
    package_.AddRelationship(std::string(kDocumentPart),
                             opc::Relationship{id, std::string(opc::rel::kImage),
                                               "media/" + fileName, false});
    imageRelationships_.emplace(resource, id);
    return id;
}

std::string Writer::LinkRelationship(const std::string& target) {
    if (const auto it = linkRelationships_.find(target); it != linkRelationships_.end()) {
        return it->second;
    }
    const std::string id = "rId" + std::to_string(nextRelationshipId_++);
    package_.AddRelationship(
        std::string(kDocumentPart),
        opc::Relationship{id, std::string(opc::rel::kHyperlink), target, /*external=*/true});
    linkRelationships_.emplace(target, id);
    return id;
}

void Writer::WriteTextRun(std::string_view text, const WriteFormat& format) {
    if (text.empty()) return;

    body_ += "<w:r>";
    body_ += RunProperties(format);
    // xml:space="preserve" unconditionally. Without it Word collapses a run that
    // is a single space, and "bold" followed by " text" loses the gap between
    // them -- a bug that only shows up where formatting changes mid-sentence.
    body_ += R"(<w:t xml:space="preserve">)";
    body_ += XmlEscape(text);
    body_ += "</w:t></w:r>";
}

void Writer::WriteImageRun(const ResourceId& resource, const std::string& alt, float widthPt,
                           float heightPt) {
    const std::string id = EmbedImage(resource);
    if (id.empty()) return;

    const Resource* found = document_.res.Find(resource);
    // Fall back to the intrinsic pixel size at 96 DPI when the IR carries no
    // display size, which is the case for everything that came from Markdown.
    float width = widthPt;
    float height = heightPt;
    if (width <= 0.0f && found != nullptr && found->widthPx > 0) {
        width = static_cast<float>(found->widthPx) * 72.0f / 96.0f;
        height = static_cast<float>(found->heightPx) * 72.0f / 96.0f;
    }
    if (width <= 0.0f) {
        width = 288.0f;  // four inches, a reasonable default for an unknown image
        height = 216.0f;
    }
    if (height <= 0.0f) height = width * 0.75f;

    const std::string cx = std::to_string(PointsToEmu(width));
    const std::string cy = std::to_string(PointsToEmu(height));
    const std::string drawingId = std::to_string(nextDrawingId_++);
    const std::string escapedAlt = XmlEscape(alt);

    body_ += "<w:r><w:drawing>";
    body_ += R"(<wp:inline distT="0" distB="0" distL="0" distR="0">)";
    body_ += R"(<wp:extent cx=")" + cx + R"(" cy=")" + cy + R"("/>)";
    body_ += R"(<wp:docPr id=")" + drawingId + R"(" name="Picture )" + drawingId +
             R"(" descr=")" + escapedAlt + R"("/>)";
    body_ += R"(<a:graphic xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main">)";
    body_ +=
        R"(<a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/picture">)";
    body_ += R"(<pic:pic xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture">)";
    body_ += R"(<pic:nvPicPr><pic:cNvPr id=")" + drawingId + R"(" name="Picture )" + drawingId +
             R"(" descr=")" + escapedAlt + R"("/><pic:cNvPicPr/></pic:nvPicPr>)";
    body_ += R"(<pic:blipFill><a:blip r:embed=")" + id +
             R"("/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>)";
    body_ += R"(<pic:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx=")" + cx + R"(" cy=")" + cy +
             R"("/></a:xfrm><a:prstGeom prst="rect"><a:avLst/></a:prstGeom></pic:spPr>)";
    body_ += "</pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r>";
}

void Writer::WriteInline(const Inline& node, const WriteFormat& format) {
    std::visit(
        Overloaded{
            [&](const Text& n) {
                WriteFormat merged = format;
                // The node's own style is direct formatting and wins; whatever
                // the enclosing nodes established fills the gaps.
                merged.style = n.style;
                merged.style.Underlay(format.style);
                WriteTextRun(n.text, merged);
            },
            [&](const Emphasis& n) {
                WriteFormat inner = format;
                inner.italic = true;
                WriteInlines(n.children, inner);
            },
            [&](const Strong& n) {
                WriteFormat inner = format;
                inner.bold = true;
                WriteInlines(n.children, inner);
            },
            [&](const Strikethrough& n) {
                WriteFormat inner = format;
                inner.strike = true;
                WriteInlines(n.children, inner);
            },
            [&](const InlineCode& n) {
                WriteFormat inner = format;
                inner.code = true;
                WriteTextRun(n.text, inner);
            },
            [&](const Link& n) {
                WriteFormat inner = format;
                inner.hyperlink = true;

                if (n.target.starts_with('#')) {
                    // An internal link to a bookmark. We do not emit bookmarks
                    // (that is heading anchors, and Word generates its own), so
                    // the link would dangle. Text is better than a dead link.
                    Note("internal-link-flattened",
                         "Links to other parts of the same document became plain text.");
                    WriteInlines(n.children, format);
                    return;
                }
                const std::string id = LinkRelationship(n.target);
                body_ += R"(<w:hyperlink r:id=")" + id + R"(")";
                if (!n.title.empty()) {
                    body_ += R"( w:tooltip=")" + XmlEscape(n.title) + R"(")";
                }
                body_ += ">";
                WriteInlines(n.children, inner);
                body_ += "</w:hyperlink>";
            },
            [&](const InlineImage& n) { WriteImageRun(n.resource, n.alt, n.widthPt, n.heightPt); },
            [&](const LineBreak& n) {
                if (n.hard) {
                    body_ += "<w:r><w:br/></w:r>";
                } else {
                    WriteTextRun(" ", format);
                }
            },
            [&](const FootnoteRef&) {
                // P3. Emitting a bare number would look like content.
                Note("footnote-dropped",
                     "Footnotes are not yet supported when writing Word documents and were "
                     "omitted.");
            },
            [&](const Math& n) {
                // No OMML writer. The TeX source in a monospace run is at least
                // recoverable; a blank would not be.
                Note("math-as-text",
                     "Mathematical expressions were written as their LaTeX source rather than as "
                     "equations.");
                WriteFormat inner = format;
                inner.code = true;
                WriteTextRun(n.tex, inner);
            },
            [&](const RawInline& n) {
                if (n.format == "ooxml") {
                    // Rule 3's promise, kept: this came out of a DOCX and goes
                    // back in byte-exact.
                    body_ += n.content;
                    return;
                }
                Note("raw-inline-dropped",
                     "Inline " + n.format + " content could not be represented and was omitted.");
            },
        },
        node.node);
}

void Writer::WriteInlines(const Inlines& inlines, const WriteFormat& format) {
    for (const Inline& node : inlines) WriteInline(node, format);
}

void Writer::WriteParagraph(const Paragraph& paragraph) {
    body_ += "<w:p>";
    body_ += ParagraphProperties(paragraph.styleRef, paragraph.direct);
    WriteInlines(paragraph.inlines, WriteFormat{});
    body_ += "</w:p>";
}

void Writer::WriteHeading(const Heading& heading) {
    const int level = std::clamp(heading.level, 1, 6);
    body_ += "<w:p>";
    body_ += ParagraphProperties(kHeadingStyles[level - 1], ParagraphStyle{});
    WriteInlines(heading.inlines, WriteFormat{});
    body_ += "</w:p>";
}

void Writer::WriteCode(const CodeBlock& code) {
    if (!code.language.empty()) {
        // A Word document has nowhere to record which language a listing is in.
        // Unavoidable, and therefore exactly what the fidelity contract is for:
        // the loss is named rather than left for the user to notice when the
        // syntax highlighting does not come back.
        Note("code-language-dropped",
             "Word documents cannot record the programming language of a code block, so "
             "syntax highlighting will not survive this conversion.");
    }

    // One paragraph per line. A single paragraph with w:br would also render,
    // but Word treats the whole thing as one block for spacing and page breaks,
    // and a fifty-line listing then refuses to split across pages.
    std::size_t at = 0;
    bool wrote_any = false;
    while (at <= code.text.size()) {
        const std::size_t end = std::min(code.text.find('\n', at), code.text.size());
        std::string_view line(code.text.data() + at, end - at);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        at = end + 1;

        // A trailing newline should not produce a final empty paragraph.
        if (line.empty() && at > code.text.size() && wrote_any) break;

        WriteFormat format;
        format.code = true;
        body_ += "<w:p>";
        body_ += ParagraphProperties(kCodeStyle, ParagraphStyle{});
        WriteTextRun(line, format);
        body_ += "</w:p>";
        wrote_any = true;
    }
}

void Writer::WriteQuote(const BlockQuote& quote, std::size_t depth) {
    // Word has no block-quote container either; the style plus an indent is what
    // a quote is in a .docx.
    for (const Block& block : quote.blocks) {
        if (const auto* paragraph = block.As<Paragraph>()) {
            Paragraph styled = *paragraph;
            styled.styleRef = std::string(kQuoteStyle);
            WriteParagraph(styled);
            continue;
        }
        WriteBlock(block, depth + 1);
    }
}

void Writer::WriteList(const ListBlock& list, const std::string& numId, int level,
                       std::size_t depth) {
    if (level > 8) {
        Note("list-depth-truncated",
             "Lists nested more than nine levels deep were flattened; Word supports nine.");
        return;
    }

    for (const ListItem& item : list.items) {
        // THE MARKER COMES FIRST, even when there is nothing to put on it. An
        // item whose only content is a nested list still needs a numbered
        // paragraph of its own, and it needs it BEFORE the nested list's
        // paragraphs: emitted afterwards, the nested list attaches to the
        // PREVIOUS item on the way back in and the empty paragraph reads as a
        // trailing empty item. The flat encoding has no other way to say "this
        // item exists".
        const bool hasOwnParagraph =
            !item.blocks.empty() && item.blocks.front().Is<Paragraph>();
        if (!hasOwnParagraph) {
            body_ += "<w:p>";
            body_ += ParagraphProperties(kListStyle, ParagraphStyle{},
                                         R"(<w:numPr><w:ilvl w:val=")" + std::to_string(level) +
                                             R"("/><w:numId w:val=")" + numId +
                                             R"("/></w:numPr>)");
            body_ += "</w:p>";
        }

        bool first = hasOwnParagraph;
        for (const Block& block : item.blocks) {
            if (const auto* nested = block.As<ListBlock>()) {
                // A nested list continues in the same numId at the next level,
                // which is exactly the flat encoding the reader reconstructs.
                //
                // KNOWN LIMITATION, found by the round-trip property test and
                // left deliberately: a nested list of a DIFFERENT kind from its
                // parent -- a numbered list inside a bulleted one -- inherits
                // the parent's markers, because both abstract definitions above
                // use one format for all nine levels. Fixing it means an
                // abstractNum per level-format ladder; giving the nested list
                // its own numId does not work, since a change of numId is how
                // WordprocessingML says "a different list" and the reader would
                // un-nest it (mapping 2 in docx_reader.cpp).
                if (nested->ordered && nested->start != 1) {
                    // Recorded on the numbering definition, which is the only
                    // place WordprocessingML will take it. That makes it a
                    // property of the whole numId rather than of this list, so
                    // two sub-lists at the same level under one parent cannot
                    // start at different numbers -- the second one inherits the
                    // first one's override. Restarting properly needs a fresh
                    // numId, and a change of numId is how the format says "a
                    // different list", which would un-nest it (mapping 2 in
                    // docx_reader.cpp). Known limitation, found by the round-trip
                    // property test.
                    numbering_.SetStart(numId, level + 1, nested->start);
                }
                WriteList(*nested, numId, level + 1, depth + 1);
                continue;
            }

            if (const auto* paragraph = block.As<Paragraph>()) {
                const std::string numbering =
                    R"(<w:numPr><w:ilvl w:val=")" + std::to_string(level) +
                    R"("/><w:numId w:val=")" + numId + R"("/></w:numPr>)";

                body_ += "<w:p>";
                // Only the FIRST paragraph of an item carries the numbering; a
                // second one would render as another bullet.
                body_ += ParagraphProperties(kListStyle, paragraph->direct,
                                             first ? numbering : std::string());
                if (item.checked.has_value() && first) {
                    // Word has no task-list construct. A ballot box character is
                    // what Word itself inserts for a checkbox in plain text.
                    WriteTextRun(*item.checked ? "\xE2\x98\x91 " : "\xE2\x98\x90 ",
                                 WriteFormat{});
                }
                WriteInlines(paragraph->inlines, WriteFormat{});
                body_ += "</w:p>";
                first = false;
                continue;
            }

            WriteBlock(block, depth + 1);
            first = false;
        }
    }
}

void Writer::WriteTable(const Table& table, std::size_t depth) {
    body_ += "<w:tbl>";
    body_ +=
        R"(<w:tblPr><w:tblW w:w="0" w:type="auto"/><w:tblBorders>)"
        R"(<w:top w:val="single" w:sz="4" w:color="auto"/>)"
        R"(<w:start w:val="single" w:sz="4" w:color="auto"/>)"
        R"(<w:bottom w:val="single" w:sz="4" w:color="auto"/>)"
        R"(<w:end w:val="single" w:sz="4" w:color="auto"/>)"
        R"(<w:insideH w:val="single" w:sz="4" w:color="auto"/>)"
        R"(<w:insideV w:val="single" w:sz="4" w:color="auto"/>)"
        R"(</w:tblBorders></w:tblPr>)";

    const std::size_t width =
        table.columns.empty()
            ? (table.rows.empty() ? 0 : table.rows.front().cells.size())
            : table.columns.size();

    body_ += "<w:tblGrid>";
    for (std::size_t c = 0; c < width; ++c) {
        const float points =
            (c < table.columns.size() && table.columns[c].widthPt > 0.0f)
                ? table.columns[c].widthPt
                : 468.0f / static_cast<float>(std::max<std::size_t>(width, 1));
        body_ += R"(<w:gridCol w:w=")" + std::to_string(PointsToTwips(points)) + R"("/>)";
    }
    body_ += "</w:tblGrid>";

    for (std::size_t r = 0; r < table.rows.size(); ++r) {
        const TableRow& row = table.rows[r];
        body_ += "<w:tr>";
        if (r < table.headerRows) {
            body_ += R"(<w:trPr><w:tblHeader/></w:trPr>)";
        }

        // A HORIZONTALLY covered cell is grid bookkeeping the IR keeps so that
        // writers can index by (row, column); Word expresses the same thing with
        // gridSpan on the covering cell, so those are skipped. A VERTICALLY
        // covered cell has no earlier cell spanning over it and must still be
        // emitted, carrying a bare w:vMerge.
        std::size_t covered_by_span = 0;

        for (std::size_t c = 0; c < row.cells.size(); ++c) {
            if (covered_by_span > 0) {
                --covered_by_span;
                continue;
            }
            const TableCell& cell = row.cells[c];
            covered_by_span = cell.colSpan > 1 ? cell.colSpan - 1 : 0;

            body_ += "<w:tc><w:tcPr>";
            if (cell.colSpan > 1) {
                body_ += R"(<w:gridSpan w:val=")" + std::to_string(cell.colSpan) + R"("/>)";
            }
            if (cell.rowSpan > 1) {
                body_ += R"(<w:vMerge w:val="restart"/>)";
            } else if (cell.covered) {
                // Reached only for a vertical continuation, which is exactly
                // where Word wants a bare w:vMerge.
                body_ += "<w:vMerge/>";
            }
            if (cell.alignment) {
                body_ += R"(<w:jc w:val=")" + std::string(AlignmentValue(*cell.alignment)) +
                         R"("/>)";
            }
            body_ += "</w:tcPr>";

            if (cell.blocks.empty()) {
                // A table cell must contain at least one paragraph or Word
                // reports the document as corrupt.
                body_ += "<w:p/>";
            } else {
                WriteBlocks(cell.blocks, depth + 1);
            }
            body_ += "</w:tc>";
        }
        body_ += "</w:tr>";
    }
    body_ += "</w:tbl>";
    // Word requires a paragraph after a table, or a table at the end of the body
    // makes the document unopenable.
    body_ += "<w:p/>";
}

void Writer::WriteImageParagraph(const ImageBlock& image) {
    body_ += "<w:p>";
    WriteImageRun(image.resource, image.alt, image.widthPt, image.heightPt);
    body_ += "</w:p>";
}

void Writer::WriteBlock(const Block& block, std::size_t depth) {
    std::visit(
        Overloaded{
            [&](const Paragraph& b) { WriteParagraph(b); },
            [&](const Heading& b) { WriteHeading(b); },
            [&](const CodeBlock& b) { WriteCode(b); },
            [&](const BlockQuote& b) { WriteQuote(b, depth); },
            [&](const ListBlock& b) {
                const std::string numId = numbering_.Allocate(b.ordered, b.start);
                WriteList(b, numId, 0, depth);
            },
            [&](const Table& b) { WriteTable(b, depth); },
            [&](const ImageBlock& b) { WriteImageParagraph(b); },
            [&](const HorizontalRule&) {
                // Word has no horizontal rule element. An empty paragraph with a
                // bottom border is what Word itself produces for one.
                body_ +=
                    R"(<w:p><w:pPr><w:pBdr><w:bottom w:val="single" w:sz="6" w:space="1")"
                    R"( w:color="auto"/></w:pBdr></w:pPr></w:p>)";
            },
            [&](const Footnote&) {
                Note("footnote-dropped",
                     "Footnotes are not yet supported when writing Word documents and were "
                     "omitted.");
            },
            [&](const RawBlock& b) {
                if (b.format == "ooxml") {
                    body_ += std::string(reinterpret_cast<const char*>(b.bytes.data()),
                                         b.bytes.size());
                    return;
                }
                Note("raw-block-dropped",
                     "A " + (b.description.empty() ? b.format : b.description) +
                         " could not be represented in a Word document and was omitted.");
            },
        },
        block.node);
}

void Writer::WriteBlocks(const Blocks& blocks, std::size_t depth) {
    if (depth > kMaxBlockDepth) {
        Note("nesting-truncated", "Content nested deeper than 100 levels was omitted.");
        return;
    }
    for (const Block& block : blocks) WriteBlock(block, depth);
}

std::string Writer::BuildStylesXml() const {
    std::string xml =
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        "\n"
        R"(<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">)";

    xml +=
        R"(<w:docDefaults><w:rPrDefault><w:rPr>)"
        R"(<w:rFonts w:ascii="Calibri" w:hAnsi="Calibri" w:cs="Calibri"/>)"
        R"(<w:sz w:val="22"/><w:szCs w:val="22"/>)"
        R"(</w:rPr></w:rPrDefault>)"
        R"(<w:pPrDefault><w:pPr><w:spacing w:after="160" w:line="259" w:lineRule="auto"/>)"
        R"(</w:pPr></w:pPrDefault></w:docDefaults>)";

    xml +=
        R"(<w:style w:type="paragraph" w:default="1" w:styleId="Normal">)"
        R"(<w:name w:val="Normal"/><w:qFormat/></w:style>)";

    // The outline level is what puts a heading in Word's navigation pane, and it
    // is also the signal our own reader trusts first.
    for (int level = 1; level <= 6; ++level) {
        const int size = 32 - (level - 1) * 3;  // half-points: 16pt down to 8.5pt
        xml += R"(<w:style w:type="paragraph" w:styleId=")" +
               std::string(kHeadingStyles[level - 1]) + R"(">)";
        xml += R"(<w:name w:val="heading )" + std::to_string(level) + R"("/>)";
        xml += R"(<w:basedOn w:val="Normal"/><w:qFormat/>)";
        xml += R"(<w:pPr><w:keepNext/><w:spacing w:before="240" w:after="80"/>)";
        xml += R"(<w:outlineLvl w:val=")" + std::to_string(level - 1) + R"("/></w:pPr>)";
        xml += R"(<w:rPr><w:b/><w:sz w:val=")" + std::to_string(size) + R"("/></w:rPr>)";
        xml += R"(</w:style>)";
    }

    xml +=
        R"(<w:style w:type="paragraph" w:styleId=")" + std::string(kQuoteStyle) + R"(">)"
        R"(<w:name w:val="Quote"/><w:basedOn w:val="Normal"/><w:qFormat/>)"
        R"(<w:pPr><w:ind w:left="720"/></w:pPr>)"
        R"(<w:rPr><w:i/><w:color w:val="404040"/></w:rPr></w:style>)";

    xml +=
        R"(<w:style w:type="paragraph" w:styleId=")" + std::string(kCodeStyle) + R"(">)"
        R"(<w:name w:val="Source Code"/><w:basedOn w:val="Normal"/>)"
        R"(<w:pPr><w:spacing w:after="0" w:line="240" w:lineRule="auto"/>)"
        R"(<w:ind w:left="360"/></w:pPr>)"
        R"(<w:rPr><w:rFonts w:ascii="Consolas" w:hAnsi="Consolas"/><w:sz w:val="20"/>)"
        R"(</w:rPr></w:style>)";

    xml +=
        R"(<w:style w:type="paragraph" w:styleId=")" + std::string(kListStyle) + R"(">)"
        R"(<w:name w:val="List Paragraph"/><w:basedOn w:val="Normal"/><w:qFormat/>)"
        R"(<w:pPr><w:spacing w:after="0"/><w:contextualSpacing/></w:pPr></w:style>)";

    xml +=
        R"(<w:style w:type="character" w:styleId=")" + std::string(kHyperlinkStyle) + R"(">)"
        R"(<w:name w:val="Hyperlink"/>)"
        R"(<w:rPr><w:color w:val="0563C1"/><w:u w:val="single"/></w:rPr></w:style>)";

    xml +=
        R"(<w:style w:type="character" w:styleId=")" + std::string(kCodeSpanStyle) + R"(">)"
        R"(<w:name w:val="HTML Code"/>)"
        R"(<w:rPr><w:rFonts w:ascii="Consolas" w:hAnsi="Consolas" w:cs="Consolas"/>)"
        R"(</w:rPr></w:style>)";

    xml += "</w:styles>";
    return xml;
}

Result Writer::Build(std::vector<std::byte>& into) {
    WriteBlocks(document_.blocks, 0);

    // A body must end with sectPr or Word complains about the document layout.
    // A4 with one-inch margins, matching the HTML writer's page geometry so that
    // md -> docx and md -> pdf do not disagree about the page.
    body_ +=
        R"(<w:sectPr><w:pgSz w:w="11906" w:h="16838"/>)"
        R"(<w:pgMar w:top="1440" w:right="1440" w:bottom="1440" w:left="1440")"
        R"( w:header="720" w:footer="720" w:gutter="0"/></w:sectPr>)";

    package_.AddDefault("rels", std::string(opc::mime::kRelationships));
    package_.AddDefault("xml", "application/xml");

    const std::string document =
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        "\n"
        R"(<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main")"
        R"( xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships")"
        R"( xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing">)"
        "<w:body>" +
        body_ + "</w:body></w:document>";

    package_.AddPart(std::string(kDocumentPart), std::string(opc::mime::kMainDocument), document);
    package_.AddRelationship("", opc::Relationship{"rIdDocument",
                                                   std::string(opc::rel::kOfficeDocument),
                                                   "word/document.xml", false});

    package_.AddPart("/word/styles.xml", std::string(opc::mime::kStyles), BuildStylesXml());
    package_.AddRelationship(std::string(kDocumentPart),
                             opc::Relationship{"rIdStyles", std::string(opc::rel::kStyles),
                                               "styles.xml", false});

    if (!numbering_.Empty()) {
        package_.AddPart("/word/numbering.xml", std::string(opc::mime::kNumbering),
                         numbering_.BuildXml());
        package_.AddRelationship(std::string(kDocumentPart),
                                 opc::Relationship{"rIdNumbering",
                                                   std::string(opc::rel::kNumbering),
                                                   "numbering.xml", false});
    }

    const Metadata& meta = document_.meta;
    if (!meta.title.empty() || !meta.authors.empty() || !meta.lang.empty()) {
        std::string core =
            R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
            "\n"
            R"(<cp:coreProperties)"
            R"( xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties")"
            R"( xmlns:dc="http://purl.org/dc/elements/1.1/")"
            R"( xmlns:dcterms="http://purl.org/dc/terms/")"
            R"( xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">)";
        if (!meta.title.empty()) core += "<dc:title>" + XmlEscape(meta.title) + "</dc:title>";
        if (!meta.authors.empty()) {
            core += "<dc:creator>" + XmlEscape(meta.authors.front()) + "</dc:creator>";
        }
        if (!meta.lang.empty()) core += "<dc:language>" + XmlEscape(meta.lang) + "</dc:language>";
        // No dcterms:created: a timestamp would break byte reproducibility, and
        // the creation date of a conversion is not a meaningful fact anyway.
        core += "</cp:coreProperties>";

        package_.AddPart("/docProps/core.xml", std::string(opc::mime::kCoreProperties), core);
        package_.AddRelationship("", opc::Relationship{"rIdCore",
                                                       std::string(opc::rel::kCoreProperties),
                                                       "docProps/core.xml", false});
    }

    return package_.Finish(into);
}

}  // namespace

Result WriteDocx(const doc::Document& document, const OptionBag& /*options*/,
                 std::vector<std::byte>& into) {
    Result result = Result::Ok();
    Writer writer(document, result);
    if (Result built = writer.Build(into); !built) return built;
    return result;
}

std::vector<MediaProfile> DocxWriter::Writes() const {
    return {*MediaProfile::Parse(kDocxProfileId)};
}

EdgeCost DocxWriter::WriteCost(const MediaProfile& /*to*/) const {
    EdgeCost cost;
    // Lossless from the IR's point of view for everything P1 and P2 covers.
    // Section 8.8's LOSSLESS is "round-trips byte-identical or semantically
    // identical", and md -> docx is semantically identical.
    cost.fidelity = Fidelity::Lossless;
    cost.generational = 0;
    cost.cpuPerMiB = 18.0f;
    cost.memPeakFactor = 4.0f;
    cost.requiresDisk = false;
    return cost;
}

Result DocxWriter::Write(ConvertContext& context, const doc::Document& document,
                         const MediaProfile& /*to*/, ISink& sink) {
    if (context.cancelled()) return Result::Cancelled();

    std::vector<std::byte> bytes;
    Result result = WriteDocx(document, context.options, bytes);
    if (!result) return result;
    if (context.cancelled()) return Result::Cancelled();

    WriteAll(sink, bytes);
    sink.Commit();
    context.progress.Report(1.0, bytes.size());
    return result;
}

}  // namespace morph::docooxml
