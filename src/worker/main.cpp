// morph.worker.exe
//
// Section 3.1: "Owns all parsing, decoding, and encoding. Assumed
// hostile-input-facing and treated as expendable. No network capability, capped
// by a job object." Per job batch, low integrity, and the one process that ever
// opens a file the user did not vouch for.
//
// Section 2 also notes: "A CLI falls out of the worker for free and ships
// unadvertised." That is what this file mostly is today. The broker-driven mode
// arrives with the broker; the surface it will use -- Providers, Executor, the
// planner -- is already what the CLI drives, so neither grows a second path.

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include "morph/core/executor.h"
#include "morph/core/extensions.h"
#include "morph/core/planner.h"
#include "morph/core/stream.h"
#include "morph/core/temp_store.h"
#include "providers.h"
#include "serve.h"

namespace {

using namespace morph;

// --- Process hardening (section 15.1) ----------------------------------------

// "Applied in the worker before any user data is touched."
//
// These are best-effort by design: every one of them can fail on an older build
// or under a debugger, and none of them is load-bearing on its own. The job
// object the broker puts around this process is the backstop that always holds.
void HardenProcess() {
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamicCode{};
    dynamicCode.ProhibitDynamicCode = 1;
    ::SetProcessMitigationPolicy(ProcessDynamicCodePolicy, &dynamicCode, sizeof(dynamicCode));

    PROCESS_MITIGATION_IMAGE_LOAD_POLICY imageLoad{};
    imageLoad.NoRemoteImages = 1;
    imageLoad.NoLowMandatoryLabelImages = 1;
    ::SetProcessMitigationPolicy(ProcessImageLoadPolicy, &imageLoad, sizeof(imageLoad));

    PROCESS_MITIGATION_ASLR_POLICY aslr{};
    aslr.EnableHighEntropy = 1;
    aslr.EnableForceRelocateImages = 1;
    ::SetProcessMitigationPolicy(ProcessASLRPolicy, &aslr, sizeof(aslr));

    // NOT set: ProcessSignaturePolicy with MicrosoftSignedOnly. The architecture
    // lists it, and it would block our own unsigned developer builds and the
    // PDFium DLL alike. It belongs in the packaged build, gated on the package
    // identity being present, rather than here where it breaks every debug run.
}

// --- Output ------------------------------------------------------------------

void Print(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
}

void PrintLine(std::string_view text = "") {
    Print(text);
    std::fputc('\n', stdout);
}

void PrintError(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fputc('\n', stderr);
}

std::string Narrow(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                             static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                             nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(),
                          needed, nullptr, nullptr);
    return out;
}

// --- Plumbing the CLI needs --------------------------------------------------

class ConsoleProgress final : public IProgressSink {
public:
    explicit ConsoleProgress(bool quiet) : quiet_(quiet) {}

    void Report(double fraction, std::uint64_t /*bytesWritten*/) override {
        if (quiet_) return;
        const int percent = static_cast<int>(fraction * 100.0 + 0.5);
        // Only redraw when the number actually changes: a provider reporting at
        // 10 Hz would otherwise repaint the line hundreds of times.
        if (percent == lastPercent_) return;
        lastPercent_ = percent;
        std::fprintf(stderr, "\r  %3d%%", percent);
        std::fflush(stderr);
    }

    void Finish() {
        if (!quiet_ && lastPercent_ >= 0) std::fprintf(stderr, "\r      \r");
    }

private:
    bool quiet_;
    int lastPercent_ = -1;
};

struct Options {
    std::string command;
    std::filesystem::path input;
    std::filesystem::path output;
    std::string targetProfile;
    OptionBag bag;
    bool quiet = false;
    bool listOnly = false;
};

// The target profile from the output file's extension. The INPUT is never
// identified this way -- section 5 is explicit that Probe must not trust an
// extension -- but the output has no bytes to inspect and its name is the only
// statement of intent the user has made. The table is shared with the broker,
// which resolves output names before any byte is written (extensions.h).
std::string TargetFromExtension(const std::filesystem::path& path) {
    return std::string(ProfileForExtension(path.extension().string()));
}

bool ParseOption(std::string_view text, OptionBag& into) {
    const auto equals = text.find('=');
    if (equals == std::string_view::npos || equals == 0) return false;

    const std::string key(text.substr(0, equals));
    const std::string value(text.substr(equals + 1));

    if (value == "true") {
        into.Set(key, true);
    } else if (value == "false") {
        into.Set(key, false);
    } else {
        // An integer if it parses cleanly as one, a double if it parses cleanly
        // as that, otherwise a string. Guessing wrong here is harmless: an
        // option read as the wrong kind falls back to its default rather than
        // being misapplied (see OptionBag).
        try {
            std::size_t consumed = 0;
            const long long asInt = std::stoll(value, &consumed);
            if (consumed == value.size()) {
                into.Set(key, static_cast<std::int64_t>(asInt));
                return true;
            }
            const double asDouble = std::stod(value, &consumed);
            if (consumed == value.size()) {
                into.Set(key, asDouble);
                return true;
            }
        } catch (...) {
            // Not a number. Fall through to string.
        }
        into.Set(key, value);
    }
    return true;
}

void PrintUsage() {
    PrintLine("morph.worker - the Morph conversion worker");
    PrintLine();
    PrintLine("This is an unadvertised command-line surface over the same engine the");
    PrintLine("Morph app and the Explorer menu use. It is supported, not secret, but it");
    PrintLine("is not the product.");
    PrintLine();
    PrintLine("USAGE");
    PrintLine("  morph.worker convert <input> <output> [--target <profile>] [-o key=value]...");
    PrintLine("  morph.worker inspect <input>");
    PrintLine("  morph.worker targets <input>");
    PrintLine("  morph.worker formats");
    PrintLine();
    PrintLine("OPTIONS");
    PrintLine("  --target <profile>   Target media profile. Defaults to the output extension.");
    PrintLine("  -o, --option k=v     A conversion option. May be repeated.");
    PrintLine("  -q, --quiet          No progress output.");
    PrintLine();
    PrintLine("EXAMPLES");
    PrintLine("  morph.worker convert report.docx report.pdf");
    PrintLine("  morph.worker convert notes.md notes.docx");
    PrintLine("  morph.worker convert paper.pdf paper.md");
    PrintLine("  morph.worker targets report.docx");
}

// --- Commands ----------------------------------------------------------------

int Inspect(worker::Providers& providers, const Options& options) {
    FileSource source(options.input);
    PathHint hint;
    hint.filename = options.input.filename().string();
    hint.extension = options.input.extension().string();
    std::transform(hint.extension.begin(), hint.extension.end(), hint.extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const std::optional<MediaProfile> profile = providers.executor().Probe(source, hint);
    if (!profile) {
        PrintError("Could not identify " + hint.filename + ".");
        return 2;
    }
    PrintLine(profile->Id());
    return 0;
}

int Targets(worker::Providers& providers, const Options& options) {
    FileSource source(options.input);
    PathHint hint;
    hint.filename = options.input.filename().string();
    hint.extension = options.input.extension().string();
    std::transform(hint.extension.begin(), hint.extension.end(), hint.extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const std::optional<MediaProfile> profile = providers.executor().Probe(source, hint);
    if (!profile) {
        PrintError("Could not identify " + hint.filename + ".");
        return 2;
    }

    PlanRequest request;
    request.source = profile->Id();
    if (const auto size = source.Size()) {
        request.inputSizeMiB = static_cast<double>(*size) / (1024.0 * 1024.0);
    }

    PrintLine(profile->Id() + " can convert to:");
    for (const auto& [node, plan] : FindAllTargets(providers.graph(), request)) {
        char line[512];
        std::snprintf(line, sizeof(line), "  %-28s %-12s %u hop%s  ~%.0f ms%s",
                      providers.graph().Node(node).Id().c_str(),
                      std::string(Name(plan.fidelity())).c_str(), plan.cost.hopCount,
                      plan.cost.hopCount == 1 ? " " : "s", plan.cost.estimatedMs,
                      plan.requiresDisk ? "  (uses temp space)" : "");
        PrintLine(line);
    }
    return 0;
}

int Formats(worker::Providers& providers) {
    PrintLine("Formats in the graph:");
    for (const MediaProfile& profile : providers.graph().Nodes()) {
        PrintLine("  " + profile.Id());
    }
    char line[128];
    std::snprintf(line, sizeof(line), "\n%zu formats, %zu conversions.",
                  providers.graph().NodeCount(), providers.graph().EdgeCount());
    PrintLine(line);
    return 0;
}

int Convert(worker::Providers& providers, const Options& options) {
    PathHint hint;
    hint.filename = options.input.filename().string();
    hint.extension = options.input.extension().string();
    std::transform(hint.extension.begin(), hint.extension.end(), hint.extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    FileSource source(options.input);

    const std::optional<MediaProfile> from = providers.executor().Probe(source, hint);
    if (!from) {
        PrintError("Could not identify " + hint.filename + ". Morph reads its own formats by "
                   "content, not by extension, so a renamed file is not the problem.");
        return 2;
    }

    std::string target = options.targetProfile;
    if (target.empty()) target = TargetFromExtension(options.output);
    if (target.empty()) {
        PrintError("Could not tell what to convert to from '" +
                   options.output.filename().string() +
                   "'. Pass --target with a media profile, or use a known extension.");
        return 2;
    }

    PlanRequest request;
    request.source = from->Id();
    request.target = target;
    if (const auto size = source.Size()) {
        request.inputSizeMiB = static_cast<double>(*size) / (1024.0 * 1024.0);
    }

    const PlanResult planned = FindPlan(providers.graph(), request);
    if (!planned) {
        switch (planned.error) {
            case PlanError::kUnknownSource:
                PrintError("Morph does not handle " + from->Id() + " files.");
                break;
            case PlanError::kUnknownTarget:
                PrintError("Morph does not produce " + target + " files.");
                break;
            case PlanError::kNoRoute:
                PrintError("There is no way to convert " + from->Id() + " to " + target + ".");
                break;
        }
        return 3;
    }

    const Plan& plan = *planned.plan;

    if (!options.quiet) {
        std::string route = from->Id();
        for (const PlanHop& hop : plan.hops) {
            route += " -> " + providers.graph().Node(hop.to).Id();
        }
        PrintLine(route);
        // The fidelity contract, before the work starts. Section 8.8: the user
        // is told what they are about to lose.
        PrintLine(std::string("fidelity: ") + std::string(Name(plan.fidelity())) + " - " +
                  std::string(Explain(plan.fidelity())));
    }
    if (options.listOnly) return 0;

    std::error_code ec;
    if (options.output.has_parent_path()) {
        std::filesystem::create_directories(options.output.parent_path(), ec);
    }

    ConsoleProgress progress(options.quiet);
    ScopedTempStore temp("cli");
    std::stop_source stop;

    ConvertContext context{options.bag, progress, stop.get_token(), temp,
                           providers.capabilities()};

    FileSink sink(options.output);
    const Result result =
        providers.executor().Execute(providers.graph(), plan, context, source, sink);
    progress.Finish();

    if (!result) {
        sink.Abandon();
        PrintError(result.message().empty() ? "The conversion failed." : result.message());
        return 4;
    }

    if (!options.quiet) {
        PrintLine("wrote " + options.output.string());
        // The notes ACTUALLY incurred, which is a different thing from the
        // rating the plan predicted.
        for (const FidelityNote& note : result.notes()) {
            PrintLine("  note: " + note.detail);
        }
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    HardenProcess();

    // UTF-8 on the console, so a converted filename with an accent in it is not
    // mangled on the way out.
    ::SetConsoleOutputCP(CP_UTF8);

    Options options;
    std::vector<std::string> positional;
    // Set by --serve, which the broker passes and a person never does. It is not
    // in the usage text for that reason: it is a private calling convention
    // between two of our own processes, not a command.
    std::wstring servePipe;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = Narrow(argv[i]);

        if (argument == "--serve" && i + 1 < argc) {
            servePipe = argv[++i];
        } else if (argument == "-q" || argument == "--quiet") {
            options.quiet = true;
        } else if (argument == "--plan") {
            options.listOnly = true;
        } else if (argument == "--target" && i + 1 < argc) {
            options.targetProfile = Narrow(argv[++i]);
        } else if ((argument == "-o" || argument == "--option") && i + 1 < argc) {
            if (!ParseOption(Narrow(argv[++i]), options.bag)) {
                PrintError("Options are written key=value.");
                return 1;
            }
        } else if (argument == "-h" || argument == "--help") {
            PrintUsage();
            return 0;
        } else if (argument.starts_with("-")) {
            PrintError("Unknown option: " + argument);
            return 1;
        } else {
            positional.push_back(argument);
        }
    }

    if (!servePipe.empty()) {
        try {
            worker::Providers providers;
            providers.ProbeCapabilities();
            return worker::Serve(providers, servePipe);
        } catch (const std::exception& error) {
            PrintError(std::string("Internal error: ") + error.what());
            return 5;
        }
    }

    if (positional.empty()) {
        PrintUsage();
        return 1;
    }

    options.command = positional[0];
    if (positional.size() > 1) options.input = std::filesystem::path(argv[0]).empty()
                                                   ? positional[1]
                                                   : positional[1];
    if (positional.size() > 2) options.output = positional[2];

    // Every command below except "formats" needs a readable input, and opening
    // it here means one error message rather than four.
    if (options.command != "formats") {
        if (options.input.empty()) {
            PrintError("Which file? " + options.command + " needs an input path.");
            return 1;
        }
        std::error_code ec;
        if (!std::filesystem::exists(options.input, ec)) {
            PrintError(options.input.string() + " does not exist.");
            return 2;
        }
    }

    try {
        worker::Providers providers;
        providers.ProbeCapabilities();

        if (options.command == "formats") return Formats(providers);
        if (options.command == "inspect") return Inspect(providers, options);
        if (options.command == "targets") return Targets(providers, options);
        if (options.command == "convert") {
            if (options.output.empty()) {
                PrintError("convert needs an output path.");
                return 1;
            }
            return Convert(providers, options);
        }

        PrintError("Unknown command: " + options.command);
        PrintUsage();
        return 1;
    } catch (const std::exception& error) {
        // The worker is expendable (section 3.1), but it should still say what
        // happened rather than dying silently and leaving the broker to guess.
        PrintError(std::string("Internal error: ") + error.what());
        return 5;
    }
}
