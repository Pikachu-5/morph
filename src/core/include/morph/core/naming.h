// Morph — the output naming template (architecture section 10.2).
//
// The template is §10.2's OutputPolicy field, and that section calls the policy
// "the part users get wrong and support questions are about". It lives in the
// core rather than in the broker for the same reason the extension table does:
// two processes have to agree about it, and a disagreement is silent.
//
// The BROKER resolves the real name, because only it can see the whole batch at
// once and decide collisions against one view of the directory. The APP shows
// the user what their template is going to produce before they commit to it,
// which is the difference between a preference and a guess. If those two ever
// disagreed the preview would be a lie -- so they are one function.
//
// Nothing here touches the filesystem. Substitution and inspection only; where
// a file actually lands is broker/output.h, which calls this and then does the
// part that needs a directory.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace morph {

// "{name}.{ext}" with {name}, {ext}, {index} and {target} substituted.
//
// An unterminated or unknown placeholder is copied through verbatim rather than
// dropped: it is a typo in a preference, not an attack, and a user who sees
// "{nmae}.pdf" on disk knows immediately what they did. Silently swallowing it
// would produce ".pdf" and no clue.
[[nodiscard]] std::string ApplyTemplate(std::string_view namingTemplate, std::string_view name,
                                        std::string_view extension, std::size_t index,
                                        std::string_view target);

// The default, used whenever a policy carries an empty template. Beside each
// input, same stem, new extension.
inline constexpr std::string_view kDefaultNamingTemplate = "{name}.{ext}";

// What is wrong with this template, in one sentence a person can act on, or
// empty when there is nothing to say.
//
// This is a WARNING, not validation: every string here is a legal template and
// the broker will resolve any of them. It exists because one particular mistake
// is invisible until after the conversion -- a template with neither {name} nor
// {index} in it produces the SAME name for every file in the batch, so a folder
// of two hundred photographs converts into one file plus a hundred and ninety
// nine renamed copies of it. That is allowed. It is almost never meant.
[[nodiscard]] std::string NamingTemplateWarning(std::string_view namingTemplate);

}  // namespace morph
