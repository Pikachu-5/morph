#include "capabilities.h"

#include <algorithm>

#include "morph/core/extensions.h"
#include "morph/core/planner.h"

namespace morph::broker {

CapabilitySet CapabilitiesOf(const FormatGraph& graph) {
    CapabilitySet set;

    for (const FormatExtension& source : KnownExtensions()) {
        // A format the graph does not have a node for is one this build cannot
        // read -- a provider removed by the capability probe, say. It gets no
        // menu entry rather than an entry that fails.
        if (!graph.Find(source.profile)) continue;

        CapabilitySet::Source entry;
        entry.extension = std::string(source.extension);

        PlanRequest request;
        request.source = std::string(source.profile);
        // The estimate is not published: it depends on the file's size, which the
        // shell extension is not allowed to look up (section 9.2's budget). The
        // app asks QueryTargets when it wants a number.
        request.inputSizeMiB = 1.0;

        for (const auto& [node, plan] : FindAllTargets(graph, request)) {
            const std::string& profile = graph.Node(node).Id();
            if (profile == source.profile) continue;

            const std::string_view extension = ExtensionFor(profile);
            if (extension.empty()) continue;  // intermediate, or read-only

            CapabilitySet::Target target;
            target.profile = profile;
            target.extension = std::string(extension);
            target.fidelity = static_cast<std::uint8_t>(plan.fidelity());
            // Both are stored in a byte. Section 16 caps a path at five hops, so
            // the only way to overflow either is a bug elsewhere -- clamp rather
            // than wrap, so that the menu stays truthful about which of two
            // routes is worse.
            target.hopCount = static_cast<std::uint8_t>(std::min<std::uint32_t>(
                plan.cost.hopCount, 255u));
            target.requiresDisk = plan.requiresDisk;
            entry.targets.push_back(std::move(target));
        }

        // Best first: the menu is read top to bottom, and the fidelity contract
        // is the thing the user is choosing between (section 8.8).
        std::sort(entry.targets.begin(), entry.targets.end(),
                  [](const CapabilitySet::Target& a, const CapabilitySet::Target& b) {
                      if (a.fidelity != b.fidelity) return a.fidelity < b.fidelity;
                      if (a.hopCount != b.hopCount) return a.hopCount < b.hopCount;
                      return a.extension < b.extension;
                  });

        set.sources.push_back(std::move(entry));
    }

    return set;
}

}  // namespace morph::broker
