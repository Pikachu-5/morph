// Morph — the provider contract (architecture section 5).
//
// "Every conversion capability, including the built-in ones, is a provider.
//  There is no privileged path. The interface is deliberately narrow so that a
//  provider can be moved across a process boundary later without touching its
//  implementation."
//
// Two rules from section 5 that are easy to violate and expensive to fix later:
//
//   1. Probe must not trust the file extension, and must not read more than
//      8 KiB. It runs on every file in a selection and a user can select ten
//      thousand files.
//   2. Convert must honour the stop token at a granularity of at most 100 ms of
//      work. Anything coarser makes cancellation feel broken regardless of what
//      the UI does.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "morph/core/edge.h"
#include "morph/core/graph.h"
#include "morph/core/media_profile.h"
#include "morph/core/options.h"
#include "morph/core/result.h"
#include "morph/core/stream.h"

namespace morph {

// Everything a provider is allowed to know about where the bytes came from.
// A hint, never a fact: section 15 assumes every input file is hostile, and an
// attacker controls the extension. Probe may use this to order its guesses and
// must not use it to skip verification.
struct PathHint {
    std::string extension;  // lowercased, with the dot: ".docx"
    std::string filename;   // for error messages only
};

// Which optional codecs and OS components are actually present, probed once at
// worker startup (section 6.2). Graph edges belonging to a missing capability
// are removed rather than left to fail mid-job.
class Capabilities {
public:
    void Add(std::string id) { present_.push_back(std::move(id)); }
    [[nodiscard]] bool Has(std::string_view id) const;

    [[nodiscard]] const std::vector<std::string>& All() const noexcept { return present_; }

private:
    std::vector<std::string> present_;
};

// Providers report raw progress freely; the broker coalesces per job to 10 Hz
// before fanning out (section 11.4).
struct IProgressSink {
    virtual ~IProgressSink() = default;

    // `fraction` is 0..1 within the current hop. The broker weights each hop by
    // its estimated cost so the bar moves proportionally instead of jumping
    // 0 -> 25% -> 25% -> 100%.
    virtual void Report(double fraction, std::uint64_t bytesWritten) = 0;
};

// Scoped temp files, auto-reaped when the worker process exits (section 5).
// Only an edge that declares requiresDisk should be reaching for this.
struct ITempStore {
    virtual ~ITempStore() = default;

    // `hint` is used only to give the file a recognisable extension, which some
    // OS components (WebView2, Media Foundation) key off.
    [[nodiscard]] virtual std::filesystem::path Acquire(std::string_view hint) = 0;
};

struct ConvertContext {
    const OptionBag& options;      // resolved preset + per-job overrides
    IProgressSink& progress;       // coalesced upstream, see section 11.4
    std::stop_token cancel;        // check at <= 100 ms granularity
    ITempStore& temp;              // scoped, auto-reaped on process exit
    const Capabilities& host;      // codec availability probed at startup

    // Convenience so the 100 ms rule reads the same in every provider.
    [[nodiscard]] bool cancelled() const noexcept { return cancel.stop_requested(); }
};

struct IConversionProvider {
    virtual ~IConversionProvider() = default;

    [[nodiscard]] virtual std::string_view Id() const noexcept = 0;

    // Declared once at registration; the broker builds the graph from these.
    [[nodiscard]] virtual std::vector<EdgeDecl> EnumerateEdges() const = 0;

    // Cheap sniff: magic bytes plus a light structural check. Never a full
    // parse. Returns nullopt when this provider does not recognise the input,
    // which is the common case and must not be expensive.
    //
    // The source is positioned at zero on entry and the provider may read at
    // most 8 KiB. It must not assume the source is seekable.
    [[nodiscard]] virtual std::optional<MediaProfile> Probe(ISource& source,
                                                            const PathHint& hint) const = 0;

    // Does the work. `from` and `to` are the exact profiles the planner chose,
    // so a provider that advertises several edges knows which one it is on
    // without re-sniffing.
    [[nodiscard]] virtual Result Convert(ConvertContext& context, ISource& source, ISink& sink,
                                         const MediaProfile& from, const MediaProfile& to) = 0;
};

// v1 providers are statically linked into the worker and registered from a
// table. The interface is designed so that v2 can load out-of-tree providers
// from an MSIX optional package without changing any provider code -- but
// section 5.1 is explicit that dynamic loading must not be built in v1.
class ProviderRegistry {
public:
    // Ownership stays with the caller: providers are long-lived singletons owned
    // by the worker, and a registry that owned them would make the worker's
    // shutdown order harder than it needs to be.
    void Register(IConversionProvider& provider);

    // Adds every registered provider's declared edges to `graph` and freezes it.
    void BuildGraph(FormatGraph& graph) const;

    // First provider whose Probe recognises the input. Order is registration
    // order, so the table in the worker is also the precedence list.
    [[nodiscard]] std::optional<MediaProfile> Probe(ISource& source, const PathHint& hint) const;

    [[nodiscard]] IConversionProvider* Find(std::string_view id) const;

    [[nodiscard]] const std::vector<IConversionProvider*>& All() const noexcept {
        return providers_;
    }

private:
    std::vector<IConversionProvider*> providers_;
};

}  // namespace morph
