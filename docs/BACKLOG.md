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
milestone remaining, and it is now mostly done**. 448 tests green in `x64-debug`
and under AddressSanitizer.

What closed since the last revision: A1 (the SDK is pinned and fetched), A2 (it
is in the licence matrix), A4 (one manifest template, and Explorer has now
actually loaded the shell extension), B1 and C1. What is left of M12 is the
FFmpeg fetch, real assets, the two SKUs, and a signing certificate — which is
the part that cannot start until §20.1's naming question is answered.

---

## A. M12 — packaging and distribution (§13)

The only milestone left, and the gate on everything else being real. The shell
extension is §2's "daily-driver path"; it needed package identity before
Explorer could load it, and that is done — what remains here is distribution
rather than capability.

| # | Item | Notes | Size |
|---|---|---|---|
| ~~A1~~ | ~~`tools/fetch-winappsdk.ps1`~~ | **Done.** Five packages, pinned by version and SHA-256 in `third_party/winappsdk-packages.txt`, which the fetcher, `src/app/winappsdk.cmake` and the licence scanner all read. `MORPH_BUILD_APP` is `ON`; `third_party` is searched before the local NuGet cache; CI caches, fetches, asserts the executable exists and uploads the staged layout. | |
| ~~A2~~ | ~~Windows App SDK in §14's licence matrix~~ | **Done.** Three of the five are Microsoft Software License Terms, accepted as platform components with the reason printed rather than by widening the allowlist. A pinned package that is not fetched fails the scan. | |
| A3 | FFmpeg pinned fetch (GitHub SKU) | §7.2/§7.3: same build, two SKUs, distinguished by whether `ffmpeg.exe` is present. No binary is fetched yet. Pin + checksum, same shape as A1 — and the pin manifest is now a pattern to copy rather than invent. | ~half a day |
| ~~A4~~ | ~~Shipping `AppxManifest.xml`, with the shell extension~~ | **Done, less the signing.** One template (`AppxManifest.xml.in`) generated twice; the CLSID comes from CMake and a test compares it with the constant the DLL was compiled with. Registered, and `tests/integration/test_shellext_registered.cpp` drives the menu through the surrogate the way Explorer does. The shipping manifest packs into a real `.msix` with `makeappx`. | |
| A5 | Real package assets | `packaging/msix/Assets/*.png` are solid-colour placeholders generated to make registration succeed. Needs a real icon set. | design work |
| A6 | Both SKUs | `packaging/store/` and `packaging/github/` are empty directories. Store bundle config; GitHub `.appinstaller` for updates. Signing needs a certificate, which needs E1. | ~1–2 days |
| A7 | Store submission | §20 flags certification of a context-menu extension as a medium risk with slow review; the advice there is to submit a minimal build early to get the review clock started. | unknown |

**Learned while doing A4, and worth not rediscovering.** A *packaged* surrogate
and an *unpackaged* broker do not share the capability table: identity puts the
surrogate in a different named-object namespace, so a broker started from a
shell prompt publishes where the surrogate cannot see it, and the menu shows
§9.2's generic entry for a reason that has nothing to do with the selection.
Nothing shipped is affected. To see a populated submenu by hand, start the
broker *through* the package (`Start-Process 'shell:AppsFolder\<PFN>!Morph'`).

Warm cost of `GetState` + `EnumSubCommands` across the surrogate, debug build:
**399 µs** against §17's 8 ms. Cold is 23 ms, which is the surrogate being
created, and is paid once per Explorer session.

---

## B. Features the architecture asks for, not yet built

| # | Item | Where | Section | Size |
|---|---|---|---|---|
| ~~B1~~ | ~~The generic entry should launch the app with the selection~~ | **Done.** `src/shellext/launch.h` builds the command line, tested against the real `CommandLineToArgvW`; a selection too long to fit is refused rather than truncated. | §9.2 | |
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
| ~~C1~~ | ~~`MORPH-IR` is offered as a conversion target~~ | — | **Done.** `QueryTargets` now applies the rule the published menu table always did: a node with no extension of its own is somewhere a plan passes through, not somewhere it can stop. |
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
| E1 | §20.1 — name and identity | **Now the blocker.** "Morph" is a working codename throughout: `morph::`, `Morph.Converter`, the pipe name, and the placeholder `CN=Morph (placeholder)` in the manifest template — which has to match a signing certificate's subject exactly. Trademark search and Store name reservation still to do. A5, A6 and A7 all wait on this, and it is the only remaining item that cannot be started by writing code. |
| E2 | §20.2 — ARM64 | Settled as x64 only for now. `CMakePresets.json` carries an `arm64-release` preset and an `arch-arm64` fragment, so enabling it is a matrix entry rather than a refactor. CI cannot run ARM64 tests on a `windows-latest` runner, which is why it is not already on. |
| E3 | Licensing of Morph itself | MIT assumed throughout. §20 suggests deciding *now* whether the core stays MIT with a separate licence for the app shell, "rather than relicensing later with contributors in the tree". |

---

## Suggested order

1. **E1, the name.** It has moved from a question to a blocker. A5's icon set,
   A6's signing certificate and A7's Store reservation all wait on it, and it
   is the only item here that cannot be started by writing code.
2. **A6, then A3.** The two SKUs, with the GitHub one first: it needs no Store
   review, and `.appinstaller` updates are what make dogfooding real. A3 is the
   FFmpeg pin that the GitHub SKU is defined by.
3. **A7.** Submit early, per §20's advice about the review clock.
4. **B2 and B3.** Options, presets and output policy — the feature §2 says the
   app exists for. Protocol and CLI first, per §3's peers rule.
5. **B5.** Tagged PDF, the largest remaining fidelity win.

§19's own shipping advice was *"a public v1 at M8 is a real product… ship there,
get users, then land DOCX in v1.1."* This is past M8 **and** past DOCX. The
engine is finished, the package installs and the context menu loads; what is
between this and something people can download is a name, an icon and a
certificate.
