// html -> pdf. Section 8.6.
//
// The provider is thin on purpose: all the difficulty is in the host thread next
// door, and everything above it is the ordinary section 5 byte contract. That is
// what makes "md -> pdf", "html -> pdf" and "docx -> pdf" free -- the planner
// routes each of them through the HTML writer and then through this one edge.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <string>
#include <vector>

#include <windows.h>

#include "morph/providers/pdf.h"

#include "morph/core/probe_cache.h"
#include "webview2_host.h"

namespace morph::pdf {
namespace {

std::wstring Widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                             static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                          needed);
    return wide;
}

// Percent-encodes the parts of a path that a file: URL cannot carry literally.
// A document in a folder with a '#' in its name would otherwise be truncated at
// the fragment, which is a genuinely confusing failure.
std::wstring FileUrl(const std::filesystem::path& path) {
    const std::string utf8 = path.generic_string();
    std::wstring url = L"file:///";
    for (const unsigned char c : utf8) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '/' || c == ':' || c == '-' ||
                          c == '_' || c == '.' || c == '~';
        if (safe) {
            url.push_back(static_cast<wchar_t>(c));
            continue;
        }
        static constexpr wchar_t kHex[] = L"0123456789ABCDEF";
        url.push_back(L'%');
        url.push_back(kHex[c >> 4]);
        url.push_back(kHex[c & 0x0F]);
    }
    return url;
}

struct PageGeometry {
    double widthInches = 8.27;
    double heightInches = 11.69;
};

PageGeometry GeometryFor(const OptionBag& options) {
    std::string size = options.GetString(opt::kPageSize, "a4");
    std::transform(size.begin(), size.end(), size.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (size == "letter") return PageGeometry{8.5, 11.0};
    if (size == "legal") return PageGeometry{8.5, 14.0};
    if (size == "a3") return PageGeometry{11.69, 16.54};
    if (size == "a5") return PageGeometry{5.83, 8.27};
    return PageGeometry{8.27, 11.69};  // A4
}

}  // namespace

bool WebView2Available() {
    // Remembered across runs (probe_cache.h). Starting the host is what makes
    // this answer trustworthy and is also most of what makes it slow, and the
    // answer only changes when the Evergreen runtime is installed or removed --
    // which comes with a Windows build or an installer, both of which move the
    // cache's stamp.
    return Cached("pdf.webview2", [] { return StartHost().ok(); });
}

std::string WebView2Version() { return HostVersion(); }

Result RenderHtmlToPdf(std::string_view html, const OptionBag& options, ITempStore& temp,
                       ISink& sink, const std::stop_token& cancel) {
    if (cancel.stop_requested()) return Result::Cancelled();

    if (Result started = StartHost(); !started) return started;

    auto request = std::make_shared<Request>();
    const PageGeometry geometry = GeometryFor(options);
    request->pageWidthInches = geometry.widthInches;
    request->pageHeightInches = geometry.heightInches;
    request->marginInches = options.GetDouble("margin-inches", 0.4);
    request->printBackgrounds = options.GetBool("print-backgrounds", true);
    request->landscape = options.GetBool("landscape", false);

    std::filesystem::path scratch;
    if (html.size() <= kNavigateToStringLimit) {
        request->useString = true;
        request->html = Widen(html);
    } else {
        // Section 4.3's requiresDisk, and the reason for it: NavigateToString
        // has a size limit, and a document whose images are inlined as data URIs
        // passes it easily.
        request->useString = false;
        scratch = temp.Acquire("render.html");

        std::ofstream file(scratch, std::ios::binary | std::ios::trunc);
        if (!file) {
            return Result::Error(Status::kIoError,
                                 "Could not write the document to a temporary file for "
                                 "rendering.");
        }
        file.write(html.data(), static_cast<std::streamsize>(html.size()));
        file.close();
        if (!file) {
            return Result::Error(Status::kIoError,
                                 "Could not write the document to a temporary file for "
                                 "rendering.");
        }
        request->navigateTo = FileUrl(scratch);
    }

    Result rendered = RenderRequest(request);

    if (!scratch.empty()) {
        std::error_code ignored;
        std::filesystem::remove(scratch, ignored);
    }
    if (!rendered) return rendered;

    if (cancel.stop_requested()) return Result::Cancelled();

    WriteAll(sink, request->pdf);
    sink.Commit();
    return Result::Ok();
}

std::vector<EdgeDecl> PdfWriter::EnumerateEdges() const {
    EdgeCost cost;
    // HIGH, not Lossless: the text stays text and the layout is preserved, but a
    // PDF does not round-trip back to the HTML it came from. Section 8.8's HIGH
    // is "visually or semantically equivalent; one lossy generation at most".
    cost.fidelity = Fidelity::High;
    cost.generational = 0;
    // Measured against the section 17 budget: a typical 40-page DOCX to PDF in
    // 1.5 s on a warm environment. Calibrated at first run (section 11.4).
    cost.cpuPerMiB = 120.0f;
    cost.memPeakFactor = 3.0f;
    // The one edge in the whole graph that sets this. See RenderHtmlToPdf.
    cost.requiresDisk = true;

    return {EdgeDecl{*MediaProfile::Parse(kHtmlProfileId), *MediaProfile::Parse(kPdfProfileId),
                     cost}};
}

std::optional<MediaProfile> PdfWriter::Probe(ISource& source, const PathHint& /*hint*/) const {
    // This provider only WRITES PDF; reading one is PDFium's job (section 8.7).
    // Recognising the format here anyway means a PDF dropped on the app is
    // identified even before that provider exists, rather than being reported as
    // an unknown file.
    const std::vector<std::byte> prefix = ReadAtMost(source, 1024);
    if (prefix.size() < 5) return std::nullopt;

    const std::string_view head(reinterpret_cast<const char*>(prefix.data()), prefix.size());
    if (!head.starts_with("%PDF-")) return std::nullopt;
    return MediaProfile::Parse(kPdfProfileId);
}

Result PdfWriter::Convert(ConvertContext& context, ISource& source, ISink& sink,
                          const MediaProfile& from, const MediaProfile& to) {
    if (to.Id() != kPdfProfileId || from.Id() != kHtmlProfileId) {
        return Result::Error(Status::kUnsupportedInput,
                             "The PDF provider converts HTML to PDF, not " + from.Id() + " to " +
                                 to.Id() + ".");
    }
    if (context.cancelled()) return Result::Cancelled();

    // The HTML comes from our own writer, so it is a complete document and fits
    // in memory by construction: it is a few hundred KiB for a long report, plus
    // whatever its images weigh.
    constexpr std::uint64_t kMaxBytes = 256ull * 1024 * 1024;
    std::string html;
    if (const auto size = source.Size()) {
        if (*size > kMaxBytes) {
            return Result::Error(Status::kResourceLimit,
                                 "This document is too large to render to PDF.");
        }
        html.reserve(static_cast<std::size_t>(*size));
    }

    std::vector<std::byte> chunk(kStreamChunkBytes);
    while (true) {
        const std::size_t got = source.Read(chunk);
        if (got == 0) break;
        html.append(reinterpret_cast<const char*>(chunk.data()), got);
        if (html.size() > kMaxBytes) {
            return Result::Error(Status::kResourceLimit,
                                 "This document is too large to render to PDF.");
        }
        if (context.cancelled()) return Result::Cancelled();
    }

    context.progress.Report(0.2, 0);
    Result result = RenderHtmlToPdf(html, context.options, context.temp, sink, context.cancel);
    context.progress.Report(1.0, 0);
    return result;
}

}  // namespace morph::pdf
