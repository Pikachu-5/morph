// Morph — the extension table (architecture sections 5 and 12.2).
//
// An extension is never how an INPUT is identified. Section 5 is explicit that
// Probe reads bytes, and a renamed file has to work. This table is for the two
// places where there are no bytes to read:
//
//   * naming an output. The broker resolves "{name}.{ext}" before the worker has
//     produced a single byte, and {ext} has to come from somewhere.
//   * reading a user's intent. "convert notes.md notes.docx" states a target in
//     the only way a command line can.
//
// It lives in the core rather than in either caller because the broker names the
// file and the worker writes it, and a disagreement between two copies of this
// table would show up as a .docx containing a PDF.
#pragma once

#include <string>
#include <string_view>

namespace morph {

// The extension to give a file of this profile, without the dot. Empty when the
// profile is not one we write -- callers must not fall back to a guess.
[[nodiscard]] std::string_view ExtensionFor(std::string_view profileId) noexcept;

// The profile a user means by this extension, with or without the leading dot
// and in any case. Empty when the extension is not one we produce.
[[nodiscard]] std::string_view ProfileForExtension(std::string_view extension) noexcept;

}  // namespace morph
