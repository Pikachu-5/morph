// morph.shellext.dll — the Explorer context menu (architecture section 9).
//
// "You have shipped one of these into PowerToys, so the mechanics are familiar.
//  What follows is the part specific to Morph: a submenu whose contents depend
//  on the selection, built under a latency budget that forbids doing any actual
//  work."
//
// The budget is section 17's 8 ms around IEnumExplorerCommand::Next, and every
// decision here follows from it. GetTitle, GetState and EnumSubCommands read a
// memory-mapped table (caps.h) and compare a few strings. Nothing on that path
// opens a file, allocates a large buffer, starts a process, or talks to the
// broker -- and section 3.1 forbids all four in as many words.
//
// Invoke is where work is allowed to happen, and even there it happens on
// another thread: Explorer is blocked until Invoke returns.
#pragma once

#include <windows.h>

#include <shobjidl_core.h>

namespace morph::shellext {

// {A1B2C3D4-...} in section 9.1 is a placeholder. This is the real one, and it
// appears in exactly two places: here, and in the package manifest. They must
// match, which is why the manifest carries a comment pointing at this file.
//
// {6F3E1D0A-9C4B-4E58-9A2F-0B5E7C1D8A34}
inline constexpr GUID kMorphContextMenuClsid = {
    0x6f3e1d0a, 0x9c4b, 0x4e58, {0x9a, 0x2f, 0x0b, 0x5e, 0x7c, 0x1d, 0x8a, 0x34}};

// The root "Convert with Morph" command. Everything else hangs off it.
[[nodiscard]] HRESULT CreateRootCommand(REFIID riid, void** out);

// Live object count, for DllCanUnloadNow.
[[nodiscard]] long LiveObjects() noexcept;
void AddObject() noexcept;
void ReleaseObject() noexcept;

}  // namespace morph::shellext
