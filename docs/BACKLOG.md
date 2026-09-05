# Morph — what is left

Everything in the architecture document that is not built yet, plus the things
that are built and known to be imperfect. Section numbers refer to
`Morph-Architecture-v0.1.pdf`.

**How to read this.** Group D is the important one. It is not a to-do list — it
is a list of things that must *stay* undone, each with the reason, because
several of them look like small omissions and are not. §20 names scope creep in
one of them as the single most likely way this project stalls.

Sizes are rough and assume the existing code as a starting point.

Status as of the last commit: M0–M11 complete, M3 complete, **M12 is the only
milestone remaining**. 439 tests green in `x64-debug` and under AddressSanitizer.

---

## A. M12 — packaging and distribution (§13)

The only milestone left, and the gate on everything else being real. The shell
extension is §2's "daily-driver path", it is implemented and tested, and Explorer
has never loaded it — because that needs package identity, which lives here.

| # | Item | Notes | Size |
|---|---|---|---|
| A1 | `tools/fetch-winappsdk.ps1` | Pinned version + SHA-256, in the shape `fetch-pdfium.ps1` already has. Flips `MORPH_BUILD_APP` to `ON` by default and puts `Morph.App.exe` in CI. Today `src/app/winappsdk.cmake` reads the SDK out of the local NuGet cache, which is why the app is off by default. | ~half a day |
| A2 | Windows App SDK in §14's licence matrix | `tools/scan-licenses.ps1` covers vcpkg and PDFium and does not see the SDK at all. The gate is a required check; it should not have a blind spot. | ~2 hours |
| A3 | FFmpeg pinned fetch (GitHub SKU) | §7.2/§7.3: same build, two SKUs, distinguished by whether `ffmpeg.exe` is present. No binary is fetched yet. Pin + checksum, same shape as A1. | ~half a day |
| A4 | Shipping `AppxManifest.xml`, with the shell extension | `packaging/msix/AppxManifest.xml` already carries the surrogate and the `windows.fileExplorerContextMenus` entry, sharing one CLSID with `src/shellext/shellext.h`. Registering it is what finally loads the menu in Explorer. Blocked since M2. | ~1 day |
| A5 | Real package assets | `packaging/msix/Assets/*.png` are solid-colour placeholders generated to make registration succeed. Needs a real icon set. | design work |
| A6 | Both SKUs | `packaging/store/` and `packaging/github/` are empty directories. Store bundle config; GitHub `.appinstaller` for updates. | ~1–2 days |
| A7 | Store submission | §20 flags certification of a context-menu extension as a medium risk with slow review; the advice there is to submit a minimal build early to get the review clock started. | unknown |

**Also here:** the dev manifest (`AppxManifest.dev.xml`) and the shipping one
must be kept in step by hand for as long as both exist. Worth collapsing into one
generated file during A4.

---

## B. Features the architecture asks for, not yet built

| # | Item | Where | Section | Size |
|---|---|---|---|---|
| B1 | The shell extension's generic entry should launch the app with the selection | `src/shellext/command.cpp:362` — `Invoke` returns `S_OK` and does nothing, with a comment saying "the app is not built yet". That is now stale: the app takes files on its command line. | §9.2 | ~1 hour |
| B2 | Options and presets in the app | `AppModel::BuildJob` always sends an empty option bag. The whole provider option surface today is `png.filter`, `quality`, `landscape`. | §2 | ~3–4 days |
| B3 | Output policy UI | Destination folder, naming template, overwrite rule. Settable on `AppModel::output()`, no UI behind it. | §12.2, §10.2 | ~1 day |
| B4 | Store deep link when a codec is missing | A `kMissingCapability` result should become a button that opens the codec in the Store, not an error message. | §6.2 | ~half a day |
| B5 | Use tagged PDF structure when present | `src/providers/pdf/pdf_reader.cpp:501` detects tagging, records `pdf-tagged` in metadata, and emits a `pdf-tagging-unused` note saying structure was inferred from layout instead. §8.7 says tags "should be used when present". Would improve PDF→anything fidelity materially. | §8.7 | ~3 days |
| B6 | HTML writer: directory-with-assets output mode | The writer is always self-contained (images as data URIs). §8.5 asks for a choice between that and "a directory with an assets folder". | §8.5 | ~1 day |

**B2 has a constraint that shapes it.** §3's design rule: *"The app and the shell
extension are peers, not layers. Every capability reachable from the UI is
reachable from the context menu and from the worker's CLI surface… If a feature
can only be built into the UI, it is in the wrong place."* So options are
protocol and CLI work first and a settings screen second. The CLI already parses
`key=value`; the wire already carries `OptionValue`. The gap is provider
declaration, discovery, and the screen.

---

## C. Built, and known to be imperfect

| # | Item | Where | Notes |
|---|---|---|---|
| C1 | `MORPH-IR` is offered as a conversion target | Visible in the app's picker | The IR is an internal graph node leaking into a user-facing list. Filter internal profiles out of `QueryTargets` in the broker. ~1 hour. |
| C2 | DOCX continuation paragraphs are not reconstructed | `tests/property/test_ir_roundtrip.cpp:313` | A list item holding two blocks comes back as an item, a paragraph, and a new list — Word expresses a continuation paragraph as list style + indent without numbering, and the reader closes the list instead. The property generator emits one block per item because of it. Fixing it lifts that restriction too. |
| C3 | Sub-lists at one depth cannot disagree | `src/providers/docooxml/docx_writer.cpp` | A level is one `w:lvl` for the whole tree, so two sub-lists under one parent that want different markers or different first numbers have no spelling. Declared with `list-level-markers-shared` / `list-level-start-shared`. This is a format limit, not a defect — listed so nobody re-derives it. |
| C4 | The app links the whole provider table to probe | `src/app/main.cpp`, `FileProber` | §12.2 wants files probed on drop; doing it in-process pulls WIC, Media Foundation and PDFium into the UI process. `AppModel`'s injected `Prober` exists precisely so this can become a round trip to the worker, which already has every provider loaded. One function to change. |
| C5 | Dropping files mid-job starts a new selection | `AppModel::Drop` | Deliberate — a queue that grows while it runs is a different feature. Revisit if it annoys anyone. |

---

## D. Deliberately out of scope — do not "fix" these

**Read this group before adding anything to the document pipeline.**

| # | Item | Why it stays undone |
|---|---|---|
| D1 | **DOCX P3 and P4** — footnotes, headers/footers, section properties; tracked changes and comments | §8.4: *"Do not attempt full OOXML. Phase it… **Ship after P2.**"* §20 ranks DOCX scope creep as **the single highest risk in the project**: *"Enforce the P1–P4 phasing… and refuse anything above the current phase, no matter how small it looks."* This is not a backlog item, it is a defence. The reader already prices it honestly — its edge cost is `High` rather than `Lossless` *because* P3/P4 are out — and `w:del` is dropped with a note because reinstating deleted text is a confidentiality problem. **If P3 is ever wanted, footnotes are much the cheapest**: the IR already models `FootnoteRef` and `Footnote` (`document.h:82`, `:214`) and no provider produces one yet, so the design work is done and only the DOCX ends are missing. |
| D2 | OCR for scanned PDFs | §1, explicitly out of scope for v1. `pdf_reader.cpp` already says so in the failure message rather than pretending. |
| D3 | Vector formats, CAD, 3D meshes, spreadsheets, presentations, e-books, RAW beyond WIC | §1's "explicitly out of scope for v1" list. Each is a plugin later; none is v1. |
| D4 | Out-of-tree providers / dynamic provider loading | §5.1: *"do not build the dynamic loading in v1. It is the single easiest way to turn a three-month project into a nine-month project."* The interface is already designed so v2 can do it without changing provider code. |
| D5 | The image budget missed by 5× | §17. Measured, understood and written up: the old benchmark fixture was uncorrelated noise, the honest figure against a photograph is ~720 ms, and roughly 120 ms of it is WIC's decoder with the rest one deflate stream over 36 MB. The target stays in the table unedited and the gate watches a 750 ms baseline beside it. |
| D6 | Broker cold start, 908 ms against a 150 ms target | §17. Both numbers are real: 78 ms warm, 908 ms on the first launch after an install, which every user pays exactly once. Reported as two numbers rather than averaged away. |
| D7 | A cleaner for the WebView2 profile in `%TEMP%` | Measured: flat across 20 conversions, and flat across five workers *killed mid-render*, which is the normal exit here. Cold and warm both cost ~0.5 s, so the folder is disposable — a stronger property than pruned. See the note in `webview2_host.cpp`. |

---

## E. Open questions (§20)

| # | Question | State |
|---|---|---|
| E1 | §20.1 — name and identity | "Morph" is a working codename throughout: `morph::`, `Morph.Converter`, the pipe name. Trademark search and Store name reservation still to do, and A6/A7 depend on the answer. |
| E2 | §20.2 — ARM64 | Settled as x64 only for now. `CMakePresets.json` carries an `arm64-release` preset and an `arch-arm64` fragment, so enabling it is a matrix entry rather than a refactor. CI cannot run ARM64 tests on a `windows-latest` runner, which is why it is not already on. |
| E3 | Licensing of Morph itself | MIT assumed throughout. §20 suggests deciding *now* whether the core stays MIT with a separate licence for the app shell, "rather than relicensing later with contributors in the tree". |

---

## Suggested order

1. **B1 and C1.** Both are an hour, and B1 is now only a stale comment and a
   `CreateProcess`. They finish §9.2 properly.
2. **A1–A4.** Packaging. This is the gate: it is what finally puts the context
   menu in Explorer, what lets the thing be installed and dogfooded, and what
   §19 has as the last milestone. Nothing else is shippable without it.
3. **B2 and B3.** Options, presets and output policy — the feature §2 says the
   app exists for. Protocol and CLI first, per §3's peers rule.
4. **B5.** Tagged PDF, the largest remaining fidelity win.

§19's own shipping advice was *"a public v1 at M8 is a real product… ship there,
get users, then land DOCX in v1.1."* This is past M8 **and** past DOCX. The
engine is finished; packaging is the only thing between it and something people
can install.
