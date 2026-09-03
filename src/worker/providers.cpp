#include "providers.h"

#include "morph/providers/docx.h"
#include "morph/providers/html.h"
#include "morph/providers/markdown.h"
#include "morph/providers/pdf.h"

namespace morph::worker {
namespace {

// The table. Statically linked, registered in order, and that order is the
// probe precedence: Executor::Probe consults readers in registration order, so
// the format with the weakest sniff goes last.
//
// Markdown is last for exactly that reason. It has no magic bytes -- every byte
// sequence is valid Markdown -- so its probe leans on the extension, and putting
// it ahead of DOCX would let a .docx be claimed as text by a document whose
// extension had been renamed.
docooxml::DocxReader& DocxReaderInstance() {
    static docooxml::DocxReader instance;
    return instance;
}
docooxml::DocxWriter& DocxWriterInstance() {
    static docooxml::DocxWriter instance;
    return instance;
}
dochtml::HtmlReader& HtmlReaderInstance() {
    static dochtml::HtmlReader instance;
    return instance;
}
dochtml::HtmlWriter& HtmlWriterInstance() {
    static dochtml::HtmlWriter instance;
    return instance;
}
docmd::MarkdownReader& MarkdownReaderInstance() {
    static docmd::MarkdownReader instance;
    return instance;
}
docmd::MarkdownWriter& MarkdownWriterInstance() {
    static docmd::MarkdownWriter instance;
    return instance;
}
pdf::PdfReader& PdfReaderInstance() {
    static pdf::PdfReader instance;
    return instance;
}
pdf::PdfWriter& PdfWriterInstance() {
    static pdf::PdfWriter instance;
    return instance;
}

}  // namespace

Providers::Providers() {
    // Readers, strongest sniff first.
    executor_.Add(DocxReaderInstance());
    executor_.Add(PdfReaderInstance());
    executor_.Add(HtmlReaderInstance());
    executor_.Add(MarkdownReaderInstance());

    // Writers. Order does not matter here: a writer is selected by the plan, not
    // by probing.
    executor_.Add(DocxWriterInstance());
    executor_.Add(HtmlWriterInstance());
    executor_.Add(MarkdownWriterInstance());

    // Byte providers.
    executor_.Add(PdfWriterInstance());

    executor_.BuildGraph(graph_);
}

void Providers::ProbeCapabilities() {
    std::vector<ProviderId> unavailable;

    // Section 20 lists a missing WebView2 runtime as a real risk in managed
    // fleets, with the mitigation being to "detect at startup and mark
    // PDF-output edges unavailable rather than crashing". The alternative is a
    // user who picks PDF, waits, and is told it failed.
    if (pdf::WebView2Available()) {
        capabilities_.Add("webview2");
    } else {
        unavailable.emplace_back("pdf");
    }

    graph_.RemoveProviderEdges(unavailable);
}

}  // namespace morph::worker
