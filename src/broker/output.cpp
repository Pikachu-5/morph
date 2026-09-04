#include "output.h"

#include <algorithm>
#include <system_error>

#include "morph/core/extensions.h"

namespace morph::broker {
namespace {

// Characters Windows will not accept in a file name, plus the ones that are
// legal but arrive from a media profile: "application/docx" has a slash in it
// and {target} is allowed in a template.
std::string Sanitised(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '<': case '>': case ':': case '"': case '/':
            case '\\': case '|': case '?': case '*':
                out.push_back('-');
                break;
            default:
                // Control characters are not legal in a name either, and a
                // filename carrying one is a sign someone is trying something.
                out.push_back(static_cast<unsigned char>(c) < 0x20 ? '-' : c);
                break;
        }
    }
    return out;
}

}  // namespace

std::string ApplyTemplate(std::string_view namingTemplate, std::string_view name,
                          std::string_view extension, std::size_t index,
                          std::string_view target) {
    std::string out;
    out.reserve(namingTemplate.size() + name.size() + extension.size());

    for (std::size_t i = 0; i < namingTemplate.size();) {
        if (namingTemplate[i] != '{') {
            out.push_back(namingTemplate[i++]);
            continue;
        }
        const std::size_t close = namingTemplate.find('}', i);
        if (close == std::string_view::npos) {
            // An unterminated placeholder is a typo in a preference, not an
            // attack. Copy it through so the user sees their mistake in the
            // filename rather than losing the rest of the template.
            out.append(namingTemplate.substr(i));
            break;
        }

        const std::string_view key = namingTemplate.substr(i + 1, close - i - 1);
        if (key == "name") {
            out.append(name);
        } else if (key == "ext") {
            out.append(extension);
        } else if (key == "index") {
            out.append(std::to_string(index));
        } else if (key == "target") {
            out.append(Sanitised(target));
        } else {
            // Same reasoning: an unknown placeholder is left visible.
            out.append(namingTemplate.substr(i, close - i + 1));
        }
        i = close + 1;
    }
    return out;
}

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

    const std::string name =
        ApplyTemplate(policy.namingTemplate.empty() ? "{name}.{ext}" : policy.namingTemplate,
                      Sanitised(input.stem().string()), extension, index, targetProfile);
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
