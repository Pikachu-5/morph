// Morph — running a plan (architecture sections 4, 5 and 11.4).
//
// The planner decides the route; this executes it. Everything else in the engine
// has been building toward this file, and it is small because of that.
//
// TWO THINGS IT DOES THAT ARE NOT OBVIOUS.
//
// FUSING DOCUMENT HOPS. A plan like docx -> IR -> html has two hops, and running
// them separately would mean serialising the IR between them -- an intermediate
// that never leaves the process, for a format that has no serialised form on
// purpose (see doc/pipeline.h). So a reader hop and the writer hop that follows
// it are executed as one in-memory pass. The planner still sees two edges, the
// fidelity contract still aggregates both, and the UI still shows the hop list.
//
// WEIGHTING PROGRESS. Section 11.4: "For multi-hop paths, weight each hop by its
// estimated cost so the bar moves proportionally instead of jumping
// 0 -> 25% -> 25% -> 100%." A four-hop conversion whose last hop is 90% of the
// work must not spend most of its time showing 75%.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "morph/core/doc/pipeline.h"
#include "morph/core/graph.h"
#include "morph/core/planner.h"
#include "morph/core/provider.h"

namespace morph {

// Owns nothing: providers are long-lived singletons in the worker, and an
// executor that owned them would complicate the worker's shutdown order for no
// benefit (the same reasoning as ProviderRegistry).
class Executor {
public:
    // Byte-oriented providers, the section 5 contract.
    void Add(IConversionProvider& provider);

    // Document providers. Registering a reader and a writer with the same id is
    // normal and expected -- the Markdown provider is both.
    void Add(doc::IDocumentReader& reader);
    void Add(doc::IDocumentWriter& writer);

    // Declares every registered edge into `graph` and freezes it.
    void BuildGraph(FormatGraph& graph) const;

    // Identifies an input. Document readers are consulted before byte providers,
    // and both before the extension-dependent fallbacks, so that a .docx is
    // recognised by its container rather than by its name.
    [[nodiscard]] std::optional<MediaProfile> Probe(ISource& source, const PathHint& hint) const;

    // Runs a plan end to end. `source` is positioned at zero on entry.
    //
    // The sink is committed by the provider that writes the final hop, so a
    // failure anywhere leaves the destination untouched -- which is what makes
    // section 11.3's "cancellation never leaves a half-written file" true for
    // multi-hop routes and not just single ones.
    [[nodiscard]] Result Execute(const FormatGraph& graph, const Plan& plan,
                                 ConvertContext& context, ISource& source, ISink& sink);

    // Intermediates between byte hops are held in memory. Section 4.3 forbids
    // spilling every hop to disk, and for the document and image families the
    // largest intermediate is a rendered HTML document or a decoded bitmap --
    // both bounded by the job's memory budget (section 11.2).
    //
    // This becomes wrong the moment an audio/video hop needs an intermediate,
    // because a 4 GB remux does not fit anywhere. Those providers stream through
    // FFmpeg in a single hop today; the day one of them needs two, this has to
    // become a paired producer/consumer rather than a buffer.
    static constexpr std::uint64_t kMaxIntermediateBytes = 512ull * 1024 * 1024;

private:
    [[nodiscard]] doc::IDocumentReader* FindReader(std::string_view providerId,
                                                   const MediaProfile& from) const;
    [[nodiscard]] doc::IDocumentWriter* FindWriter(std::string_view providerId,
                                                   const MediaProfile& to) const;
    [[nodiscard]] IConversionProvider* FindProvider(std::string_view providerId) const;

    std::vector<IConversionProvider*> providers_;
    std::vector<doc::IDocumentReader*> readers_;
    std::vector<doc::IDocumentWriter*> writers_;
};

// Scales a provider's 0..1 progress into its slice of the whole task, so a hop
// that is 90% of the estimated work occupies 90% of the bar (section 11.4).
class HopProgress final : public IProgressSink {
public:
    HopProgress(IProgressSink& downstream, double start, double span)
        : downstream_(&downstream), start_(start), span_(span) {}

    void Report(double fraction, std::uint64_t bytesWritten) override {
        const double clamped = fraction < 0.0 ? 0.0 : (fraction > 1.0 ? 1.0 : fraction);
        downstream_->Report(start_ + clamped * span_, bytesWritten);
    }

private:
    IProgressSink* downstream_;
    double start_;
    double span_;
};

}  // namespace morph
