#include "morph/core/doc/pipeline.h"

#include <stdexcept>
#include <utility>

namespace morph::doc {

MediaProfile IrProfile() {
    static const MediaProfile profile = *MediaProfile::Parse(kIrProfileId);
    return profile;
}

DocumentProvider::DocumentProvider(std::string id, IDocumentReader* reader,
                                   IDocumentWriter* writer)
    : id_(std::move(id)), reader_(reader), writer_(writer) {
    if (id_.empty()) throw std::invalid_argument("morph: document provider with no id");
    if (!reader_ && !writer_) {
        throw std::invalid_argument("morph: document provider " + id_ +
                                    " has neither a reader nor a writer");
    }
}

std::vector<EdgeDecl> DocumentProvider::EnumerateEdges() const {
    std::vector<EdgeDecl> edges;
    const MediaProfile ir = IrProfile();

    if (reader_) {
        for (const MediaProfile& from : reader_->Reads()) {
            edges.push_back(EdgeDecl{from, ir, reader_->ReadCost(from)});
        }
    }
    if (writer_) {
        for (const MediaProfile& to : writer_->Writes()) {
            edges.push_back(EdgeDecl{ir, to, writer_->WriteCost(to)});
        }
    }
    return edges;
}

std::optional<MediaProfile> DocumentProvider::Probe(ISource& source, const PathHint& hint) const {
    return reader_ ? reader_->Probe(source, hint) : std::nullopt;
}

Result DocumentProvider::Convert(ConvertContext& context, ISource& source, ISink& /*sink*/,
                                 const MediaProfile& from, const MediaProfile& to) {
    // One half of the pipeline at a time. The executor fuses reader and writer
    // in memory and never routes a whole document conversion through here; this
    // path exists for the CLI's IR dump and for symmetry with every other
    // provider.
    const MediaProfile ir = IrProfile();

    if (to == ir) {
        if (!reader_) {
            return Result::Error(Status::kUnsupportedInput,
                                 "This provider cannot read " + from.Id() + ".");
        }
        Document document;
        Result result = reader_->Read(context, source, from, document);
        if (!result) return result;
        // There is no serialised IR yet, on purpose (see the header). Asking for
        // one is a programming error in the caller, not a user-facing failure.
        return Result::Error(Status::kInternal,
                             "The intermediate representation has no serialised form; "
                             "use the document executor rather than converting to " +
                                 ir.Id() + " directly.");
    }

    if (from == ir) {
        return Result::Error(Status::kInternal,
                             "The intermediate representation has no serialised form; "
                             "use the document executor rather than converting from " +
                                 ir.Id() + " directly.");
    }

    return Result::Error(Status::kUnsupportedInput,
                         "No edge from " + from.Id() + " to " + to.Id() + " in " + id_ + ".");
}

}  // namespace morph::doc
