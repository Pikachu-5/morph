#include "morph/core/executor.h"

#include <algorithm>
#include <numeric>

namespace morph {
namespace {

// A sink that hands its bytes to the next hop. Commit is meaningful only for the
// final destination; an intermediate's Commit just says "the producer finished".
class BufferSink final : public ISink {
public:
    void Write(std::span<const std::byte> bytes) override {
        if (bytes_.size() + bytes.size() > Executor::kMaxIntermediateBytes) {
            overflowed_ = true;
            return;
        }
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }
    void Commit() override { committed_ = true; }
    void Abandon() noexcept override {
        bytes_.clear();
        committed_ = false;
    }

    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }
    [[nodiscard]] bool committed() const noexcept { return committed_; }
    [[nodiscard]] std::vector<std::byte>& bytes() noexcept { return bytes_; }

private:
    std::vector<std::byte> bytes_;
    bool committed_ = false;
    bool overflowed_ = false;
};

class BufferSource final : public ISource {
public:
    explicit BufferSource(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {}

    std::size_t Read(std::span<std::byte> into) override {
        if (position_ >= bytes_.size()) return 0;
        const std::size_t take = std::min(bytes_.size() - position_, into.size());
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(position_), take, into.begin());
        position_ += take;
        return take;
    }
    bool Seek(std::uint64_t position) override {
        if (position > bytes_.size()) return false;
        position_ = static_cast<std::size_t>(position);
        return true;
    }
    [[nodiscard]] std::optional<std::uint64_t> Size() const override { return bytes_.size(); }

private:
    std::vector<std::byte> bytes_;
    std::size_t position_ = 0;
};

// A provider ignores the cancellation token between calls, so the executor
// checks at every hop boundary as well. Section 5 asks for 100 ms granularity
// inside a provider; this is the coarse backstop between them.
bool Cancelled(const ConvertContext& context) { return context.cancelled(); }

}  // namespace

void Executor::Add(IConversionProvider& provider) { providers_.push_back(&provider); }
void Executor::Add(doc::IDocumentReader& reader) { readers_.push_back(&reader); }
void Executor::Add(doc::IDocumentWriter& writer) { writers_.push_back(&writer); }

void Executor::BuildGraph(FormatGraph& graph) const {
    const MediaProfile ir = doc::IrProfile();

    for (const doc::IDocumentReader* reader : readers_) {
        for (const MediaProfile& from : reader->Reads()) {
            graph.AddEdge(from, ir, reader->ReadCost(from), reader->Id());
        }
    }
    for (const doc::IDocumentWriter* writer : writers_) {
        for (const MediaProfile& to : writer->Writes()) {
            graph.AddEdge(ir, to, writer->WriteCost(to), writer->Id());
        }
    }
    for (const IConversionProvider* provider : providers_) {
        for (const EdgeDecl& decl : provider->EnumerateEdges()) {
            graph.AddEdge(decl, provider->Id());
        }
    }
    graph.Freeze();
}

std::optional<MediaProfile> Executor::Probe(ISource& source, const PathHint& hint) const {
    // ALWAYS rewind before returning, including on the success path. A probe
    // reads up to 8 KiB and leaves the source wherever it stopped, so returning
    // early without seeking hands the next reader a source positioned past the
    // start -- which does not fail, it silently converts the tail of the file.
    // The result is an empty output reported as a success, and nothing about
    // that is visible until someone opens the file.
    const auto rewind = [&source] { (void)source.Seek(0); };

    // Document readers first: a .docx is an OPC package and recognisable from
    // its bytes, whereas the Markdown reader's probe is the one that has to fall
    // back to the extension. Consulting them in registration order and putting
    // the weakest last is what keeps a .docx from being claimed as text.
    for (const doc::IDocumentReader* reader : readers_) {
        if (!source.Seek(0)) return std::nullopt;
        if (auto profile = reader->Probe(source, hint)) {
            rewind();
            return profile;
        }
    }
    for (const IConversionProvider* provider : providers_) {
        if (!source.Seek(0)) return std::nullopt;
        if (auto profile = provider->Probe(source, hint)) {
            rewind();
            return profile;
        }
    }
    rewind();
    return std::nullopt;
}

doc::IDocumentReader* Executor::FindReader(std::string_view providerId,
                                           const MediaProfile& from) const {
    for (doc::IDocumentReader* reader : readers_) {
        if (reader->Id() != providerId) continue;
        for (const MediaProfile& accepted : reader->Reads()) {
            if (accepted == from) return reader;
        }
    }
    return nullptr;
}

doc::IDocumentWriter* Executor::FindWriter(std::string_view providerId,
                                           const MediaProfile& to) const {
    for (doc::IDocumentWriter* writer : writers_) {
        if (writer->Id() != providerId) continue;
        for (const MediaProfile& produced : writer->Writes()) {
            if (produced == to) return writer;
        }
    }
    return nullptr;
}

IConversionProvider* Executor::FindProvider(std::string_view providerId) const {
    const auto it = std::find_if(providers_.begin(), providers_.end(),
                                 [providerId](const IConversionProvider* provider) {
                                     return provider->Id() == providerId;
                                 });
    return it == providers_.end() ? nullptr : *it;
}

Result Executor::Execute(const FormatGraph& graph, const Plan& plan, ConvertContext& context,
                         ISource& source, ISink& sink) {
    if (Cancelled(context)) return Result::Cancelled();

    // Defence in depth for the bug above: a plan always starts at byte zero, and
    // enforcing that here means a caller who probed and forgot to rewind gets a
    // correct conversion rather than a silently empty one. A forward-only source
    // cannot rewind, and one that has already been read is genuinely unusable.
    if (!source.Seek(0)) {
        return Result::Error(Status::kIoError,
                             "The input could not be rewound to its start before converting.");
    }

    // A zero-hop plan is "convert this to what it already is". Copying is the
    // only honest interpretation; refusing would break a batch where one file in
    // fifty happens to be in the target format already.
    if (plan.hops.empty()) {
        std::vector<std::byte> chunk(kStreamChunkBytes);
        while (true) {
            const std::size_t got = source.Read(chunk);
            if (got == 0) break;
            sink.Write(std::span(chunk.data(), got));
            if (Cancelled(context)) return Result::Cancelled();
        }
        sink.Commit();
        return Result::Ok();
    }

    const MediaProfile ir = doc::IrProfile();

    // Section 11.4's weighting. cpuPerMiB is the only per-hop cost estimate the
    // graph carries; hops with no estimate share the bar evenly rather than
    // taking zero width, which would make them invisible.
    std::vector<double> weights;
    weights.reserve(plan.hops.size());
    for (const PlanHop& hop : plan.hops) {
        weights.push_back(std::max(0.001, static_cast<double>(graph.Cost(hop.edge).cpuPerMiB)));
    }
    const double totalWeight = std::accumulate(weights.begin(), weights.end(), 0.0);

    Result outcome = Result::Ok();
    std::unique_ptr<BufferSource> carried;  // output of the previous hop
    double completed = 0.0;

    for (std::size_t index = 0; index < plan.hops.size();) {
        if (Cancelled(context)) return Result::Cancelled();

        const PlanHop& hop = plan.hops[index];
        const MediaProfile& from = graph.Node(hop.from);
        const MediaProfile& to = graph.Node(hop.to);
        const EdgeCost& cost = graph.Cost(hop.edge);

        ISource& hopSource = carried ? static_cast<ISource&>(*carried) : source;

        // --- Fused document pass ---------------------------------------------
        if (to == ir) {
            // A plan that ends at the IR has nowhere to put its output: the IR
            // has no serialised form on purpose. The planner never produces one,
            // so this is a bug rather than bad input.
            if (index + 1 >= plan.hops.size()) {
                return Result::Error(Status::kInternal,
                                     "The conversion plan ends at the intermediate "
                                     "representation, which cannot be written to a file.");
            }
            const PlanHop& writeHop = plan.hops[index + 1];
            const MediaProfile& writeTo = graph.Node(writeHop.to);
            const EdgeCost& writeCost = graph.Cost(writeHop.edge);

            doc::IDocumentReader* reader = FindReader(cost.provider, from);
            doc::IDocumentWriter* writer = FindWriter(writeCost.provider, writeTo);
            if (reader == nullptr || writer == nullptr) {
                return Result::Error(Status::kInternal,
                                     "The conversion plan names a document provider that is not "
                                     "registered in this process.");
            }

            const double readSpan = weights[index] / totalWeight;
            const double writeSpan = weights[index + 1] / totalWeight;

            doc::Document document;
            {
                HopProgress progress(context.progress, completed, readSpan);
                ConvertContext hopContext{context.options, progress, context.cancel,
                                          context.temp, context.host};
                outcome.Absorb(reader->Read(hopContext, hopSource, from, document));
                if (!outcome) return outcome;
            }
            completed += readSpan;
            if (Cancelled(context)) return Result::Cancelled();

            const bool last = index + 2 >= plan.hops.size();
            BufferSink buffer;
            ISink& target = last ? sink : static_cast<ISink&>(buffer);

            {
                HopProgress progress(context.progress, completed, writeSpan);
                ConvertContext hopContext{context.options, progress, context.cancel,
                                          context.temp, context.host};
                outcome.Absorb(writer->Write(hopContext, document, writeTo, target));
                if (!outcome) return outcome;
            }
            completed += writeSpan;

            if (!last) {
                if (buffer.overflowed()) {
                    return Result::Error(Status::kResourceLimit,
                                         "An intermediate step of this conversion grew beyond "
                                         "512 MB.");
                }
                carried = std::make_unique<BufferSource>(std::move(buffer.bytes()));
            }
            index += 2;
            continue;
        }

        // --- Ordinary byte hop ------------------------------------------------
        if (from == ir) {
            // Reached only if a plan begins at the IR, which the planner cannot
            // produce because nothing can be read into it from a file.
            return Result::Error(Status::kInternal,
                                 "The conversion plan begins at the intermediate "
                                 "representation, which cannot be read from a file.");
        }

        IConversionProvider* provider = FindProvider(cost.provider);
        if (provider == nullptr) {
            return Result::Error(Status::kInternal,
                                 "The conversion plan names the provider '" + cost.provider +
                                     "', which is not registered in this process.");
        }

        const double span = weights[index] / totalWeight;
        const bool last = index + 1 >= plan.hops.size();
        BufferSink buffer;
        ISink& target = last ? sink : static_cast<ISink&>(buffer);

        {
            HopProgress progress(context.progress, completed, span);
            ConvertContext hopContext{context.options, progress, context.cancel, context.temp,
                                      context.host};
            outcome.Absorb(provider->Convert(hopContext, hopSource, target, from, to));
            if (!outcome) return outcome;
        }
        completed += span;

        if (!last) {
            if (buffer.overflowed()) {
                return Result::Error(Status::kResourceLimit,
                                     "An intermediate step of this conversion grew beyond "
                                     "512 MB.");
            }
            carried = std::make_unique<BufferSource>(std::move(buffer.bytes()));
        }
        ++index;
    }

    context.progress.Report(1.0, 0);
    return outcome;
}

}  // namespace morph
