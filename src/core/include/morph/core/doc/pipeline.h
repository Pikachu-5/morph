// Morph — how document providers plug into the graph.
//
// A PROBLEM SECTION 5 LEAVES OPEN, and how this resolves it.
//
// Section 4.3 makes the IR a graph node: "docx -> IR -> html -> pdf -> png".
// Section 5 gives every provider a byte-oriented Convert(ISource&, ISink&). Put
// together, those say that a docx -> md conversion serialises the IR to bytes
// after the reader and parses it back before the writer -- for an intermediate
// that never leaves the worker process. For a document whose ResourceStore holds
// fifty megabytes of images, that is two pointless copies and a parser we would
// have to write, test and fuzz for no user-visible benefit.
//
// So document providers implement one of the two narrow interfaces below, and
// the executor fuses an adjacent reader/writer pair into a single in-memory
// handoff. The byte-oriented IConversionProvider contract is untouched and
// remains what every non-document provider implements; a document provider is
// exposed through it too (see DocumentProvider), so the graph, the planner and
// the fidelity contract see nothing unusual.
//
// The IR deliberately has no serialised form yet. If v2 moves the document
// pipeline into its own process, that is the moment to add one -- and the
// interfaces below are already the right seam for it.
#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "morph/core/doc/document.h"
#include "morph/core/provider.h"

namespace morph::doc {

// Parses a source format into the IR. Rule 1 applies: keep everything the format
// expresses, even what the eventual target cannot represent. The loss belongs to
// the writer, which can report it.
struct IDocumentReader {
    virtual ~IDocumentReader() = default;

    [[nodiscard]] virtual std::string_view Id() const noexcept = 0;

    // Profiles this reader accepts, e.g. "text/markdown".
    [[nodiscard]] virtual std::vector<MediaProfile> Reads() const = 0;

    // The cost of parsing this format into the IR. Almost always Lossless: a
    // reader that loses something is a reader with a bug, because the IR is a
    // superset by construction.
    [[nodiscard]] virtual EdgeCost ReadCost(const MediaProfile& from) const = 0;

    [[nodiscard]] virtual std::optional<MediaProfile> Probe(ISource& source,
                                                            const PathHint& hint) const = 0;

    [[nodiscard]] virtual Result Read(ConvertContext& context, ISource& source,
                                      const MediaProfile& from, Document& into) = 0;
};

// Serialises the IR into a target format. This is where loss happens and where
// it is reported: a writer that drops a table must add a FidelityNote saying so.
struct IDocumentWriter {
    virtual ~IDocumentWriter() = default;

    [[nodiscard]] virtual std::string_view Id() const noexcept = 0;

    [[nodiscard]] virtual std::vector<MediaProfile> Writes() const = 0;

    // The rating the UI shows before the user commits. IR -> Markdown is
    // Structural because layout does not survive; IR -> DOCX is Lossless.
    [[nodiscard]] virtual EdgeCost WriteCost(const MediaProfile& to) const = 0;

    [[nodiscard]] virtual Result Write(ConvertContext& context, const Document& document,
                                       const MediaProfile& to, ISink& sink) = 0;
};

// The canonical IR node id. Every document reader declares an edge INTO this and
// every writer an edge OUT of it, which is what gives the planner md -> docx,
// docx -> md, docx -> pdf and so on without anyone writing those converters.
inline constexpr std::string_view kIrProfileId = "application/morph-ir";

[[nodiscard]] MediaProfile IrProfile();

// Exposes a reader, a writer, or both through the byte-oriented provider
// contract, so the registry and the graph builder need no special case.
//
// Converting through this adapter alone would serialise the IR; it does not,
// because the only edges it advertises have the IR at one end, and the executor
// never asks a single provider to cross the IR node. Its Convert is reachable
// only when a caller deliberately asks for one half of the pipeline -- the CLI
// does this to dump the IR for debugging.
class DocumentProvider final : public IConversionProvider {
public:
    DocumentProvider(std::string id, IDocumentReader* reader, IDocumentWriter* writer);

    [[nodiscard]] std::string_view Id() const noexcept override { return id_; }
    [[nodiscard]] std::vector<EdgeDecl> EnumerateEdges() const override;
    [[nodiscard]] std::optional<MediaProfile> Probe(ISource& source,
                                                    const PathHint& hint) const override;
    [[nodiscard]] Result Convert(ConvertContext& context, ISource& source, ISink& sink,
                                 const MediaProfile& from, const MediaProfile& to) override;

    [[nodiscard]] IDocumentReader* reader() const noexcept { return reader_; }
    [[nodiscard]] IDocumentWriter* writer() const noexcept { return writer_; }

private:
    std::string id_;
    IDocumentReader* reader_;
    IDocumentWriter* writer_;
};

}  // namespace morph::doc
