# Morph

A native Windows file conversion engine. Shell-integrated, permissively licensed,
format-agnostic.

Morph is a local conversion engine with three properties no existing free tool
combines: a **format graph** rather than a format list, a **document model we
own** rather than a shell over Pandoc or LibreOffice, and a **published fidelity
contract** that tells users what they are about to lose before they commit.

Source comments cite section numbers (§4.2, §8.4, §15.2 and so on). They refer
to the project's architecture document, which is kept outside the repository;
the comments are written to stand on their own without it.

## Status

Pre-M0. What exists today:

| Component | State |
| --- | --- |
| `morph.core` — format graph, cost model, path planner, plan cache | Implemented |
| `morph.core` — provider contract, streams, options, `Result` | Implemented |
| `morph.core` — document IR, style resolution, resource store | Implemented |
| Markdown provider — md4c reader, our writer (§8.3) | Implemented |
| HTML provider — lexbor reader, our writer (§8.5) | Implemented |
| DOCX provider — OPC container, reader and writer (§8.4) | Implemented, P1 + P2 |
| PDF output — WebView2 print pipeline (§8.6) | Implemented |
| PDF input — PDFium text extraction (§8.7) | Implemented |
| IPC framing, codec and pipe transport (§10.1, §10.2) | Implemented |
| `morph.broker.exe`, `morph.worker.exe`, `morph.shellext.dll`, `Morph.App.exe` | Not started |
| Image and A/V providers (WIC, Media Foundation, FFmpeg) | Not started |

285 tests green (`ctest --preset x64-debug`).

Markdown, HTML and DOCX convert to each other in every direction, all three
render to PDF, and PDF text extracts back into any of them — with no direct
converter between any pair — the graph composes each route through
the IR, which is the whole argument of §8.2. Adding one format wires it to all
the others.

DOCX scope is held to §8.4's P1 (paragraphs, runs, headings, character
formatting, hyperlinks, images) and P2 (lists, tables). P3 (footnotes,
headers/footers, section properties) and P4 (tracked changes, comments) are
deliberately not implemented.

## Decisions taken beyond the document

The architecture leaves four questions open in section 20. They are settled as
follows, and the code enforces each one:

| Question | Decision |
| --- | --- |
| §20.1 Name and identity | `Morph` placeholders kept throughout — `morph::`, `Morph.Converter`, `\\.\pipe\morph.broker.*`. Trademark and Store availability still to be checked. |
| §20.2 ARM64 | x64 only for now. `CMakePresets.json` carries an `arm64-release` preset and an `arch-arm64` fragment so enabling it is a preset addition, not a refactor. |
| §20.3 Windows 10 | Windows 11 only. Enforced by `NTDDI_VERSION=0x0A00000C` in the shared compile settings; no legacy `IContextMenu` path and no acrylic fallback. |
| Build order | M0 skeleton, with the format graph built into `morph.core` first — the section 20 closing advice ("build the format graph and the fidelity contract before building any converter") applied inside M0's scope. |

One correction to the document is implemented in
[`planner.cpp`](src/core/src/graph/planner.cpp): section 4.2 specifies
lexicographic path comparison and then prescribes a single Dijkstra run over it.
Those are incompatible. `worstFidelity` folds with `max()`, which is not monotone
under lexicographic tie-breaking, so one Dijkstra returns a path that is not the
minimum under the document's own `Better()`. The comparator is kept verbatim; the
search runs one additive Dijkstra per fidelity ceiling — five in total — over
`(node, hops)` states. The header comment in that file carries the counterexample,
and `test_planner.cpp` has it as a regression test.

## Building

Requires Visual Studio 2022 with the *Desktop development with C++* workload,
CMake 3.25+, Ninja, and vcpkg.

```powershell
.\tools\build.ps1
```

The script locates Visual Studio, imports the x64 developer environment, then
configures, builds and tests. It sets `VCPKG_ROOT` if a vcpkg checkout is in a
usual place; otherwise set it yourself.

```powershell
.\tools\build.ps1 -Preset x64-release -Target test
.\tools\build.ps1 -Preset x64-asan -Fresh
```

From an existing Developer PowerShell the presets work directly:

```powershell
cmake --preset x64-debug
cmake --build --preset x64-debug
ctest --preset x64-debug
```

The presets pin `CMAKE_CXX_COMPILER=cl`. Without that, Ninja picks up whatever
compiler is first on `PATH` — Strawberry Perl's MinGW `g++` on a stock machine —
and the failure surfaces much later as something unrelated.

## Layout

```
src/core/          morph.core — IR, format graph, planner, job model, options
  include/morph/core/    public headers
  src/graph/             node/edge model, Dijkstra sweep, plan cache
src/providers/     one directory per provider (section 5)
src/broker/        morph.broker.exe — job queue, scheduler, capability publisher
src/worker/        morph.worker.exe — all parsing, decoding and encoding
src/shellext/      morph.shellext.dll — IExplorerCommand
src/app/           Morph.App.exe — WinUI, C++/WinRT
packaging/         MSIX manifests, Store and GitHub SKUs
tests/             unit, property, golden, fuzz, bench
third_party/       fetched prebuilts (PDFium); not committed
tools/             build script, PDFium fetcher, license scanner
```

## Licensing

MIT, with a hard rule enforced in CI: no GPL, AGPL or LGPLv3 dependency enters
the tree. Third-party components are limited to MIT, BSD-2, BSD-3, Apache-2.0,
BSL-1.0, ISC and Zlib. FFmpeg is consumed as a subprocess in the GitHub SKU only,
never linked. See section 14.
