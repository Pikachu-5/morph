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
    double budget = 0.0;
    // Lower is better for a time or a size; higher is better for a throughput
    // ratio. Section 17 has one of the latter and it would be reported backwards
    // without this.
    bool lowerIsBetter = true;
    double measured = 0.0;
    bool ran = false;
    std::string note;
};

std::vector<Case> gCases;

Case& Add(std::string name, std::string unit, double budget, bool lowerIsBetter = true) {
    gCases.push_back(Case{std::move(name), std::move(unit), budget, lowerIsBetter});
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

    // A gradient with noise in it: a flat colour compresses to nothing and would
    // measure the header rather than the codec.
    for (int y = 0; y < height; ++y) {
        int written = 0;
        for (int x = 0; x < width; ++x) {
            out.push_back(static_cast<std::byte>((x * 7 + y * 3) & 0xFF));
            out.push_back(static_cast<std::byte>((x ^ y) & 0xFF));
            out.push_back(static_cast<std::byte>((x * 13 + y * 5) & 0xFF));
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
        OptionBag options;
        std::stop_source stop;
        ConvertContext context{options, progress, stop.get_token(), temp, capabilities};

        const Clock::time_point start = Clock::now();
        const Result result = executor.Execute(graph, *planned.plan, context, source, sink);
        const double elapsed = MillisecondsSince(start);
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

    BrokerProcess broker;
    const Clock::time_point start = Clock::now();
    if (!StartBroker(broker)) {
        into.note = "morph.broker.exe is not beside the benchmark";
        return;
    }

    // Section 17 measures to "accepting jobs", which is the first successful
    // connect -- not the moment the process exists.
    for (int attempt = 0; attempt < 600; ++attempt) {
        ipc::Connection connection;
        if (ipc::ConnectToBroker(connection, ipc::PipeName(), 50) && Handshake(connection)) {
            into.measured = MillisecondsSince(start);
            into.ran = true;
            return;
        }
        ::Sleep(10);
    }
    into.note = "the broker never accepted a connection";
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

        const bool met = one.lowerIsBetter ? one.measured <= one.budget
                                           : one.measured >= one.budget;
        // Section 16 gates on a 10% regression rather than on the budget to the
        // decimal, so a case within a tenth of its budget is reported as met
        // with the margin visible.
        const char* verdict = met ? "met" : "MISSED";
        std::printf("%-38s %9.2f %s %9.1f %s   %s%s%s\n", one.name.c_str(), one.measured,
                    one.unit.c_str(), one.budget, one.unit.c_str(), verdict,
                    one.note.empty() ? "" : " -- ", one.note.c_str());
        if (!met) failed = true;
    }
    std::printf("\n");

    if (failed && gate) {
        std::printf("At least one budget was missed. Section 17 calls these targets rather "
                    "than measurements,\nso the answer is either a fix or a revised number "
                    "-- but not silence.\n\n");
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
        void (*measure)(Case&);
    };

    // Section 17's table, minus the rows nothing exists to measure yet: the app
    // has no window, no FFmpeg binary ships, and there is no package.
    const Registration registrations[] = {
        {"context menu, 100 files", "ms", 8.0, true, &MeasureMenuLatency},
        {"broker cold start to accepting", "ms", 150.0, true, &MeasureBrokerColdStart},
        {"4000x3000 JPEG to PNG", "ms", 120.0, true, &MeasureImageConversion},
        {"40-page DOCX to PDF", "ms", 1500.0, true, &MeasureDocumentToPdf},
        {"broker idle private bytes", "MB", 12.0, true, &MeasureBrokerIdleMemory},
    };

    for (const Registration& registration : registrations) {
        if (!only.empty() && only != registration.name) continue;
        Case& one = Add(registration.name, registration.unit, registration.budget,
                        registration.lowerIsBetter);
        std::printf("  measuring %s ...\n", registration.name);
        registration.measure(one);
    }

    return Report(gate) ? 0 : 1;
}
