// PDF -> IR. Section 8.7.
//
// "Text extraction to IR is best-effort and must be labelled as such. PDF has no
//  paragraphs -- only positioned glyphs. Reconstructing structure means
//  clustering text runs by baseline and gap heuristics, inferring paragraph
//  breaks from vertical spacing, and detecting columns from horizontal
//  distribution. It works acceptably on single-column prose and poorly on
//  anything else. Be explicit in the UI that pdf -> docx is a STRUCTURAL
//  conversion."
//
// That paragraph is the whole design brief, and the honesty in it is the feature.
// Every other free converter presents PDF import as if it recovered a document;
// what it actually recovers is a guess about where the paragraphs were. Morph
// makes the same guess and then says so, through the fidelity contract, before
// the user commits.
//
// WHAT THIS DOES AND DOES NOT ATTEMPT
//
//   Does:      lines from baselines, paragraphs from vertical gaps, headings
//              from relative font size, bold and italic from the font name.
//   Does not:  columns, tables, lists, figures, reading order on complex layouts.
//              Each of those is a research problem dressed as a feature, and
//              getting one subtly wrong is worse than not offering it.
//
// A tagged PDF carries real structure and section 8.7 says it should be used
// when present. This reader detects tagging and records it, but still uses
// geometry; walking the structure tree is a separate piece of work and claiming
// it half-done would make the fidelity rating a lie.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <fpdf_doc.h>
#include <fpdf_structtree.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include "morph/providers/pdf.h"

namespace morph::pdf {
namespace {

using namespace morph::doc;

// PDFium's library init is global and not reference counted, so it is done once
// per process and never torn down. FPDF_DestroyLibrary while another thread held
// a document would be a crash, and the worker is short-lived anyway.
void EnsureLibrary() {
    static const bool initialised = [] {
        FPDF_LIBRARY_CONFIG config{};
        config.version = 2;
        config.m_pUserFontPaths = nullptr;
        config.m_pIsolate = nullptr;
        config.m_v8EmbedderSlot = 0;
        FPDF_InitLibraryWithConfig(&config);
        return true;
    }();
    (void)initialised;
}

void AppendUtf8(std::string& out, std::uint32_t codepoint) {
    if (codepoint > 0x10FFFFu || (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) return;
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

// PDFium hands metadata back as UTF-16LE including the terminator.
std::string MetaText(FPDF_DOCUMENT document, const char* key) {
    const unsigned long bytes = FPDF_GetMetaText(document, key, nullptr, 0);
    if (bytes <= 2) return {};

    std::vector<unsigned short> utf16(bytes / 2 + 1, 0);
    FPDF_GetMetaText(document, key, utf16.data(), bytes);

    std::string out;
    for (std::size_t i = 0; i + 1 < utf16.size(); ++i) {
        const std::uint32_t unit = utf16[i];
        if (unit == 0) break;
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < utf16.size()) {
            const std::uint32_t low = utf16[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                AppendUtf8(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
                ++i;
                continue;
            }
        }
        AppendUtf8(out, unit);
    }
    return out;
}

struct Glyph {
    std::uint32_t codepoint = 0;
    double left = 0, right = 0, bottom = 0, top = 0;
    double fontSize = 0;
    bool bold = false;
    bool italic = false;
};

struct Line {
    std::string text;
    double left = 0, right = 0, baseline = 0, height = 0;
    double fontSize = 0;
    bool bold = false;
    bool italic = false;
};

// A font name says more about the run than PDFium's flags do, because a PDF may
// synthesise emphasis by choosing a different face rather than setting a flag.
void ClassifyFont(std::string_view name, bool& bold, bool& italic) {
    std::string lowered(name);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    bold = lowered.find("bold") != std::string::npos ||
           lowered.find("black") != std::string::npos ||
           lowered.find("heavy") != std::string::npos ||
           lowered.find("semibold") != std::string::npos;
    italic = lowered.find("italic") != std::string::npos ||
             lowered.find("oblique") != std::string::npos;
}

std::vector<Glyph> ReadGlyphs(FPDF_TEXTPAGE textPage) {
    const int count = FPDFText_CountChars(textPage);
    std::vector<Glyph> glyphs;
    if (count <= 0) return glyphs;
    glyphs.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i) {
        Glyph glyph;
        glyph.codepoint = FPDFText_GetUnicode(textPage, i);
        if (glyph.codepoint == 0) continue;

        if (!FPDFText_GetCharBox(textPage, i, &glyph.left, &glyph.right, &glyph.bottom,
                                 &glyph.top)) {
            continue;
        }
        glyph.fontSize = FPDFText_GetFontSize(textPage, i);

        char fontName[128]{};
        int flags = 0;
        const unsigned long written =
            FPDFText_GetFontInfo(textPage, i, fontName, sizeof(fontName), &flags);
        if (written > 0) {
            ClassifyFont(std::string_view(fontName, std::min<unsigned long>(written - 1,
                                                                           sizeof(fontName) - 1)),
                         glyph.bold, glyph.italic);
        }
        // The PDF font descriptor's italic flag, when the name did not say so.
        constexpr int kFlagItalic = 1 << 6;
        if (!glyph.italic && (flags & kFlagItalic) != 0) glyph.italic = true;

        glyphs.push_back(glyph);
    }
    return glyphs;
}

// Groups glyphs into lines. PDF content order usually matches reading order for
// single-column prose, which is exactly the case this reader claims to handle,
// so glyphs are taken in order and a line ends when the baseline moves.
std::vector<Line> BuildLines(const std::vector<Glyph>& glyphs) {
    std::vector<Line> lines;
    if (glyphs.empty()) return lines;

    Line current;
    bool open = false;
    double previousRight = 0;
    std::size_t boldCount = 0, italicCount = 0, glyphCount = 0;

    const auto finish = [&] {
        if (!open) return;
        // Trailing whitespace on a line is layout, not content.
        while (!current.text.empty() && (current.text.back() == ' ' || current.text.back() == '\t')) {
            current.text.pop_back();
        }
        if (!current.text.empty()) {
            current.bold = glyphCount > 0 && boldCount * 2 > glyphCount;
            current.italic = glyphCount > 0 && italicCount * 2 > glyphCount;
            lines.push_back(current);
        }
        current = Line{};
        open = false;
        boldCount = italicCount = glyphCount = 0;
    };

    for (const Glyph& glyph : glyphs) {
        // A carriage return or newline in the content stream is a hard break.
        if (glyph.codepoint == '\r' || glyph.codepoint == '\n') {
            finish();
            continue;
        }

        const double size = glyph.fontSize > 0 ? glyph.fontSize : (glyph.top - glyph.bottom);
        if (open) {
            // A new line when the baseline moves by more than a third of the
            // font size. Superscripts and subscripts move less than that, which
            // is why the threshold is not simply "any change".
            const bool movedDown = std::abs(glyph.bottom - current.baseline) > size * 0.35;
            // Or when the pen jumps backwards: a wrapped line starts at the left
            // margin again.
            const bool wrapped = glyph.left < previousRight - size * 2.0;
            if (movedDown || wrapped) finish();
        }

        if (!open) {
            current = Line{};
            current.left = glyph.left;
            current.baseline = glyph.bottom;
            current.height = glyph.top - glyph.bottom;
            current.fontSize = size;
            open = true;
        }

        // PDF often omits space glyphs and relies on positioning instead. A gap
        // wider than a quarter of the font size is a word break.
        if (!current.text.empty() && glyph.left - previousRight > size * 0.25 &&
            current.text.back() != ' ' && glyph.codepoint != ' ') {
            current.text.push_back(' ');
        }

        AppendUtf8(current.text, glyph.codepoint);
        current.right = glyph.right;
        current.fontSize = std::max(current.fontSize, size);
        previousRight = glyph.right;

        if (glyph.codepoint != ' ') {
            ++glyphCount;
            if (glyph.bold) ++boldCount;
            if (glyph.italic) ++italicCount;
        }
    }
    finish();
    return lines;
}

double Median(std::vector<double> values) {
    if (values.empty()) return 0;
    const std::size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle),
                     values.end());
    return values[middle];
}

// The font size of ordinary prose on a page, weighted by how much text is set in
// it rather than by how many lines use it.
//
// The unweighted median gets this wrong on short pages, which is where it
// matters most: a page holding one heading and one line of body text has two
// samples, and the median of two picks the larger -- so the heading defines the
// baseline and then fails to stand out against it. Weighting by character count
// makes the body dominate, because a heading is one short line and prose is not.
double BodyFontSize(const std::vector<Line>& lines) {
    std::vector<std::pair<double, std::size_t>> samples;
    std::size_t total = 0;
    for (const Line& line : lines) {
        if (line.text.empty()) continue;
        samples.emplace_back(line.fontSize, line.text.size());
        total += line.text.size();
    }
    if (samples.empty()) return 0;

    std::sort(samples.begin(), samples.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::size_t seen = 0;
    for (const auto& [size, weight] : samples) {
        seen += weight;
        if (seen * 2 >= total) return size;
    }
    return samples.back().first;
}

// Turns lines into blocks. This is the guess, and everything about it is a
// heuristic tuned for single-column prose.
void LinesToBlocks(const std::vector<Line>& lines, double bodyFontSize, Blocks& out,
                   std::vector<std::string>& anchors) {
    if (lines.empty()) return;

    // Typical line spacing on this page, used as the yardstick for "this gap is
    // bigger than a line break, so it is a paragraph break".
    std::vector<double> gaps;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const double gap = lines[i - 1].baseline - lines[i].baseline;
        if (gap > 0) gaps.push_back(gap);
    }
    const double typicalGap = gaps.empty() ? bodyFontSize * 1.2 : Median(gaps);

    std::string paragraph;
    bool paragraphBold = false;
    bool paragraphItalic = false;

    const auto flush = [&] {
        if (paragraph.empty()) return;
        Paragraph node;
        Inline text = Inline::Of(Text{paragraph, {}});
        if (paragraphItalic) text = Inline::Of(Emphasis{{std::move(text)}});
        if (paragraphBold) text = Inline::Of(Strong{{std::move(text)}});
        node.inlines.push_back(std::move(text));
        out.push_back(Block::Of(std::move(node)));
        paragraph.clear();
        paragraphBold = paragraphItalic = false;
    };

    const auto slug = [&anchors](std::string_view text) {
        std::string s;
        for (const char c : text) {
            const auto u = static_cast<unsigned char>(c);
            if (u >= 'A' && u <= 'Z') s.push_back(static_cast<char>(u - 'A' + 'a'));
            else if ((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u >= 0x80) {
                s.push_back(static_cast<char>(u));
            } else if (u == ' ') {
                s.push_back('-');
            }
        }
        while (!s.empty() && s.back() == '-') s.pop_back();
        if (s.empty()) s = "section";
        std::string candidate = s;
        for (int i = 1; std::find(anchors.begin(), anchors.end(), candidate) != anchors.end();
             ++i) {
            candidate = s + "-" + std::to_string(i);
        }
        anchors.push_back(candidate);
        return candidate;
    };

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const Line& line = lines[i];

        // A line noticeably larger than the body text is a heading. The level
        // comes from how much larger, which is crude but stable: a document's
        // h1 is reliably bigger than its h2.
        const double ratio = bodyFontSize > 0 ? line.fontSize / bodyFontSize : 1.0;
        const bool isHeading = ratio > 1.15 && line.text.size() < 200;

        if (isHeading) {
            flush();
            Heading heading;
            heading.level = ratio > 1.8 ? 1 : ratio > 1.5 ? 2 : ratio > 1.3 ? 3 : 4;
            heading.inlines.push_back(Inline::Of(Text{line.text, {}}));
            heading.anchorId = slug(line.text);
            out.push_back(Block::Of(std::move(heading)));
            continue;
        }

        if (!paragraph.empty()) {
            const double gap = lines[i - 1].baseline - line.baseline;
            // A gap half again as large as the usual line spacing is a paragraph
            // break. So is a line that starts noticeably further right than the
            // one before it, which is a first-line indent.
            const bool bigGap = gap > typicalGap * 1.45;
            const bool indented = line.left > lines[i - 1].left + line.fontSize * 0.8;
            if (bigGap || indented) {
                flush();
            } else {
                // Joining a wrapped line. A trailing hyphen usually means the
                // word was split across the break, so the hyphen goes with it.
                if (paragraph.ends_with("-")) {
                    paragraph.pop_back();
                } else {
                    paragraph.push_back(' ');
                }
            }
        }

        if (paragraph.empty()) {
            paragraphBold = line.bold;
            paragraphItalic = line.italic;
        } else {
            paragraphBold = paragraphBold && line.bold;
            paragraphItalic = paragraphItalic && line.italic;
        }
        paragraph += line.text;
    }
    flush();
}

struct DocumentCloser {
    FPDF_DOCUMENT document = nullptr;
    ~DocumentCloser() {
        if (document != nullptr) FPDF_CloseDocument(document);
    }
};

std::string DescribeLoadError() {
    switch (FPDF_GetLastError()) {
        case FPDF_ERR_SUCCESS: return {};
        case FPDF_ERR_FILE:    return "The file could not be read.";
        case FPDF_ERR_FORMAT:  return "The file is not a valid PDF.";
        case FPDF_ERR_PASSWORD:
            return "This PDF is password protected. Morph cannot open encrypted documents.";
        case FPDF_ERR_SECURITY: return "This PDF uses an unsupported security scheme.";
        case FPDF_ERR_PAGE:     return "A page of this PDF could not be read.";
        default:                return "The PDF could not be opened.";
    }
}

}  // namespace

Result ReadPdf(std::span<const std::byte> bytes, const OptionBag& /*options*/,
               doc::Document& into) {
    EnsureLibrary();

    if (bytes.size() < 5) {
        return Result::Error(Status::kMalformedInput, "This file is too small to be a PDF.");
    }

    DocumentCloser closer;
    closer.document = FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr);
    if (closer.document == nullptr) {
        const std::string detail = DescribeLoadError();
        return Result::Error(FPDF_GetLastError() == FPDF_ERR_PASSWORD
                                 ? Status::kUnsupportedInput
                                 : Status::kMalformedInput,
                             detail.empty() ? "The PDF could not be opened." : detail);
    }

    Result result = Result::Ok();
    // Section 8.7 and section 8.8: the rating for this conversion is STRUCTURAL,
    // and the note says why in terms a user can act on.
    result.Incurred(Fidelity::Structural);
    result.Note("pdf-structure-inferred",
                "PDF files store positioned text rather than paragraphs, so the structure of "
                "this document was inferred from its layout. Headings and paragraph breaks are "
                "a best guess, and columns, tables and lists are not recovered.");

    into.meta.title = MetaText(closer.document, "Title");
    if (const std::string author = MetaText(closer.document, "Author"); !author.empty()) {
        into.meta.authors.push_back(author);
    }
    if (const std::string subject = MetaText(closer.document, "Subject"); !subject.empty()) {
        into.meta.extra.emplace("subject", subject);
    }

    const int pages = FPDF_GetPageCount(closer.document);
    if (pages <= 0) {
        return Result::Error(Status::kMalformedInput, "This PDF contains no pages.");
    }
    into.meta.extra.emplace("pdf-page-count", std::to_string(pages));

    bool tagged = false;
    bool anyText = false;

    for (int index = 0; index < pages; ++index) {
        FPDF_PAGE page = FPDF_LoadPage(closer.document, index);
        if (page == nullptr) continue;

        if (!tagged) {
            // A tagged PDF carries real structure. It is recorded rather than
            // used: walking the structure tree is separate work, and claiming it
            // half-done would make the fidelity rating a lie.
            if (FPDF_STRUCTTREE tree = FPDF_StructTree_GetForPage(page); tree != nullptr) {
                tagged = true;
                FPDF_StructTree_Close(tree);
            }
        }

        FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
        if (textPage != nullptr) {
            const std::vector<Glyph> glyphs = ReadGlyphs(textPage);
            const std::vector<Line> lines = BuildLines(glyphs);

            if (!lines.empty()) {
                anyText = true;
                std::vector<std::string> anchors;
                LinesToBlocks(lines, BodyFontSize(lines), into.blocks, anchors);
            }
            FPDFText_ClosePage(textPage);
        }
        FPDF_ClosePage(page);

        if (into.blocks.size() > kMaxBlockCount) {
            result.Note("truncated", "This PDF contains more content than Morph will process.");
            break;
        }
    }

    if (!anyText) {
        // A scanned document. Saying so is far more useful than handing back an
        // empty file, and OCR is explicitly out of scope for v1 (section 1).
        return Result::Error(
            Status::kUnsupportedInput,
            "This PDF contains no extractable text. It is probably a scan, which needs OCR "
            "that Morph does not include.");
    }

    if (tagged) {
        into.meta.extra.emplace("pdf-tagged", "true");
        result.Note("pdf-tagging-unused",
                    "This PDF carries accessibility tags describing its real structure. Morph "
                    "does not read them yet and inferred the structure from layout instead.");
    }
    return result;
}

std::vector<MediaProfile> PdfReader::Reads() const {
    return {*MediaProfile::Parse(kPdfProfileId)};
}

EdgeCost PdfReader::ReadCost(const MediaProfile& /*from*/) const {
    EdgeCost cost;
    // The rating section 4.3 gives this exact conversion: "Fidelity STRUCTURAL.
    // Layout is not recoverable from PDF in the general case; we say so rather
    // than pretending."
    cost.fidelity = Fidelity::Structural;
    cost.generational = 0;
    cost.cpuPerMiB = 40.0f;
    cost.memPeakFactor = 5.0f;
    cost.requiresDisk = false;
    return cost;
}

std::optional<MediaProfile> PdfReader::Probe(ISource& source, const PathHint& /*hint*/) const {
    const std::vector<std::byte> prefix = ReadAtMost(source, 1024);
    if (prefix.size() < 5) return std::nullopt;

    const std::string_view head(reinterpret_cast<const char*>(prefix.data()), prefix.size());
    if (!head.starts_with("%PDF-")) return std::nullopt;
    return MediaProfile::Parse(kPdfProfileId);
}

Result PdfReader::Read(ConvertContext& context, ISource& source, const MediaProfile& /*from*/,
                       doc::Document& into) {
    if (context.cancelled()) return Result::Cancelled();

    // PDFium wants the whole file: the cross-reference table is at the end and
    // the object graph is random access.
    constexpr std::uint64_t kMaxBytes = 512ull * 1024 * 1024;
    std::vector<std::byte> bytes;
    if (const auto size = source.Size()) {
        if (*size > kMaxBytes) {
            return Result::Error(Status::kResourceLimit, "This PDF is larger than 512 MB.");
        }
        bytes.reserve(static_cast<std::size_t>(*size));
    }

    std::vector<std::byte> chunk(kStreamChunkBytes);
    while (true) {
        const std::size_t got = source.Read(chunk);
        if (got == 0) break;
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + got);
        if (bytes.size() > kMaxBytes) {
            return Result::Error(Status::kResourceLimit, "This PDF is larger than 512 MB.");
        }
        if (context.cancelled()) return Result::Cancelled();
    }

    context.progress.Report(0.3, 0);
    Result result = ReadPdf(bytes, context.options, into);
    context.progress.Report(1.0, 0);
    return result;
}

}  // namespace morph::pdf
