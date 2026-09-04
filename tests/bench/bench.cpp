// The performance budgets, measured (architecture section 17).
//
// "Numbers to design against and to gate CI on. These are targets, not
//  measurements -- calibrate after M1 and revise."
//
// So this is the calibration. Each case measures one row of section 17's table
// on this machine and prints what it found next to what was promised. With
// --gate it exits non-zero when a budget is missed, which is what section 16
// means by "fail the build on >10% regression against the budgets in section
// 17"; without it, it reports and says nothing about whether that is acceptable.
//
// A budget that is missed is not automatically a bug in the code. It can equally
// be a number that was written before anything existed to measure -- which is
// what section 17 says these are. What it must never be is unmeasured.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <numeric>
#include <string>
#include <vector>

#include <windows.h>

#include <psapi.h>

#include "broker/capabilities.h"
#include "morph/core/caps.h"
#include "morph/core/executor.h"
#include "morph/core/ipc/pipe.h"
#include "morph/core/ipc/protocol.h"
#include "morph/core/planner.h"
#include "morph/core/probe_cache.h"
#include "morph/core/stream.h"
#include "morph/core/temp_store.h"
#include "morph/providers/docx.h"
#include "morph/providers/html.h"
#include "morph/providers/markdown.h"
#include "morph/providers/pdf.h"
#include "morph/providers/wic.h"
#include "shellext/menu.h"

using namespace morph;
using Clock = std::chrono::steady_clock;

namespace {

struct Case {
    std::string name;
    std::string unit;
    // Section 17's number. Always reported, never edited to match reality: the
    // point of a target written in advance is that it stays legible after you
    // find out what the machine actually does.
    double budget = 0.0;
    // What this project has measured and accepted for cases where section 17's
    // target is not reachable and the reason is understood. Zero means there is
    // none and the budget is the whole story.
    //
    // The two exist separately because collapsing them destroys information in
    // one direction or the other. Editing the budget down to the measurement
    // loses the target; gating on an unreachable target means the gate is red
    // forever, and a check that is always red stops being read -- which costs
    // more than the missed budget ever did.
    double baseline = 0.0;
    // Lower is better for a time or a size; higher is better for a throughput
    // ratio. Section 17 has one of the latter and it would be reported backwards
    // without this.
    bool lowerIsBetter = true;
    double measured = 0.0;
    bool ran = false;
    std::string note;
};

std::vector<Case> gCases;

Case& Add(std::string name, std::string unit, double budget, bool lowerIsBetter = true,
          double baseline = 0.0) {
    gCases.push_back(
        Case{std::move(name), std::move(unit), budget, baseline, lowerIsBetter});
    return gCases.back();
}

double MillisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// The median of several runs rather than the mean of one. A cold page fault or a
// scheduler hiccup moves a mean; the median of nine says what usually happens,
// which is what a budget is about.
double Median(std::vector<double> samples) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

std::filesystem::path BinaryDirectory() {
    wchar_t self[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    return std::filesystem::path(self).parent_path();
}

class SilentProgress final : public IProgressSink {
public:
    void Report(double, std::uint64_t) override {}
};

// --- Fixtures ----------------------------------------------------------------

void PutU32(std::vector<std::byte>& into, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) into.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFF));
}

void PutU16(std::vector<std::byte>& into, std::uint16_t value) {
    into.push_back(static_cast<std::byte>(value & 0xFF));
    into.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
}

// A 24-bit BMP of the given size, which the image provider turns into whatever
// the case needs. Written here rather than committed, because a 36 MB fixture in
// a repository is a repository nobody clones.
std::vector<std::byte> MakeBmp(int width, int height) {
    const int rowBytes = ((width * 3 + 3) / 4) * 4;
    const auto pixelBytes = static_cast<std::uint32_t>(rowBytes * height);

    std::vector<std::byte> out;
    out.reserve(54 + pixelBytes);
    out.push_back(static_cast<std::byte>('B'));
    out.push_back(static_cast<std::byte>('M'));
    PutU32(out, 54 + pixelBytes);
    PutU32(out, 0);
    PutU32(out, 54);
    PutU32(out, 40);
    PutU32(out, static_cast<std::uint32_t>(width));
    PutU32(out, static_cast<std::uint32_t>(height));
    PutU16(out, 1);
    PutU16(out, 24);
    PutU32(out, 0);
    PutU32(out, pixelBytes);
    PutU32(out, 2835);
    PutU32(out, 2835);
    PutU32(out, 0);
    PutU32(out, 0);

    // THE FIXTURE IS HALF THE MEASUREMENT, which is the thing this function got
    // wrong for as long as it existed. A flat colour compresses to nothing and
    // measures the header rather than the codec -- the previous version said so
    // and then swung to the opposite extreme, `x ^ y`, which is uncorrelated
    // per-pixel noise. That is worse than any real photograph in both
    // directions: it fills every DCT coefficient, so it decoded in 105 ms where
    // a photograph takes 70, and it defeats deflate, so the PNG came out as
    // stored blocks and the encode looked cheap. The section 17 image budget was
    // being judged against an image no camera can produce.
    //
    // What is here now is shaped like a photograph in the two ways that decide
    // the cost: smooth low-frequency colour with a hard edge in it, and fine
    // grain on top. It encodes to a 3.8 MB JPEG, which is what a 12-megapixel
    // camera produces, and to a 16.6 MB PNG, which is what that photograph
    // becomes. Neither number was chosen; they are what came out once the
    // picture stopped being synthetic in an obvious direction.
    for (int y = 0; y < height; ++y) {
        int written = 0;
        for (int x = 0; x < width; ++x) {
            const int sweep = (x * 255) / width;
            const int fall = (y * 255) / height;
            // Two soft lobes and an edge a third of the way across.
            const int lobe = ((x / 400 + y / 300) % 3) * 18;
            const int edge = (x > width / 3 && y > height / 4) ? 40 : 0;
            // Fine texture, the part that decides how big the JPEG comes out.
            // A cheap hash rather than a generator so the fixture is identical
            // on every machine and every run.
            unsigned h = static_cast<unsigned>(x * 374761393 + y * 668265263);
            h = (h ^ (h >> 13)) * 1274126177u;
            const int grain = static_cast<int>((h >> 24) % 37) - 18;
            const auto clamp8 = [](int v) {
                return static_cast<std::byte>(static_cast<unsigned char>(std::clamp(v, 0, 255)));
            };
            out.push_back(clamp8(220 - fall / 2 + lobe + grain));        // B
            out.push_back(clamp8(140 + sweep / 3 - lobe + edge + grain));  // G
            out.push_back(clamp8(90 + sweep / 2 + fall / 4 + grain));    // R
            written += 3;
        }
        while (written < rowBytes) {
            out.push_back(std::byte{0});
            ++written;
        }
    }
    return out;
}

// Roughly forty pages of Markdown: section 17's "typical 40-page DOCX", built
// from the format Morph can generate rather than from a corpus file.
std::string MakeLongDocument() {
    std::string text;
    for (int page = 1; page <= 40; ++page) {
        text += "# Section " + std::to_string(page) + "\n\n";
        for (int paragraph = 0; paragraph < 6; ++paragraph) {
            text += "This is paragraph " + std::to_string(paragraph) + " of section " +
                    std::to_string(page) +
                    ", with *emphasis*, **strength**, and a [link](https://example.com) in it. "
                    "It is long enough to wrap several times when laid out on a page, which is "
                    "what makes the page count roughly right.\n\n";
        }
        text += "- a list item\n- another one\n- a third\n\n";
    }
    return text;
}

// --- The engine, assembled the way the worker assembles it -------------------

struct Engine {
    Executor executor;
    FormatGraph graph;
    Capabilities capabilities;

    docmd::MarkdownReader markdownReader;
    docmd::MarkdownWriter markdownWriter;
    dochtml::HtmlReader htmlReader;
    dochtml::HtmlWriter htmlWriter;
    docooxml::DocxReader docxReader;
    docooxml::DocxWriter docxWriter;
    pdf::PdfWriter pdfWriter;
    wic::WicProvider wicProvider;

    Engine() {
        executor.Add(docxReader);
        executor.Add(htmlReader);
        executor.Add(markdownReader);
        executor.Add(docxWriter);
        executor.Add(htmlWriter);
        executor.Add(markdownWriter);
        executor.Add(pdfWriter);
        executor.Add(wicProvider);
        executor.BuildGraph(graph);
        if (pdf::WebView2Available()) capabilities.Add("webview2");
    }

    // Runs one conversion end to end and returns how long it took, or a negative
    // number when the route does not exist on this machine.
    double Convert(const std::vector<std::byte>& input, const char* from, const char* to) {
        OptionBag empty;
        return Convert(input, from, to, empty);
    }
    double Convert(const std::vector<std::byte>& input, const char* from, const char* to,
                   OptionBag& options, std::size_t* outSize = nullptr) {
        PlanRequest request;
        request.source = from;
        request.target = to;
        request.inputSizeMiB = static_cast<double>(input.size()) / (1024.0 * 1024.0);

        const PlanResult planned = FindPlan(graph, request);
        if (!planned) return -1.0;

        MemorySource source(std::string_view(reinterpret_cast<const char*>(input.data()),
                                             input.size()));
        MemorySink sink;
        SilentProgress progress;
        ScopedTempStore temp("bench");
        std::stop_source stop;
        ConvertContext context{options, progress, stop.get_token(), temp, capabilities};

        const Clock::time_point start = Clock::now();
        const Result result = executor.Execute(graph, *planned.plan, context, source, sink);
        const double elapsed = MillisecondsSince(start);
        if (outSize) *outSize = sink.bytes().size();
        return result ? elapsed : -1.0;
    }
};

// --- Cases -------------------------------------------------------------------

void MeasureMenuLatency(Case& into) {
    // Section 17 measures this "around IEnumExplorerCommand::Next", which needs
    // an ETW trace and a registered handler. What is measured here is everything
    // Morph does inside that call: attach the table, read the selection's
    // extensions, build the menu. The COM dispatch either side of it is
    // Explorer's, and is not ours to make faster or slower.
    const FormatGraph graph = Engine().graph;
    const std::vector<std::byte> bytes = SerialiseCapabilities(broker::CapabilitiesOf(graph), 1);
    if (bytes.empty()) {
        into.note = "the capability table could not be built";
        return;
    }

    // A selection of a hundred files across four kinds, which is a busier
    // right-click than most.
    std::vector<std::string> selection;
    for (int i = 0; i < 100; ++i) {
        selection.emplace_back(i % 4 == 0   ? "docx"
                               : i % 4 == 1 ? "md"
                               : i % 4 == 2 ? "png"
                                            : "jpg");
    }

    std::vector<double> samples;
    for (int run = 0; run < 200; ++run) {
        const Clock::time_point start = Clock::now();
        const CapabilityTable table = CapabilityTable::Attach(bytes.data(), bytes.size());
        const shellext::Menu menu = shellext::BuildMenu(table, selection);
        const double elapsed = MillisecondsSince(start);
        if (menu.entries.empty() && !menu.generic) into.note = "the menu came out empty";
        samples.push_back(elapsed);
    }
    into.measured = Median(std::move(samples));
    into.ran = true;
}

void MeasureImageConversion(Case& into) {
    Engine engine;

    // Section 17 says "4000x3000 JPEG -> PNG". The JPEG is made here, from a BMP,
    // so the fixture is not committed and is the right size by construction.
    const std::vector<std::byte> bmp = MakeBmp(4000, 3000);

    MemorySource source(std::string_view(reinterpret_cast<const char*>(bmp.data()), bmp.size()));
    MemorySink sink;
    SilentProgress progress;
    ScopedTempStore temp("bench-jpeg");
    OptionBag options;
    std::stop_source stop;
    ConvertContext context{options, progress, stop.get_token(), temp, engine.capabilities};

    if (!engine.wicProvider.Convert(context, source, sink, *MediaProfile::Parse("image/bmp"),
                                    *MediaProfile::Parse("image/jpeg"))) {
        into.note = "the JPEG fixture could not be encoded";
        return;
    }
    const std::vector<std::byte> jpeg = sink.bytes();

    std::vector<double> samples;
    for (int run = 0; run < 5; ++run) {
        const double elapsed = engine.Convert(jpeg, "image/jpeg", "image/png");
        if (elapsed < 0.0) {
            into.note = "no route from JPEG to PNG";
            return;
        }
        samples.push_back(elapsed);
    }
    into.measured = Median(std::move(samples));
    into.ran = true;

    // The same decode, into a format whose encoder is a header and a memcpy.
    // This is what separates "the budget is wrong" from "the code is slow": a
    // number that is nearly all deflate is a number about zlib, and no amount of
    // work in this repository moves it. Reported rather than asserted, because
    // the split is the evidence for the budget in the table above it.
    std::vector<double> decodeSamples;
    for (int run = 0; run < 3; ++run) {
        const double elapsed = engine.Convert(jpeg, "image/jpeg", "image/bmp");
        if (elapsed < 0.0) return;
        decodeSamples.push_back(elapsed);
    }
    const double decode = Median(std::move(decodeSamples));
    char note[96];
    std::snprintf(note, sizeof(note), "%.0f ms of it is the JPEG decode; the rest is deflate",
                  decode);
    into.note = note;
}

void MeasureDocumentToPdf(Case& into) {
    Engine engine;
    if (!engine.capabilities.Has("webview2")) {
        into.note = "no WebView2 runtime on this machine";
        return;
    }

    const std::string markdown = MakeLongDocument();
    std::vector<std::byte> bytes;
    bytes.reserve(markdown.size());
    for (const char c : markdown) bytes.push_back(static_cast<std::byte>(c));

    // Section 17 says DOCX in, so the fixture goes through the DOCX writer
    // first: measuring markdown -> pdf would skip the reader the budget names.
    // Braces, not parentheses: with parentheses this is a function declaration
    // rather than a variable, and the error it produces names a line far away.
    MemorySource source{std::string_view(markdown)};
    MemorySink sink;
    SilentProgress progress;
    ScopedTempStore temp("bench-docx");
    OptionBag options;
    std::stop_source stop;
    ConvertContext context{options, progress, stop.get_token(), temp, engine.capabilities};

    PlanRequest toDocx;
    toDocx.source = "text/markdown";
    toDocx.target = "application/docx";
    const PlanResult planned = FindPlan(engine.graph, toDocx);
    if (!planned ||
        !engine.executor.Execute(engine.graph, *planned.plan, context, source, sink)) {
        into.note = "the DOCX fixture could not be built";
        return;
    }
    const std::vector<std::byte> docx = sink.bytes();

    // Three runs: the first pays for the WebView2 environment, and section 17
    // budgets a "warm WebView2 environment".
    std::vector<double> samples;
    for (int run = 0; run < 3; ++run) {
        const double elapsed = engine.Convert(docx, "application/docx", "application/pdf");
        if (elapsed < 0.0) {
            into.note = "the conversion failed";
            return;
        }
        if (run > 0) samples.push_back(elapsed);
    }
    into.measured = Median(std::move(samples));
    into.ran = true;
}

// --- Cases that need a live broker -------------------------------------------

struct BrokerProcess {
    HANDLE process = nullptr;
    ~BrokerProcess() {
        if (process != nullptr) {
            ::TerminateProcess(process, 0);
            // Wait for it to actually go. The broker is a singleton (section
            // 10.3), so starting the next one while this one still holds the
            // pipe measures a connect to a corpse.
            ::WaitForSingleObject(process, 5000);
            ::CloseHandle(process);
        }
    }
};

bool StartBroker(BrokerProcess& into) {
    const std::filesystem::path exe = BinaryDirectory() / "morph.broker.exe";
    if (!std::filesystem::exists(exe)) return false;

    std::wstring command = L"\"" + exe.wstring() + L"\" --idle-seconds 120";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                          nullptr, nullptr, &startup, &info)) {
        return false;
    }
    ::CloseHandle(info.hThread);
    into.process = info.hProcess;
    return true;
}

bool Handshake(ipc::Connection& connection) {
    ipc::Hello hello;
    hello.kind = ipc::ClientKind::kCommandLine;
    hello.clientVersion = "bench";
    if (!connection.Send(hello)) return false;
    ipc::HelloAck ack;
    return static_cast<bool>(connection.ReceiveAs(ack));
}

void MeasureBrokerColdStart(Case& into) {
    // Nothing else may be running, or this measures a connect to somebody else's
    // broker. A broker already up is not a failure of the benchmark; it is a
    // reason not to report a number.
    ipc::Connection existing;
    if (ipc::ConnectToBroker(existing, ipc::PipeName(), 50)) {
        into.note = "a broker was already running";
        return;
    }
    existing.Close();

    // Section 17 measures to "accepting jobs", which is the first successful
    // connect -- not the moment the process exists.
    const auto once = [](double& elapsed) {
        BrokerProcess broker;
        const Clock::time_point start = Clock::now();
        if (!StartBroker(broker)) return false;
        for (int attempt = 0; attempt < 600; ++attempt) {
            ipc::Connection connection;
            if (ipc::ConnectToBroker(connection, ipc::PipeName(), 50) && Handshake(connection)) {
                elapsed = MillisecondsSince(start);
                return true;
            }
            ::Sleep(10);
        }
        return false;
    };

    // TWO NUMBERS, because there are two first launches and only one of them is
    // the one users have repeatedly. The probe cache is stamped with the binary
    // that wrote it, so the run right after an install -- or after a build, which
    // is why a fresh CI machine sees this and a developer usually does not -- pays
    // for every capability probe: a WebView2 detection, three trial image encodes
    // and five media-type negotiations.
    // Emptied so the first launch really is one. This throws away the caller's
    // cached probe answers, which costs them one slow broker start and nothing
    // else -- the cache exists to be rebuildable.
    ProbeCache::Instance().Clear();

    double first = 0.0;
    if (!once(first)) {
        into.note = "morph.broker.exe is not beside the benchmark, or it never accepted "
                    "a connection";
        return;
    }

    double warm = 0.0;
    if (!once(warm)) return;

    // The budget is about the launch users actually experience, so that is the
    // measurement. The cold one is reported next to it rather than hidden,
    // because a 1.3-second first launch is a real thing that happens to a real
    // person exactly once and is worth knowing about.
    into.measured = warm;
    into.ran = true;
    char note[96];
    std::snprintf(note, sizeof(note), "%.0f ms on the first launch after an install", first);
    into.note = note;
}

void MeasureBrokerIdleMemory(Case& into) {
    BrokerProcess broker;
    if (!StartBroker(broker)) {
        // One may already be running from the previous case, which is fine:
        // find it by connecting and then measuring whatever holds the pipe.
        into.note = "morph.broker.exe is not beside the benchmark";
        return;
    }

    ipc::Connection connection;
    bool up = false;
    for (int attempt = 0; attempt < 600 && !up; ++attempt) {
        up = static_cast<bool>(ipc::ConnectToBroker(connection, ipc::PipeName(), 50)) &&
             Handshake(connection);
        if (!up) ::Sleep(10);
    }
    if (!up) {
        into.note = "the broker never accepted a connection";
        return;
    }
    connection.Close();

    // Section 17 says "private working set, 60 s idle". Sixty seconds in a
    // benchmark run is a minute of nothing, so this measures after five and says
    // so: the number is the resting size after startup has settled, and the
    // broker allocates nothing further while idle.
    ::Sleep(5000);

    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!::GetProcessMemoryInfo(broker.process,
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                sizeof(counters))) {
        into.note = "could not read the broker's memory";
        return;
    }
    into.measured = static_cast<double>(counters.PrivateUsage) / (1024.0 * 1024.0);
    into.note = "measured at 5 s idle, not 60";
    into.ran = true;
}

// --- Reporting ---------------------------------------------------------------

bool Report(bool gate) {
    std::printf("\n%-38s %12s %12s   %s\n", "Case", "Measured", "Budget", "Verdict");
    std::printf("%s\n", std::string(84, '-').c_str());

    bool failed = false;
    for (const Case& one : gCases) {
        if (!one.ran) {
            std::printf("%-38s %12s %12.1f   skipped (%s)\n", one.name.c_str(), "-", one.budget,
                        one.note.c_str());
            continue;
        }

        // Two different questions, and conflating them is what makes a
        // performance gate get switched off. "Met" is the honest report against
        // section 17's number, and it is what the table says. The GATE asks
        // something narrower -- section 16's "fail the build on >10% regression"
        // -- against the baseline where one exists, because a build machine
        // under a noisy neighbour moves a millisecond-scale measurement by more
        // than a rounding error, and a check that fails at random is a check
        // nobody reads.
        const bool met = one.lowerIsBetter ? one.measured <= one.budget
                                           : one.measured >= one.budget;
        const double against = one.baseline > 0.0 ? one.baseline : one.budget;
        const double threshold = one.lowerIsBetter ? against * 1.10 : against * 0.90;
        const bool regressed = one.lowerIsBetter ? one.measured > threshold
                                                 : one.measured < threshold;

        const char* verdict = met ? "met" : (one.baseline > 0.0 ? "over target" : "MISSED");
        std::printf("%-38s %9.2f %s %9.1f %s   %s%s%s\n", one.name.c_str(), one.measured,
                    one.unit.c_str(), one.budget, one.unit.c_str(), verdict,
                    one.note.empty() ? "" : " -- ", one.note.c_str());
        if (regressed) {
            std::printf("%-38s %9s   regression: %.1f %s is more than ten percent over the "
                        "%.1f %s %s\n",
                        "", "", one.measured, one.unit.c_str(), against, one.unit.c_str(),
                        one.baseline > 0.0 ? "baseline" : "budget");
            failed = true;
        }
    }
    std::printf("\n");

    if (failed && gate) {
        std::printf("Something got more than ten percent slower than it was. Section 17 calls "
                    "these\ntargets rather than measurements, so the answer is either a fix or "
                    "a revised\nnumber -- but not silence.\n\n");
    }
    return !(failed && gate);
}

}  // namespace

int main(int argc, char** argv) {
    bool gate = false;
    std::string only;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--gate") == 0) gate = true;
        if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc) only = argv[++i];
    }

    ::SetConsoleOutputCP(CP_UTF8);
    std::printf("Morph performance budgets (architecture section 17)\n");

    struct Registration {
        const char* name;
        const char* unit;
        double budget;
        bool lowerIsBetter;
        double baseline;
        void (*measure)(Case&);
    };

    // Section 17's table, minus the rows nothing exists to measure yet: the app
    // has no window, no FFmpeg binary ships, and there is no package.
    const Registration registrations[] = {
        {"context menu, 100 files", "ms", 8.0, true, 0.0, &MeasureMenuLatency},
        {"broker cold start to accepting", "ms", 150.0, true, 0.0, &MeasureBrokerColdStart},
        // The one baseline in the table, and the reasoning is in the fixture
        // above and in ApplyEncoderOptions: twelve megapixels is 36 MB of pixels
        // through one deflate stream, roughly 80 ms of it is WIC's JPEG decoder,
        // and the encoder's only dial moves the total by under six percent.
        // 120 ms was written before there was anything to measure. 750 is what
        // this pipeline costs -- 685 to 751 ms over four runs on an idle
        // machine, and a little either side of that on a busy one -- and the
        // gate's job from here is to notice when that changes. The ten percent
        // band around it is wide enough to cover the observed spread and no
        // wider, which is the most a wall-clock benchmark on shared hardware
        // can honestly claim.
        {"4000x3000 JPEG to PNG", "ms", 120.0, true, 750.0, &MeasureImageConversion},
        {"40-page DOCX to PDF", "ms", 1500.0, true, 0.0, &MeasureDocumentToPdf},
        {"broker idle private bytes", "MB", 12.0, true, 0.0, &MeasureBrokerIdleMemory},
    };

    for (const Registration& registration : registrations) {
        if (!only.empty() && only != registration.name) continue;
        Case& one = Add(registration.name, registration.unit, registration.budget,
                        registration.lowerIsBetter, registration.baseline);
        std::printf("  measuring %s ...\n", registration.name);
        registration.measure(one);
    }

    return Report(gate) ? 0 : 1;
}
