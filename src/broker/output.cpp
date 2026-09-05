#include "output.h"

#include <algorithm>
#include <system_error>

#include "morph/core/extensions.h"
#include "morph/core/naming.h"
#include "morph/core/path_safety.h"

namespace morph::broker {
namespace {

// Characters Windows will not accept in a file name, plus the ones that are
// legal but arrive from a media profile: "application/docx" has a slash in it
// and {target} is allowed in a template.
//
// Section 15.2's device names are handled by the same call, which is why the
// substitution below sanitises the FRAGMENTS and the finished name sanitises
// again: a stem of "nul" is safe until the template puts ".pdf" after it.
std::string Sanitised(std::string_view text) { return io::SafeComponent(text); }

}  // namespace

OutputPlan ResolveOutput(const std::filesystem::path& input, const std::string& targetProfile,
                         const ipc::OutputPolicy& policy, std::size_t index,
                         std::vector<std::filesystem::path>& taken) {
    OutputPlan plan;

    const std::string_view extension = ExtensionFor(targetProfile);
    if (extension.empty()) {
        plan.error = "Morph does not write " + targetProfile + " files.";
        return plan;
    }

    // An empty directory means "beside the input", which is what the shell
    // extension always wants and what the app defaults to.
    std::filesystem::path directory =
        policy.directory.empty() ? input.parent_path() : std::filesystem::path(policy.directory);
    if (directory.empty()) directory = std::filesystem::current_path();

    // Sanitised twice, deliberately: once on the fragments, so a hostile stem
    // cannot inject a separator and escape the directory, and once on the
    // assembled name, because the template chooses the final spelling and is the
    // only thing that knows whether "nul" ended up in front of a dot.
    const std::string name = Sanitised(
        ApplyTemplate(policy.namingTemplate.empty() ? kDefaultNamingTemplate
                                                    : std::string_view(policy.namingTemplate),
                      Sanitised(input.stem().string()), extension, index, targetProfile));
    if (name.empty()) {
        plan.error = "The naming template produced an empty file name.";
        return plan;
    }

    std::filesystem::path candidate = directory / name;

    const auto claimed = [&taken](const std::filesystem::path& path) {
        return std::find(taken.begin(), taken.end(), path) != taken.end();
    };

    std::error_code ec;
    const bool occupied = std::filesystem::exists(candidate, ec) || claimed(candidate);

    if (occupied) {
        switch (policy.overwrite) {
            case ipc::OverwriteRule::kSkip:
                plan.path = candidate;
                plan.skip = true;
                return plan;

            case ipc::OverwriteRule::kReplace:
                break;  // the caller asked for it explicitly

            case ipc::OverwriteRule::kRename:
            default: {
                // "report (1).pdf", the Explorer convention. The cap is not a
                // real limit, it is a refusal to spin: a thousand collisions on
                // one name means something is wrong upstream.
                const std::filesystem::path stem = candidate.stem();
                const std::filesystem::path suffix = candidate.extension();
                bool resolved = false;
                for (int n = 1; n <= 1000; ++n) {
                    std::filesystem::path attempt =
                        directory / (stem.string() + " (" + std::to_string(n) + ")" +
                                     suffix.string());
                    if (!std::filesystem::exists(attempt, ec) && !claimed(attempt)) {
                        candidate = std::move(attempt);
                        resolved = true;
                        break;
                    }
                }
                if (!resolved) {
                    plan.error = "There are already a thousand files named like " +
                                 candidate.filename().string() + ".";
                    return plan;
                }
                break;
            }
        }
    }

    // An input and its output being the same file is the one case that destroys
    // data outright, and it is reachable: converting report.html to text/html
    // with kReplace names the output over the input, and the reader is still
    // streaming from it when the writer truncates it.
    if (std::filesystem::equivalent(input, candidate, ec)) {
        plan.error = "That would write over " + input.filename().string() + " while reading it.";
        return plan;
    }

    taken.push_back(candidate);
    plan.path = std::move(candidate);
    return plan;
}

}  // namespace morph::broker
