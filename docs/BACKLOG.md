# Morph — what is left

Everything in the architecture document that is not built yet, plus the things
that are built and known to be imperfect. Section numbers refer to
`Morph-Architecture-v0.1.pdf`.

**How to read this.** Group D is the important one. It is not a to-do list — it
is a list of things that must *stay* undone, each with the reason, because
several of them look like small omissions and are not. §20 names scope creep in
one of them as the single most likely way this project stalls.

---

## Scoreboard

Keep this honest. A future session should be able to read one table and know
where the project is without reading the rest of the file.

| Group | Open | Done | Total | What it is |
|---|---:|---:|---:|---|
| **A** — packaging and distribution (§13) | 4 | 3 | 7 | The last milestone |
| **B** — features the architecture asks for | 5 | 5 | 10 | Not built yet |
| **C** — built, and known to be imperfect | 3 | 4 | 7 | Defects and priced limits |
| **D** — deliberately out of scope | — | — | 7 | **Must stay undone** |
| **E** — open questions (§20) | 2 | 1 | 3 | Decisions, not code |
| **F** — measurement and test gaps (§16, §17) | 8 | 0 | 8 | The gates with holes in them |
| | **22** | **13** | **42** | |

**Blocked on a decision, not on effort:** A5, A6, A7 all wait on E1 (the name),
because the signing certificate's subject has to match the package identity.

**Estimates in this file are for a human developer.** They were written that way
and are kept that way; do not read them as a session budget.

Status: M0–M11 complete, M3 complete, M12 mostly done. 465 tests green in
`x64-debug`; the release and sanitiser configurations are run before a
milestone rather than on every edit.

---

## A. M12 — packaging and distribution (§13)

The only milestone left. The shell extension needed package identity before
Explorer could load it, and that is done — what remains here is distribution
rather than capability.

| # | Item | Status | Notes | Size |
|---|---|---|---|---|
| A1 | `tools/fetch-winappsdk.ps1` | **done** | Five packages pinned by version and SHA-256 in `third_party/winappsdk-packages.txt`, read by the fetcher, `src/app/winappsdk.cmake` and the licence scanner alike. `MORPH_BUILD_APP` is `ON`. | |
| A2 | Windows App SDK in §14's licence matrix | **done** | Three of the five are Microsoft Software License Terms, accepted as platform components with the reason printed rather than by widening the allowlist. An unfetched pin fails the scan. | |
| A3 | FFmpeg pinned fetch (GitHub SKU) | open | §7.2/§7.3: same build, two SKUs, distinguished by whether `ffmpeg.exe` is present. Pin + checksum, same shape as A1 — the pin manifest is now a pattern to copy. Also unblocks F3 and the real-`ffprobe` half of §16. | ~half a day |
| A4 | Shipping manifest, with the shell extension | **done** | One template generated twice; the CLSID comes from CMake and a test compares it with the constant the DLL was compiled with. Registered, and Explorer loads it. Packs into a real `.msix`. | |
| A5 | Real package assets | open | `packaging/msix/Assets/*.png` are solid-colour placeholders. Needs an icon set. **Waits on E1.** | design work |
| A6 | Both SKUs | open | `packaging/store/` and `packaging/github/` are empty. Store bundle config; GitHub `.appinstaller` for updates. Signing needs a certificate. **Waits on E1.** | ~1–2 days |
| A7 | Store submission | open | §20 flags certification of a context-menu extension as medium risk with slow review; submit a minimal build early. **Waits on E1.** | unknown |

**Learned while doing A4, and worth not rediscovering.** A *packaged* surrogate
and an *unpackaged* broker do not share the capability table: identity puts the
surrogate in a different named-object namespace, so a broker started from a
shell prompt publishes where the surrogate cannot see it, and the menu shows
§9.2's generic entry for a reason that has nothing to do with the selection.
Nothing shipped is affected. To see a populated submenu by hand, start the
broker *through* the package (`Start-Process 'shell:AppsFolder\<PFN>!Morph'`).

---

## B. Features the architecture asks for, not yet built

| # | Item | Status | Where | § | Size |
|---|---|---|---|---|---|
| B1 | Generic menu entry launches the app with the selection | **done** | `src/shellext/launch.h` builds the command line, tested against the real `CommandLineToArgvW`; too long is refused, not truncated. | 9.2 | |
| B2 | Options and presets in the app | open | `AppModel::BuildJob` always sends an empty option bag. The whole provider option surface is `png.filter`, `quality`, `landscape`. | 2 | ~3–4 days |
| B3 | Output policy UI | **done** | Destination, naming template and overwrite rule, behind an Expander with an always-visible preview line. `ApplyTemplate` moved to the core so the preview and the broker's real answer come from one implementation; `NamingTemplateWarning` is new. | 12.2, 10.2 | |
| B4 | Store deep link when a codec is missing | open | A `kMissingCapability` result should become a button that opens the *codec* in the Store, not an error message. Not our storefront — Microsoft's codec packs. | 6.2 | ~half a day |
| B5 | Use tagged PDF structure when present | open | `pdf_reader.cpp:501` detects tagging and emits a `pdf-tagging-unused` note saying structure was inferred from layout. PDFium exposes the tree (19 `FPDF_StructElement_*` calls); the cost is mapping it without regressing the layout path, and PDF fidelity is the worst thing to verify. | 8.7 | ~3 days |
| B6 | HTML writer: directory-with-assets output mode | open | Always self-contained (images as data URIs). §8.5 asks for a choice. | 8.5 | ~1 day |
| B7 | Title bar drag rectangles | **done** | Computed from the real bounds of a drag element, in physical pixels via `RasterizationScale`, recomputed on resize and on `XamlRoot::Changed`. The strip carries the window title, which is also where §12.2's "aggregate progress in the title bar" became visible — with the content extended into it, the system draws no caption text. | 12.1 | |
| B8 | Fidelity badge explains on click | **done** | The badge is its own `HyperlinkButton` with a flyout, a sibling of the target button rather than nested inside it — a button inside a button is a hit-testing argument nobody wins. The tooltip stays for the mouse. | 12.2 | |
| B9 | Legacy `IContextMenu` path | open | §9.1 asks for it *"for Windows 10 and for the 'Show more options' menu on Windows 11."* §20.3 excused Windows 10; it did not excuse the Win11 secondary menu, where Morph is currently absent. | 9.1 | ~half a day |
| B10 | Application manifest | **done** | There was none, for any executable, so every process was DPI **unaware** and Windows rendered the window at 96 DPI and bitmap-stretched it. `PerMonitorV2` (which B7 needs to be told about monitor changes at all) plus `longPathAware`, which §15.2 asked for and only `stream.cpp` was honouring. One manifest, all three executables. | 12.1, 15.2 | |

**B2 has a constraint that shapes it.** §3: *"The app and the shell extension are
peers, not layers. Every capability reachable from the UI is reachable from the
context menu and from the worker's CLI surface… If a feature can only be built
into the UI, it is in the wrong place."* So options are protocol and CLI work
first and a settings screen second. The CLI already parses `key=value`; the wire
already carries `OptionValue`. The gap is provider declaration and discovery.

---

## C. Built, and known to be imperfect

| # | Item | Status | Where | Notes |
|---|---|---|---|---|
| C1 | `MORPH-IR` offered as a conversion target | **done** | — | `QueryTargets` now applies the rule the published table always did: a node with no extension of its own is somewhere a plan passes through, not somewhere it can stop. |
| C2 | DOCX continuation paragraphs not reconstructed | open | `tests/property/test_ir_roundtrip.cpp:313` | A list item holding two blocks comes back as an item, a paragraph, and a new list — Word expresses a continuation paragraph as list style + indent without numbering, and the reader closes the list instead. The property generator emits one block per item because of it; fixing this lifts that restriction too. |
| C3 | Sub-lists at one depth cannot disagree | open (won't fix) | `docx_writer.cpp` | A level is one `w:lvl` for the whole tree. Declared with `list-level-markers-shared` / `list-level-start-shared`. A format limit, not a defect — listed so nobody re-derives it. |
| C4 | The app links the whole provider table to probe | **done** | `src/app/main.cpp`, `FileProber` | §12.2 probes on drop; doing it in-process pulls WIC, Media Foundation and PDFium into the UI process. **The backlog used to say "one function to change" and that is wrong**: `kProbeFiles`/`kProbeResult` exist only broker↔worker, so the client contract needs two new message types, a broker handler and a client method. `AppModel`'s injected `Prober` is the seam, and it held: the table is filled on the background thread and the Prober became a lookup. Measured after: pdfium and Media Foundation are gone from the UI process; windowscodecs remains, loaded by XAML for its own decoding. |
| C6 | A file that could not be opened terminated the worker | **done** | `serve.cpp` | `ProbeAll` promised "an unreadable file is an empty string, not a missing element" and did not keep it: `FileSource` throws, nothing caught it, and unwinding past a joinable `std::thread` calls `std::terminate`. One file deleted between the drop and the probe killed the worker mid-batch. Reachable before C4 from the app's own prober, which had no guard either. |
| C7 | The frame reader accepted a *range* of message tags | **done** | `frame.cpp` | `raw >= kHello && raw <= kRunTask` meant "whichever enumerator is numerically last". A new tag outside it decoded to `kInvalid` and was dropped **silently**, leaving the client waiting for a reply. It is a list of cases now. |
| C5 | Dropping files mid-job starts a new selection | open (deliberate) | `AppModel::Drop` | A queue that grows while it runs is a different feature. Revisit if it annoys anyone. |

---

## D. Deliberately out of scope — do not "fix" these

**Read this group before adding anything to the document pipeline.**

| # | Item | Why it stays undone |
|---|---|---|
| D1 | **DOCX P3 and P4** — footnotes, headers/footers, section properties; tracked changes and comments | §8.4: *"Do not attempt full OOXML. Phase it… **Ship after P2.**"* §20 ranks DOCX scope creep as **the single highest risk in the project**: *"Enforce the P1–P4 phasing… and refuse anything above the current phase, no matter how small it looks."* This is not a backlog item, it is a defence. The reader already prices it honestly — its edge cost is `High` rather than `Lossless` *because* P3/P4 are out — and `w:del` is dropped with a note because reinstating deleted text is a confidentiality problem. **If P3 is ever wanted, footnotes are much the cheapest**: the IR already models `FootnoteRef` and `Footnote` (`document.h:82`, `:214`) and no provider produces one yet, so the design work is done and only the DOCX ends are missing. |
| D2 | OCR for scanned PDFs | §1, explicitly out of scope for v1. `pdf_reader.cpp` already says so in the failure message rather than pretending. |
| D3 | Vector formats, CAD, 3D meshes, spreadsheets, presentations, e-books, RAW beyond WIC | §1's "explicitly out of scope for v1" list. Each is a plugin later; none is v1. |
| D4 | Out-of-tree providers / dynamic provider loading | §5.1: *"do not build the dynamic loading in v1. It is the single easiest way to turn a three-month project into a nine-month project."* The interface is already designed so v2 can do it without changing provider code. |
| D5 | The image budget missed by 5× | §17. Measured, understood and written up: the old benchmark fixture was uncorrelated noise, the honest figure against a photograph is ~595 ms, and roughly 87 ms of it is WIC's decoder with the rest one deflate stream over 36 MB. The target stays in the table unedited and the gate watches a baseline beside it. |
| D6 | Broker cold start, 929 ms against a 150 ms target | §17. Both numbers are real: ~76 ms warm, ~929 ms on the first launch after an install, which every user pays exactly once. Reported as two numbers rather than averaged away. |
| D7 | A cleaner for the WebView2 profile in `%TEMP%` | Measured: flat across 20 conversions, and flat across five workers *killed mid-render*, which is the normal exit here. Cold and warm both cost ~0.5 s, so the folder is disposable — a stronger property than pruned. See the note in `webview2_host.cpp`. |

---

## E. Open questions (§20)

| # | Question | Status |
|---|---|---|
| E1 | §20.1 — name and identity | **open, and now the blocker.** "Morph" is a working codename throughout: `morph::`, `Morph.Converter`, the pipe name, and `CN=Morph (placeholder)` in the manifest template — which has to match a signing certificate's subject exactly. A5, A6 and A7 all wait on this, and it is the only remaining item that cannot be started by writing code. |
| E2 | §20.2 — ARM64 | **settled**: x64 only for now. `CMakePresets.json` carries an `arm64-release` preset and an `arch-arm64` fragment, so enabling it is a matrix entry rather than a refactor. CI cannot run ARM64 tests on a `windows-latest` runner. |
| E3 | Licensing of Morph itself | open. MIT assumed throughout. §20 suggests deciding *now* whether the core stays MIT with a separate licence for the app shell, "rather than relicensing later with contributors in the tree". |

---

## F. Measurement and test gaps (§16, §17)

The gates exist and are required checks. These are the holes in them, which is a
different kind of debt: nothing here is a missing feature, and all of it is a
reason to trust the green tick less than it looks.

**§17 has nine budgets. Five are measured.**

| # | Item | Status | Notes |
|---|---|---|---|
| F1 | App cold start to interactive, ≤600 ms | open | Never measured. Now measurable — the app builds by default. |
| F2 | 50-image batch, ≥6× single-file throughput | open | Never measured. Tests the pool sizing in §11.1, which nothing else exercises. |
| F3 | 4 GB MKV → MP4 remux, ≤1.15× file copy | open | Needs a large fixture and a real FFmpeg. **Waits on A3.** |
| F4 | Package size, Store SKU ≤30 MB | open | Measurable against an unsigned `.msix` today. |
| F5 | Broker idle memory measured at 5 s, not 60 | open | The harness prints the caveat itself. Cheap to fix, and 60 s is the number §17 wrote. |
| F6 | Golden-file corpus (§16) | open | §16 wants a document-pair corpus in a separate repo, with byte comparison where ZIP output is deterministic and normalised-IR comparison where it is not. Does not exist. |
| F7 | Perceptual diff for images (§16) | open | §16 asks for SSIM thresholds *"since encoders vary across OS builds"*. The image tests compare exact pixels on synthetic BMPs, which covers lossless round trips and nothing else. |
| F8 | Shell extension under Application Verifier (§16) | open | §16: *"Drive a real Explorer under Application Verifier with the handler loaded; watch for leaks and COM apartment violations."* The new integration test drives `CoCreateInstance`, which is not the same thing. |

---

## Suggested order

1. **E1, the name.** A5's icons, A6's certificate and A7's reservation all wait
   on it, and it is the only item here that cannot be started by writing code.
2. **B4, B6, B9.** The three remaining half-day features. None waits on
   anything, and B9 is the one a user would notice: Morph is absent from
   Windows 11's "Show more options" menu entirely.
3. **F1, F2, F4, F5.** Four of §17's nine budgets are unmeasured and three are
   measurable today. F1, app cold start, is the one with a user waiting in it.
4. **A3, then A6.** The GitHub SKU needs no Store review, and `.appinstaller`
   updates are what make dogfooding real. A3 also unblocks F3.
5. **B2.** Options and presets — the feature §2 says the app exists for.
   Protocol and CLI first, per §3's peers rule.
6. **B5.** Tagged PDF, the largest remaining fidelity win and the hardest to
   verify: plausible code is easy here and knowing it is right is not.

§19's own shipping advice was *"a public v1 at M8 is a real product… ship there,
get users, then land DOCX in v1.1."* This is past M8 **and** past DOCX. The
engine is finished, the package installs and the context menu loads; what is
between this and something people can download is a name, an icon and a
certificate.
