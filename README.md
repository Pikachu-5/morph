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
| `morph.core` — plan executor with hop fusion and weighted progress | Implemented |
| `morph.worker.exe` + the unadvertised CLI (§2, §3.1) | Implemented |
| `morph.broker.exe` — singleton, job queue, scheduler, worker lifetime (§3.1, §10.3, §11) | Implemented |
| Capability table memory map (§9.2) | Implemented |
| Image provider — WIC, every installed codec (§6) | Implemented |
| `morph.shellext.dll` — IExplorerCommand, zero-IPC menu (§9) | Implemented, not yet registered |
| A/V provider — Media Foundation, remux fast path (§7.1) | Implemented |
| FFmpeg backend — subprocess, remux fast path (§7.2) | Implemented, no binary shipped |
| IR round-trip property tests (§16) | Implemented |
| Fuzz targets and the hostile-input corpus (§15, §16) | Implemented |
| Performance budgets, measured (§17) | Implemented, one budget missed |
| `Morph.App.exe` | Not started |

407 tests green (`ctest --preset x64-debug`), including one that starts the
broker, lets it start a worker, and converts a real file over the real pipe. The
same 407 pass under AddressSanitizer (`.\tools\build.ps1 -Preset x64-asan`),
which is the configuration the hostile-input corpus is worth running in.

§16 calls the IR round-trip property test the one that "catches more real bugs
than any other single test", and it was right. It generates documents nobody
thought of, writes them to DOCX and to Markdown, reads them back and compares
against what each format can actually hold. The afternoon it was first switched
on it found eleven defects in code that had been read carefully several times --
a Markdown writer that flattened every table cell to plain text, code spans that
could not survive a Word round trip, a stray backslash appended to any paragraph
ending in a line break, tight lists turning loose, sibling lists merging into
one, horizontal rules disappearing through Word, and a document beginning with
`---` whose entire contents were eaten as YAML front matter. All eleven are
fixed, each with an example test of its own beside the property.

Markdown, HTML and DOCX convert to each other in every direction, all three
render to PDF, and PDF text extracts back into any of them — with no direct
converter between any pair — the graph composes each route through
the IR, which is the whole argument of §8.2. Adding one format wires it to all
the others.

Images convert between PNG, JPEG, BMP and TIFF, reading GIF, WebP, HEIF and AVIF
wherever the machine has the codec — and only offering a target it has actually
proved it can encode, because a registered HEIF encoder that fails at commit is
what a stock Windows 11 has (§6.2). Orientation is normalised from EXIF before
anything touches the pixels and the tag is stripped on the way out, which is the
detail §6.1 puts first for good reason.

Audio and video go through Media Foundation, which is the OS's own codecs: MP4,
MOV, ASF, MP3, AAC, WAV, FLAC and WMA in, and whatever this machine can actually
encode out. What it can encode is established by negotiating with the encoder at
startup rather than by asking whether one is registered, because the two answers
differ. The remux fast path (§7.2) copies compressed samples when the codec
already matches — extracting the AAC soundtrack from an MP4 rewrites no audio at
all. Everything Media Foundation cannot do (Matroska, WebM, AV1, Opus, FLAC out)
is the FFmpeg backend's job. That backend runs FFmpeg as a subprocess and never
links it — §7.2's first reason is that the LGPL relinking obligation evaporates
when you execute rather than link, and §14's licensing matrix depends on it
staying true. It declares no edges at all unless `ffmpeg.exe` is beside the
binaries or `MORPH_FFMPEG_DIR` points at one, which is exactly the difference
between the two SKUs in §7.3: the same build, shipped two ways. No binary is
fetched yet; the pinned, checksummed fetch belongs with the packaging milestone
that assembles the GitHub SKU. The tests drive a stand-in binary through the real
CreateProcess, pipes and job object, so everything between the graph and the
process is covered without shipping a codec.

The shell extension builds its submenu from the memory-mapped capability table
with no IPC and no file IO, which is what §9.2 requires and what the 8 ms budget
in §17 leaves room for. Its logic — what a mixed selection may be offered, when
to fall back to a single generic entry, how a fidelity rating reads in four
words — is a separate library from the COM shell around it and is tested without
Explorer. It has not been loaded by Explorer yet: that needs package identity,
which arrives with the packaging milestone. The manifest that will register it is
in [`packaging/msix`](packaging/msix/AppxManifest.xml), holding the one CLSID it
shares with the code.

DOCX scope is held to §8.4's P1 (paragraphs, runs, headings, character
formatting, hyperlinks, images) and P2 (lists, tables). P3 (footnotes,
headers/footers, section properties) and P4 (tracked changes, comments) are
deliberately not implemented.

## Trying it

The worker carries an unadvertised command line over the same engine the app and
the Explorer menu use (§2). After building:

```powershell
cd .\out\build\x64-debug\bin
.\morph.worker.exe convert report.docx report.pdf
.\morph.worker.exe targets notes.md
.\morph.worker.exe formats
```

`targets` lists everything a file can become, with the fidelity rating and hop
count the planner worked out — the same numbers the UI will show before you
commit.

Every executable lands in one `bin` directory, because the broker finds its
worker by looking beside itself and because all four processes derive the pipe
name from the directory they share.

The broker has no user interface and is not started by hand in normal use — a
client that fails to connect starts it (§10.3). To watch one work:

```powershell
.\morph.broker.exe --verbose --idle-seconds 30
```

It exits after sixty seconds with no clients and no queued work. `--idle-seconds`
shortens that, and is what the end-to-end test passes so that a test run does not
leave a broker sitting in the session for a minute.

## Performance

`morph.bench.exe` measures §17's budgets on the machine it runs on, and
`--gate` makes it exit non-zero when something got more than ten percent slower
than it was — §16's threshold, not the budget to the decimal, because a check
that fails at random is a check nobody reads. On a release build of this
machine:

| Budget (§17) | Measured | Target |
| --- | --- | --- |
| Context menu, 100 files selected | 0.004 ms | ≤ 8 ms |
| Broker cold start to accepting jobs | 78 ms warm, 908 ms cold | ≤ 150 ms |
| 4000×3000 JPEG → PNG | 720 ms | ≤ 120 ms |
| 40-page DOCX → PDF, warm | 489 ms | ≤ 1500 ms |
| Broker idle private bytes | 1.3 MB | ≤ 12 MB |

Two of these need their story told.

**Cold start is two numbers, and both are real.** The probe answers — a WebView2
detection, three trial image encodes and five media-type negotiations — are
cached in `%LOCALAPPDATA%\Morph`, stamped with the Windows build and the binary
that wrote it. Warm, that is 78 ms. The launch right after an install pays the
whole probe cost and takes 908 ms, and that happens to every user exactly once,
so it is reported rather than averaged away.

**The image target is missed by a factor of five, and the number that was wrong
is the fixture.** The benchmark used to convert an `x ^ y` pattern, which is
uncorrelated per-pixel noise: it fills every DCT coefficient, so it decoded
slower than a photograph, and it defeats deflate, so the PNG encode looked cheap.
Against something actually shaped like a photograph — smooth colour, one hard
edge, fine grain, a 3.8 MB JPEG in and a 16.6 MB PNG out — the honest figure is
around 720 ms, and 685–751 ms across four runs on an otherwise idle machine.
Roughly 120 ms of it is WIC's JPEG decoder and the rest is one deflate stream
over 36 MB of pixels. The encoder's only dial is the PNG filter, and
sweeping all six moves the total by under six percent (turning filtering *off*
is the slowest of the lot, because it leaves deflate twice as much to chew).

So 120 ms is not reachable through this pipeline, and §17 says these are targets
to calibrate rather than measurements. The target stays in the table unedited;
the gate watches a 750 ms baseline beside it. Keeping both means the gate is
useful and the promise is still legible — collapsing them would lose one or the
other. The `png.filter` option exposes the dial for anyone who wants to spend
size on time.

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

Every one of those runs on GitHub Actions as well
([`.github/workflows/ci.yml`](.github/workflows/ci.yml)): x64 debug and release
on each push and pull request, with §14's licence scanner and §17's performance
gate as steps rather than as separate scripts, and the AddressSanitizer build
nightly and on `main`. The workflow drives `tools/build.ps1` rather than
reimplementing it, so CI and a local run cannot drift apart. ARM64 is not in the
matrix: §20.2 settled this as x64-only for now, and a runner that can build
ARM64 but not execute it would report green for a build nobody has run.

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

MIT, with a hard rule that is enforced rather than asserted: no GPL, AGPL or
LGPLv3 dependency enters the tree. Third-party components are limited to MIT,
BSD-2, BSD-3, Apache-2.0, BSL-1.0, ISC and Zlib. FFmpeg is consumed as a
subprocess in the GitHub SKU only, never linked. See section 14.

The enforcement is `tools/scan-licenses.ps1`, which runs as a build step:

```powershell
.\tools\build.ps1 -Target licenses
```

It reads what vcpkg actually installed rather than what the manifest asked for —
a transitive dependency is precisely the one that would arrive unnoticed — and
fails on anything outside the allowlist, on anything explicitly copyleft, and on
any package whose licence cannot be determined. It also writes the
`ThirdPartyNotices.txt` that §14's table obliges for every component.
