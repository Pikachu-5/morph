// IR -> HTML. Section 8.5.
//
// Semantic HTML5 plus a small embedded theme. Two constraints shape everything
// here, and both come from the fact that this output is also the PDF stage's
// input (section 8.6):
//
//   DETERMINISTIC. The same IR must produce the same bytes. Golden-file testing
//   depends on it (section 16) and so does any hope of diffing two conversions.
//
//   SELF-CONTAINED. No network references of any kind. Images are inlined as
//   data URIs and the theme uses system font stacks. The WebView2 host blocks
//   non-data:/file: navigation anyway (section 15.1), so an external reference
//   would not merely be slow, it would silently not load.

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "morph/providers/html.h"

namespace morph::dochtml {
namespace {

using namespace morph::doc;

constexpr std::string_view kStylesheet = R"CSS(
:root {
  --ink: #1a1a1a;
  --muted: #5b5b5b;
  --rule: #d8d8d8;
  --accent: #2f5d8a;
  --code-bg: #f5f5f4;
}
html { -webkit-text-size-adjust: 100%; }
body {
  /* System stacks only. A webfont would need the network, which the PDF host
     blocks, and the fallback would silently change the page geometry. */
  font-family: "Segoe UI", -apple-system, "Helvetica Neue", Arial, sans-serif;
  font-size: 11pt;
  line-height: 1.55;
  color: var(--ink);
  margin: 0;
  padding: 0;
  hyphens: auto;
}
h1, h2, h3, h4, h5, h6 {
  line-height: 1.25;
  margin: 1.6em 0 0.5em;
  font-weight: 600;
  /* Keeps a heading from being orphaned at the foot of a printed page. */
  break-after: avoid-page;
  page-break-after: avoid;
}
h1 { font-size: 2em; margin-top: 0; }
h2 { font-size: 1.5em; }
h3 { font-size: 1.22em; }
h4 { font-size: 1.05em; }
h5, h6 { font-size: 1em; color: var(--muted); }
p { margin: 0 0 0.85em; orphans: 2; widows: 2; }
a { color: var(--accent); }
strong { font-weight: 600; }
blockquote {
  margin: 1em 0;
  padding: 0.1em 0 0.1em 1.1em;
  border-left: 3px solid var(--rule);
  color: var(--muted);
}
ul, ol { margin: 0 0 0.85em; padding-left: 1.6em; }
li { margin: 0.2em 0; }
li > input[type="checkbox"] { margin-right: 0.4em; }
code, pre {
  font-family: "Cascadia Mono", Consolas, "SF Mono", "Liberation Mono", monospace;
  font-size: 0.9em;
}
code { background: var(--code-bg); padding: 0.1em 0.3em; border-radius: 3px; }
pre {
  background: var(--code-bg);
  padding: 0.8em 1em;
  border-radius: 4px;
  overflow-x: auto;
  /* Long lines must wrap in a PDF; there is nowhere to scroll to. */
  white-space: pre-wrap;
  word-wrap: break-word;
  break-inside: avoid-page;
}
pre code { background: none; padding: 0; }
hr { border: 0; border-top: 1px solid var(--rule); margin: 2em 0; }
img { max-width: 100%; height: auto; }
figure { margin: 1.2em 0; }
figcaption { font-size: 0.9em; color: var(--muted); margin-top: 0.4em; }
table {
  border-collapse: collapse;
  width: 100%;
  margin: 1.2em 0;
  font-size: 0.95em;
  break-inside: avoid-page;
}
th, td {
  border: 1px solid var(--rule);
  padding: 0.4em 0.6em;
  text-align: left;
  vertical-align: top;
}
thead th { background: var(--code-bg); font-weight: 600; }
caption { caption-side: bottom; font-size: 0.9em; color: var(--muted); padding-top: 0.5em; }
sup.footnote-ref { font-size: 0.75em; vertical-align: super; line-height: 0; }
.footnotes { margin-top: 2.5em; padding-top: 1em; border-top: 1px solid var(--rule);
             font-size: 0.9em; color: var(--muted); }
@media print {
  body { font-size: 10.5pt; }
  a { color: var(--ink); text-decoration: none; }
}
)CSS";

constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64(std::span<const std::byte> bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);

    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                     (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
                                     static_cast<std::uint32_t>(bytes[i + 2]);
        out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
        out.push_back(kBase64Alphabet[(triple >> 6) & 0x3F]);
        out.push_back(kBase64Alphabet[triple & 0x3F]);
    }

    if (i < bytes.size()) {
        const bool two = (bytes.size() - i) == 2;
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(bytes[i]) << 16) |
            (two ? (static_cast<std::uint32_t>(bytes[i + 1]) << 8) : 0u);
        out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
        out.push_back(two ? kBase64Alphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

std::string_view AlignmentName(Alignment alignment) {
    switch (alignment) {
        case Alignment::kStart:   return "left";
        case Alignment::kCenter:  return "center";
        case Alignment::kEnd:     return "right";
        case Alignment::kJustify: return "justify";
    }
    return "left";
}

class Writer {
public:
    Writer(const Document& document, const OptionBag& options, Result& result)
        : document_(document), options_(options), result_(result) {}

    void WriteDocument();
    [[nodiscard]] std::string Take() { return std::move(out_); }

private:
    void WriteBlocks(const Blocks& blocks, std::size_t depth);
    void WriteBlock(const Block& block, std::size_t depth);
    void WriteInlines(const Inlines& inlines);
    void WriteInline(const Inline& node);
    void WriteTable(const Table& table, std::size_t depth);
    void WriteImage(const ResourceId& resource, const std::string& alt, const std::string& title,
                    float widthPt, float heightPt);
    void WriteStyleAttribute(const TextStyle& style);

    void Emit(std::string_view text) { out_.append(text); }
    void Line(std::string_view text) {
        out_.append(text);
        out_.push_back('\n');
    }

    void Note(std::string code, std::string detail, Fidelity incurred) {
        result_.Note(std::move(code), std::move(detail));
        result_.Incurred(incurred);
    }

    const Document& document_;
    const OptionBag& options_;
    Result& result_;
    std::string out_;
    std::vector<const Footnote*> footnotes_;
    std::vector<std::string> footnoteIds_;
};

void Writer::WriteStyleAttribute(const TextStyle& style) {
    if (style.Empty()) return;

    std::string css;
    const auto add = [&css](std::string_view property, const std::string& value) {
        if (!css.empty()) css += "; ";
        css += property;
        css += ": ";
        css += value;
    };

    if (style.colour) add("color", style.colour->ToHex());
    if (style.highlight) add("background-color", style.highlight->ToHex());
    if (style.fontFamily) add("font-family", EscapeHtmlAttribute(*style.fontFamily));
    if (style.fontSizePt) {
        std::string size = std::to_string(*style.fontSizePt);
        // Trim the float noise; "14.000000pt" in every span is unreadable and
        // makes golden files churn on formatting changes.
        while (size.size() > 1 && size.back() == '0') size.pop_back();
        if (!size.empty() && size.back() == '.') size.pop_back();
        add("font-size", size + "pt");
    }
    if (style.underline && *style.underline != UnderlineStyle::kNone) {
        add("text-decoration", "underline");
    }
    if (style.smallCaps && *style.smallCaps) add("font-variant", "small-caps");
    if (style.vertical == VerticalAlign::kSuperscript) add("vertical-align", "super");
    if (style.vertical == VerticalAlign::kSubscript) add("vertical-align", "sub");
    if (style.letterSpacingPt) {
        add("letter-spacing", std::to_string(*style.letterSpacingPt) + "pt");
    }

    if (!css.empty()) {
        Emit(" style=\"");
        Emit(css);
        Emit("\"");
    }
}

void Writer::WriteImage(const ResourceId& resource, const std::string& alt,
                        const std::string& title, float widthPt, float heightPt) {
    Emit("<img src=\"");

    if (const Resource* found = document_.res.Find(resource)) {
        // Inlined, not referenced. Self-contained is the contract.
        Emit("data:");
        Emit(found->mime.empty() ? "application/octet-stream" : found->mime);
        Emit(";base64,");
        Emit(Base64(found->bytes));
    } else {
        // No bytes in the store: the resource id is a URL carried through from a
        // Markdown or HTML source. Emit it as written and do not fetch it --
        // section 1 is local only, with no network egress at all.
        Emit(EscapeHtmlAttribute(resource));
    }

    Emit("\" alt=\"");
    Emit(EscapeHtmlAttribute(alt));
    Emit("\"");
    if (!title.empty()) {
        Emit(" title=\"");
        Emit(EscapeHtmlAttribute(title));
        Emit("\"");
    }
    if (widthPt > 0.0f) {
        Emit(" style=\"width:");
        Emit(std::to_string(static_cast<int>(widthPt)));
        Emit("pt");
        if (heightPt > 0.0f) {
            Emit(";height:");
            Emit(std::to_string(static_cast<int>(heightPt)));
            Emit("pt");
        }
        Emit("\"");
    }
    Emit(">");
}

void Writer::WriteInline(const Inline& node) {
    std::visit(Overloaded{
                   [&](const Text& n) {
                       if (n.style.Empty()) {
                           Emit(EscapeHtmlText(n.text));
                           return;
                       }
                       Emit("<span");
                       WriteStyleAttribute(n.style);
                       Emit(">");
                       Emit(EscapeHtmlText(n.text));
                       Emit("</span>");
                   },
                   [&](const Emphasis& n) {
                       Emit("<em>");
                       WriteInlines(n.children);
                       Emit("</em>");
                   },
                   [&](const Strong& n) {
                       Emit("<strong>");
                       WriteInlines(n.children);
                       Emit("</strong>");
                   },
                   [&](const Strikethrough& n) {
                       Emit("<del>");
                       WriteInlines(n.children);
                       Emit("</del>");
                   },
                   [&](const InlineCode& n) {
                       Emit("<code>");
                       Emit(EscapeHtmlText(n.text));
                       Emit("</code>");
                   },
                   [&](const Link& n) {
                       Emit("<a href=\"");
                       Emit(EscapeHtmlAttribute(n.target));
                       Emit("\"");
                       if (!n.title.empty()) {
                           Emit(" title=\"");
                           Emit(EscapeHtmlAttribute(n.title));
                           Emit("\"");
                       }
                       Emit(">");
                       WriteInlines(n.children);
                       Emit("</a>");
                   },
                   [&](const InlineImage& n) {
                       WriteImage(n.resource, n.alt, n.title, n.widthPt, n.heightPt);
                   },
                   [&](const LineBreak& n) { Emit(n.hard ? "<br>" : "\n"); },
                   [&](const FootnoteRef& n) {
                       std::size_t index = 0;
                       const auto it =
                           std::find(footnoteIds_.begin(), footnoteIds_.end(), n.id);
                       if (it != footnoteIds_.end()) {
                           index = static_cast<std::size_t>(
                               std::distance(footnoteIds_.begin(), it));
                       } else {
                           index = footnoteIds_.size();
                           footnoteIds_.push_back(n.id);
                           const Footnote* body = nullptr;
                           ForEachBlock(document_.blocks, [&](const Block& block) {
                               if (const auto* f = block.As<Footnote>();
                                   f != nullptr && f->id == n.id) {
                                   body = f;
                               }
                           });
                           footnotes_.push_back(body);
                       }
                       const std::string number = std::to_string(index + 1);
                       Emit("<sup class=\"footnote-ref\"><a href=\"#fn" + number + "\" id=\"fnref" +
                            number + "\">" + number + "</a></sup>");
                   },
                   [&](const Math& n) {
                       // No MathML and no renderer: emitting the TeX verbatim in
                       // a marked-up element is honest and lets a downstream tool
                       // find it. Claiming to render maths we cannot render would
                       // be worse.
                       Emit(n.display ? "<div class=\"math display\">" : "<span class=\"math\">");
                       Emit(EscapeHtmlText(n.tex));
                       Emit(n.display ? "</div>" : "</span>");
                   },
                   [&](const RawInline& n) {
                       if (n.format == "html") {
                           Emit(n.content);
                           return;
                       }
                       Note("raw-inline-dropped", "Inline " + n.format + " content was omitted.",
                            Fidelity::Structural);
                   },
               },
               node.node);
}

void Writer::WriteInlines(const Inlines& inlines) {
    for (const Inline& node : inlines) WriteInline(node);
}

void Writer::WriteTable(const Table& table, std::size_t depth) {
    Emit("<table>\n");

    if (!table.caption.empty()) {
        Emit("<caption>");
        WriteInlines(table.caption);
        Line("</caption>");
    }

    if (!table.columns.empty() &&
        std::any_of(table.columns.begin(), table.columns.end(),
                    [](const TableColumn& c) { return c.widthPt > 0.0f; })) {
        Emit("<colgroup>\n");
        for (const TableColumn& column : table.columns) {
            Emit("<col");
            if (column.widthPt > 0.0f) {
                Emit(" style=\"width:" + std::to_string(static_cast<int>(column.widthPt)) + "pt\"");
            }
            Line(">");
        }
        Emit("</colgroup>\n");
    }

    const auto write_row = [&](const TableRow& row, bool header) {
        Emit("<tr>\n");
        for (std::size_t c = 0; c < row.cells.size(); ++c) {
            const TableCell& cell = row.cells[c];
            // A covered cell is a placeholder for the grid, not an element. It
            // exists so writers can index by (row, column) safely; HTML expresses
            // the same thing with colspan/rowspan on the covering cell.
            if (cell.covered) continue;

            const std::string_view tag = header ? "th" : "td";
            Emit("<");
            Emit(tag);
            if (cell.colSpan > 1) Emit(" colspan=\"" + std::to_string(cell.colSpan) + "\"");
            if (cell.rowSpan > 1) Emit(" rowspan=\"" + std::to_string(cell.rowSpan) + "\"");

            std::optional<Alignment> align = cell.alignment;
            if (!align && c < table.columns.size()) align = table.columns[c].alignment;
            if (align) Emit(" style=\"text-align:" + std::string(AlignmentName(*align)) + "\"");
            Emit(">");

            // A cell holding one paragraph writes its inlines directly: <td><p>
            // adds a margin that makes every table look wrong.
            if (cell.blocks.size() == 1) {
                if (const auto* paragraph = cell.blocks.front().As<Paragraph>()) {
                    WriteInlines(paragraph->inlines);
                    Emit("</");
                    Emit(tag);
                    Line(">");
                    continue;
                }
            }
            WriteBlocks(cell.blocks, depth + 1);
            Emit("</");
            Emit(tag);
            Line(">");
        }
        Emit("</tr>\n");
    };

    if (table.headerRows > 0) {
        Emit("<thead>\n");
        for (std::uint32_t r = 0; r < table.headerRows && r < table.rows.size(); ++r) {
            write_row(table.rows[r], /*header=*/true);
        }
        Emit("</thead>\n");
    }

    Emit("<tbody>\n");
    for (std::size_t r = table.headerRows; r < table.rows.size(); ++r) {
        write_row(table.rows[r], /*header=*/false);
    }
    Emit("</tbody>\n");
    Emit("</table>\n");
}

void Writer::WriteBlock(const Block& block, std::size_t depth) {
    std::visit(
        Overloaded{
            [&](const Paragraph& b) {
                Emit("<p");
                if (b.direct.alignment) {
                    Emit(" style=\"text-align:" + std::string(AlignmentName(*b.direct.alignment)) +
                         "\"");
                }
                Emit(">");
                WriteInlines(b.inlines);
                Line("</p>");
            },
            [&](const Heading& b) {
                const std::string level = std::to_string(std::clamp(b.level, 1, 6));
                Emit("<h" + level);
                if (!b.anchorId.empty()) Emit(" id=\"" + EscapeHtmlAttribute(b.anchorId) + "\"");
                Emit(">");
                WriteInlines(b.inlines);
                Line("</h" + level + ">");
            },
            [&](const CodeBlock& b) {
                Emit("<pre><code");
                if (!b.language.empty()) {
                    // The language-* convention every syntax highlighter reads.
                    Emit(" class=\"language-" + EscapeHtmlAttribute(b.language) + "\"");
                }
                Emit(">");
                Emit(EscapeHtmlText(b.text));
                Line("</code></pre>");
            },
            [&](const BlockQuote& b) {
                Emit("<blockquote>\n");
                WriteBlocks(b.blocks, depth + 1);
                Emit("</blockquote>\n");
            },
            [&](const ListBlock& b) {
                const std::string_view tag = b.ordered ? "ol" : "ul";
                Emit("<");
                Emit(tag);
                if (b.ordered && b.start != 1) Emit(" start=\"" + std::to_string(b.start) + "\"");
                if (!b.tight) Emit(" class=\"loose\"");
                Line(">");

                for (const ListItem& item : b.items) {
                    Emit("<li>");
                    if (item.checked.has_value()) {
                        Emit("<input type=\"checkbox\" disabled");
                        if (*item.checked) Emit(" checked");
                        Emit(">");
                    }
                    // A tight item holding one paragraph writes inline, so the
                    // list does not gain a blank line between every entry.
                    if (b.tight && item.blocks.size() == 1) {
                        if (const auto* paragraph = item.blocks.front().As<Paragraph>()) {
                            WriteInlines(paragraph->inlines);
                            Line("</li>");
                            continue;
                        }
                    }
                    Line("");
                    WriteBlocks(item.blocks, depth + 1);
                    Line("</li>");
                }
                Emit("</");
                Emit(tag);
                Line(">");
            },
            [&](const Table& b) { WriteTable(b, depth); },
            [&](const ImageBlock& b) {
                Emit("<figure>");
                WriteImage(b.resource, b.alt, b.title, b.widthPt, b.heightPt);
                if (!b.title.empty()) {
                    Emit("<figcaption>");
                    Emit(EscapeHtmlText(b.title));
                    Emit("</figcaption>");
                }
                Line("</figure>");
            },
            [&](const HorizontalRule&) { Line("<hr>"); },
            [&](const Footnote& b) {
                if (std::find(footnoteIds_.begin(), footnoteIds_.end(), b.id) ==
                    footnoteIds_.end()) {
                    footnoteIds_.push_back(b.id);
                    footnotes_.push_back(&b);
                }
            },
            [&](const RawBlock& b) {
                if (b.format == "html") {
                    Emit(std::string(reinterpret_cast<const char*>(b.bytes.data()),
                                     b.bytes.size()));
                    Line("");
                    return;
                }
                Note("raw-block-dropped",
                     "A " + (b.description.empty() ? b.format : b.description) +
                         " could not be represented in HTML and was omitted.",
                     Fidelity::Structural);
            },
        },
        block.node);
}

void Writer::WriteBlocks(const Blocks& blocks, std::size_t depth) {
    if (depth > kMaxBlockDepth) {
        Note("nesting-truncated", "Content nested deeper than 100 levels was omitted.",
             Fidelity::Structural);
        return;
    }
    for (const Block& block : blocks) WriteBlock(block, depth);
}

void Writer::WriteDocument() {
    const Metadata& meta = document_.meta;

    Line("<!DOCTYPE html>");
    Emit("<html");
    if (!meta.lang.empty()) Emit(" lang=\"" + EscapeHtmlAttribute(meta.lang) + "\"");
    Line(">");
    Line("<head>");
    Line("<meta charset=\"utf-8\">");
    Line("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
    Emit("<title>");
    Emit(EscapeHtmlText(meta.title.empty() ? "Document" : meta.title));
    Line("</title>");

    if (!meta.authors.empty()) {
        std::string authors;
        for (const std::string& author : meta.authors) {
            if (!authors.empty()) authors += ", ";
            authors += author;
        }
        Line("<meta name=\"author\" content=\"" + EscapeHtmlAttribute(authors) + "\">");
    }
    // Deliberately no <meta name="generator">: it would embed a version string in
    // every output and break byte-for-byte golden comparison on every release.

    Line("<style>");
    Emit(kStylesheet);
    Line("</style>");
    Line("</head>");
    Line("<body>");

    WriteBlocks(document_.blocks, 0);

    if (!footnotes_.empty()) {
        Line("<section class=\"footnotes\">");
        Line("<hr>");
        Line("<ol>");
        for (std::size_t i = 0; i < footnotes_.size(); ++i) {
            const std::string number = std::to_string(i + 1);
            Emit("<li id=\"fn" + number + "\">");
            if (footnotes_[i] != nullptr) {
                Line("");
                WriteBlocks(footnotes_[i]->blocks, 1);
            }
            Line("<a href=\"#fnref" + number + "\">&#8617;</a></li>");
        }
        Line("</ol>");
        Line("</section>");
    }

    Line("</body>");
    Line("</html>");
}

}  // namespace

std::string EscapeHtmlText(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            // '>' only closes a tag after a '<', but escaping it costs nothing
            // and defends against the "]]>" and "--!>" edge cases that trip up
            // sanitisers downstream.
            case '>': out += "&gt;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string EscapeHtmlAttribute(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            // Single quotes are escaped too: we always write double-quoted
            // attributes, but a value that escapes only one quote style is one
            // refactor away from an injection.
            case '\'': out += "&#39;"; break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}

std::string_view DefaultStylesheet() { return kStylesheet; }

Result WriteHtml(const doc::Document& document, const OptionBag& options, std::string& into) {
    Result result = Result::Ok();
    Writer writer(document, options, result);
    writer.WriteDocument();
    into = writer.Take();
    return result;
}

std::vector<MediaProfile> HtmlWriter::Writes() const {
    return {*MediaProfile::Parse(kHtmlProfileId)};
}

EdgeCost HtmlWriter::WriteCost(const MediaProfile& /*to*/) const {
    EdgeCost cost;
    // HIGH, not Lossless: HTML expresses almost everything the IR holds, but the
    // OOXML pressure-valve blocks cannot survive and section 8.8's example for
    // HIGH is exactly "MD -> HTML".
    cost.fidelity = Fidelity::High;
    cost.generational = 0;
    cost.cpuPerMiB = 5.0f;
    cost.memPeakFactor = 4.0f;
    cost.requiresDisk = false;
    return cost;
}

Result HtmlWriter::Write(ConvertContext& context, const doc::Document& document,
                         const MediaProfile& /*to*/, ISink& sink) {
    if (context.cancelled()) return Result::Cancelled();

    std::string html;
    Result result = WriteHtml(document, context.options, html);
    if (!result) return result;
    if (context.cancelled()) return Result::Cancelled();

    WriteAll(sink, html);
    sink.Commit();
    context.progress.Report(1.0, html.size());
    return result;
}

}  // namespace morph::dochtml
