// A small format graph that mirrors the worked examples in architecture
// section 4.3. It is not the real provider table — that arrives with the
// providers in M1 onwards — but it has the same shape: parallel edges, a remux
// fast path, a document spine routed through the IR, and a rasterising escape
// hatch that must only be chosen when nothing better exists.
#pragma once

#include <string_view>

#include "morph/core/graph.h"

namespace morph::test {

inline EdgeCost Cost(Fidelity fidelity, std::uint8_t generational, float cpu_per_mib,
                     float mem_peak = 2.0f, bool requires_disk = false) {
    EdgeCost cost;
    cost.fidelity = fidelity;
    cost.generational = generational;
    cost.cpuPerMiB = cpu_per_mib;
    cost.memPeakFactor = mem_peak;
    cost.requiresDisk = requires_disk;
    return cost;
}

inline MediaProfile P(std::string_view spec) { return *MediaProfile::Parse(spec); }

// Profile ids used by the tests, so a typo is a compile error rather than a
// silently unreachable node.
namespace ids {
inline constexpr std::string_view kHeif = "image/heif";
inline constexpr std::string_view kJpeg = "image/jpeg";
inline constexpr std::string_view kPng  = "image/png";
inline constexpr std::string_view kWebp = "image/webp";

inline constexpr std::string_view kMp4H264 = "video/mp4;codec=h264";
inline constexpr std::string_view kMkvH264 = "video/mkv;codec=h264";
inline constexpr std::string_view kWebmVp9 = "video/webm;codec=vp9";

inline constexpr std::string_view kDocx = "application/docx";
inline constexpr std::string_view kIr   = "application/morph-ir";
inline constexpr std::string_view kHtml = "text/html";
inline constexpr std::string_view kMd   = "text/markdown";
inline constexpr std::string_view kPdf  = "application/pdf";
inline constexpr std::string_view kTxt  = "text/plain";
}  // namespace ids

inline FormatGraph MakeTestGraph() {
    FormatGraph g;
    using namespace ids;

    // --- Images (WIC). One decode + one encode is one lossy generation. -------
    g.AddEdge(P(kHeif), P(kWebp), Cost(Fidelity::High, 1, 8.0f), "wic");
    g.AddEdge(P(kHeif), P(kJpeg), Cost(Fidelity::High, 1, 7.0f), "wic");
    g.AddEdge(P(kJpeg), P(kPng),  Cost(Fidelity::Lossless, 0, 12.0f), "wic");
    g.AddEdge(P(kPng),  P(kJpeg), Cost(Fidelity::High, 1, 6.0f), "wic");
    g.AddEdge(P(kPng),  P(kWebp), Cost(Fidelity::Lossless, 0, 14.0f), "wic");
    g.AddEdge(P(kWebp), P(kPng),  Cost(Fidelity::Lossless, 0, 11.0f), "wic");

    // --- Video. The remux fast path is the highest-value edge in the graph. ---
    // Same codec node, different container: no re-encode, near-IO-bound.
    g.AddEdge(P(kMp4H264), P(kMkvH264), Cost(Fidelity::Lossless, 0, 0.4f, 1.05f), "ffmpeg");
    g.AddEdge(P(kMkvH264), P(kMp4H264), Cost(Fidelity::Lossless, 0, 0.4f, 1.05f), "ffmpeg");
    // Different codec node: a full re-encode, one generation of loss.
    g.AddEdge(P(kMp4H264), P(kWebmVp9), Cost(Fidelity::High, 1, 900.0f, 1.5f), "ffmpeg");

    // --- Documents. Everything routes through the IR (section 8.2). ----------
    g.AddEdge(P(kDocx), P(kIr),   Cost(Fidelity::Lossless, 0, 20.0f, 4.0f), "docooxml");
    g.AddEdge(P(kIr),   P(kDocx), Cost(Fidelity::Lossless, 0, 18.0f, 4.0f), "docooxml");
    g.AddEdge(P(kMd),   P(kIr),   Cost(Fidelity::Lossless, 0, 3.0f), "docmd");
    g.AddEdge(P(kIr),   P(kMd),   Cost(Fidelity::Structural, 0, 3.0f), "docmd");
    g.AddEdge(P(kHtml), P(kIr),   Cost(Fidelity::Lossless, 0, 9.0f), "dochtml");
    g.AddEdge(P(kIr),   P(kHtml), Cost(Fidelity::High, 0, 5.0f), "dochtml");

    // WebView2 print pipeline. The one edge that legitimately needs a temp file.
    g.AddEdge(P(kHtml), P(kPdf), Cost(Fidelity::High, 0, 120.0f, 3.0f, /*requires_disk=*/true),
              "pdf");
    // PDFium: rasterising a page is the escape hatch of last resort.
    g.AddEdge(P(kPdf), P(kPng), Cost(Fidelity::Rasterized, 1, 60.0f, 6.0f), "pdf");
    g.AddEdge(P(kPdf), P(kTxt), Cost(Fidelity::TextOnly, 0, 15.0f), "pdf");
    g.AddEdge(P(kPdf), P(kIr),  Cost(Fidelity::Structural, 0, 40.0f, 5.0f), "pdf");

    g.Freeze();
    return g;
}

}  // namespace morph::test
