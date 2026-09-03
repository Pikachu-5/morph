// Morph — PDF output through WebView2 (architecture section 8.6).
//
// "The cleanest trick in the whole design. WebView2 ships with Windows 11 and is
//  redistributable; it embeds Chromium's print pipeline, which produces better
//  PDF than anything we could write and better than most commercial libraries.
//  It is free, it is not GPL, and the API is a single call."
//
// WHY THIS IS AN html -> pdf EDGE RATHER THAN AN IR -> pdf WRITER
//
// Section 4.3 spells the document route out as "docx -> IR -> html -> pdf -> png",
// so html -> pdf is an edge in its own right and the planner should see it that
// way: it has its own cost, its own fidelity rating, and it is the one hop in the
// entire graph that sets requiresDisk. Modelling it as an IR -> pdf writer would
// hide the HTML stage from the planner and from the fidelity badge, and would
// duplicate the HTML writer inside this provider.
//
// The consequence section 8.6 points out is the good one: "md -> pdf, html -> pdf
// and docx -> pdf all fall out of the graph for free, because every one of them
// routes through the HTML writer." Nobody writes those three converters.
#pragma once

#include <string>
#include <string_view>

#include "morph/core/provider.h"

namespace morph::pdf {

// Whether a usable WebView2 runtime is present. Section 20 lists the Evergreen
// runtime being absent in a managed fleet as a real risk, and the mitigation is
// to "detect at startup and mark PDF-output edges unavailable rather than
// crashing" -- which is what FormatGraph::RemoveProviderEdges is for.
[[nodiscard]] bool WebView2Available();

// The installed runtime's version, or empty when there is none. Reported in
// diagnostics, because "PDF output is unavailable" is a much less useful thing
// to tell a user than which component is missing.
[[nodiscard]] std::string WebView2Version();

// Renders a complete HTML document to PDF.
//
// Exposed as a free function so it can be tested without going through the
// provider and the graph. Returns kMissingCapability when no runtime is present,
// which the UI turns into a Store deep link rather than an error (section 6.2).
[[nodiscard]] Result RenderHtmlToPdf(std::string_view html, const OptionBag& options,
                                     ITempStore& temp, ISink& sink,
                                     const std::stop_token& cancel);

class PdfWriter final : public IConversionProvider {
public:
    [[nodiscard]] std::string_view Id() const noexcept override { return "pdf"; }
    [[nodiscard]] std::vector<EdgeDecl> EnumerateEdges() const override;
    [[nodiscard]] std::optional<MediaProfile> Probe(ISource& source,
                                                    const PathHint& hint) const override;
    [[nodiscard]] Result Convert(ConvertContext& context, ISource& source, ISink& sink,
                                 const MediaProfile& from, const MediaProfile& to) override;
};

inline constexpr std::string_view kPdfProfileId = "application/pdf";
inline constexpr std::string_view kHtmlProfileId = "text/html";

// NavigateToString is documented with a size limit around 2 MB, and a document
// whose images are inlined as data URIs passes that easily. Above it the HTML is
// written to the temp store and loaded through a file: URL instead -- which is
// the real reason this edge declares requiresDisk (section 4.3).
inline constexpr std::size_t kNavigateToStringLimit = 1536 * 1024;

}  // namespace morph::pdf
