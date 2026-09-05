#include "morph/core/naming.h"

#include "morph/core/path_safety.h"

namespace morph {
namespace {

// True when the template contains {key} as a placeholder this resolver knows.
// Written against the same scanner ApplyTemplate uses rather than a find() so
// that "{{name}}" and "{nameish}" answer the same way in both.
[[nodiscard]] bool Mentions(std::string_view namingTemplate, std::string_view key) {
    for (std::size_t i = 0; i < namingTemplate.size();) {
        if (namingTemplate[i] != '{') {
            ++i;
            continue;
        }
        const std::size_t close = namingTemplate.find('}', i);
        if (close == std::string_view::npos) break;
        if (namingTemplate.substr(i + 1, close - i - 1) == key) return true;
        i = close + 1;
    }
    return false;
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
            // The profile is "application/pdf" and a slash is not a filename
            // character. Sanitising the FRAGMENT here is half of the pair;
            // broker/output.cpp sanitises the assembled name as well, because
            // only that knows whether a device name ended up in front of a dot.
            out.append(io::SafeComponent(target));
        } else {
            // Same reasoning as the unterminated case: left visible.
            out.append(namingTemplate.substr(i, close - i + 1));
        }
        i = close + 1;
    }
    return out;
}

std::string NamingTemplateWarning(std::string_view namingTemplate) {
    if (namingTemplate.empty()) return {};  // empty means the default, not a mistake

    // The one that costs a user their afternoon. Everything else on this list is
    // cosmetic; this one converts a batch into a single file.
    if (!Mentions(namingTemplate, "name") && !Mentions(namingTemplate, "index")) {
        return "Every file would be given the same name. Add {name} or {index} "
               "so each one is different.";
    }

    // The malformed cases come BEFORE the missing-{ext} one, because a
    // misspelling is usually the reason {ext} is missing and "{etx} is not a
    // placeholder" is the sentence that fixes it. Reporting "no extension"
    // first would be true and useless: it describes the symptom of the typo
    // sitting three characters away.
    for (std::size_t i = 0; i < namingTemplate.size(); ++i) {
        if (namingTemplate[i] != '{') continue;
        const std::size_t close = namingTemplate.find('}', i);
        if (close == std::string_view::npos) {
            return "There is a { with no matching }, so the rest of the name "
                   "will be used literally.";
        }
        const std::string_view key = namingTemplate.substr(i + 1, close - i - 1);
        if (key != "name" && key != "ext" && key != "index" && key != "target") {
            return "{" + std::string(key) +
                   "} is not one of {name}, {ext}, {index} or {target}, so it "
                   "will appear in the filename as written.";
        }
        i = close;
    }

    // Not fatal -- ResolveOutput appends nothing and the file simply has no
    // extension -- but Explorer will not know what to open it with, which is
    // not what anyone is trying to arrange.
    if (!Mentions(namingTemplate, "ext")) {
        return "The name has no {ext}, so the converted files will have no "
               "extension.";
    }

    return {};
}

}  // namespace morph
