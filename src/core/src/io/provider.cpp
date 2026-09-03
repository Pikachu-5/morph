#include "morph/core/provider.h"

#include <algorithm>
#include <stdexcept>

namespace morph {

bool Capabilities::Has(std::string_view id) const {
    return std::find(present_.begin(), present_.end(), id) != present_.end();
}

void ProviderRegistry::Register(IConversionProvider& provider) {
    if (provider.Id().empty()) throw std::invalid_argument("morph: provider with no id");
    if (Find(provider.Id())) {
        throw std::invalid_argument("morph: duplicate provider id " + std::string(provider.Id()));
    }
    providers_.push_back(&provider);
}

void ProviderRegistry::BuildGraph(FormatGraph& graph) const {
    for (IConversionProvider* provider : providers_) {
        for (const EdgeDecl& decl : provider->EnumerateEdges()) {
            graph.AddEdge(decl, provider->Id());
        }
    }
    graph.Freeze();
}

std::optional<MediaProfile> ProviderRegistry::Probe(ISource& source, const PathHint& hint) const {
    for (IConversionProvider* provider : providers_) {
        // Every provider sees the input from byte zero. A provider that leaves
        // the source somewhere else must not break the next one's sniff.
        if (!source.Seek(0)) return std::nullopt;

        if (auto profile = provider->Probe(source, hint)) return profile;
    }
    return std::nullopt;
}

IConversionProvider* ProviderRegistry::Find(std::string_view id) const {
    const auto it = std::find_if(providers_.begin(), providers_.end(),
                                 [id](const IConversionProvider* p) { return p->Id() == id; });
    return it == providers_.end() ? nullptr : *it;
}

}  // namespace morph
