// Morph — the extension table (architecture sections 5, 9.2 and 12.2).
//
// An extension is never how an INPUT is identified. Section 5 is explicit that
// Probe reads bytes, and a renamed file has to work. This table is for the three
// places where there are no bytes to read:
//
//   * naming an output. The broker resolves "{name}.{ext}" before the worker has
//     produced a single byte, and {ext} has to come from somewhere.
//   * reading a user's intent. "convert notes.md notes.docx" states a target in
//     the only way a command line can.
//   * building the Explorer menu. Section 9.2's capability table is keyed by
//     extension because the shell extension has a selection of file NAMES and a
//     latency budget that forbids opening any of them.
//
// It lives in the core rather than in any one caller because the broker names the
// file, the worker writes it, and the shell extension predicts it. A
// disagreement between copies of this table would show up as a .docx containing
// a PDF.
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace morph {

struct FormatExtension {
    std::string_view profile;
    std::string_view extension;  // canonical, no dot, lowercase
    std::string_view alternate;  // also accepted from a user; empty when there is none

    // True when Morph can WRITE this format. A read-only format still belongs in
    // the table -- it is a legitimate source, and the menu has to key on its
    // extension -- but it must never be offered as a target.
    bool writable = true;
};

// Every format the table knows, in no particular order.
[[nodiscard]] std::span<const FormatExtension> KnownExtensions() noexcept;

// The extension to give a file of this profile, without the dot. Empty when the
// profile is not one we write -- callers must not fall back to a guess.
[[nodiscard]] std::string_view ExtensionFor(std::string_view profileId) noexcept;

// The profile a user means by this extension, with or without the leading dot
// and in any case. Empty when the extension is not one Morph handles.
[[nodiscard]] std::string_view ProfileForExtension(std::string_view extension) noexcept;

}  // namespace morph
