// What the right-click menu decides (architecture sections 9.2 and 9.3).
//
// No COM here, which is the point of splitting the logic out: the questions
// worth asking -- what does a mixed selection offer, when does the menu give up,
// does a lossy target say so -- are answerable without Explorer.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "morph/core/caps.h"
#include "morph/core/fidelity.h"
#include "shellext/menu.h"

using namespace morph;
using namespace morph::shellext;

namespace {

CapabilitySet::Target Target(std::string profile, std::string extension, Fidelity fidelity,
                            std::uint8_t hops = 1) {
    CapabilitySet::Target target;
    target.profile = std::move(profile);
    target.extension = std::move(extension);
    target.fidelity = static_cast<std::uint8_t>(fidelity);
    target.hopCount = hops;
    return target;
}

// A table shaped like the real one: documents that route through the IR, images
// that convert directly.
std::vector<std::byte> SampleTable() {
    CapabilitySet set;

    CapabilitySet::Source docx;
    docx.extension = "docx";
    docx.targets = {Target("text/markdown", "md", Fidelity::Structural, 2),
                    Target("text/html", "html", Fidelity::Lossless, 2),
                    Target("application/pdf", "pdf", Fidelity::High, 3)};

    CapabilitySet::Source md;
    md.extension = "md";
    md.targets = {Target("text/html", "html", Fidelity::High, 2),
                  Target("application/docx", "docx", Fidelity::Lossless, 2),
                  Target("application/pdf", "pdf", Fidelity::High, 3)};

    CapabilitySet::Source png;
    png.extension = "png";
    png.targets = {Target("image/jpeg", "jpg", Fidelity::High),
                   Target("image/tiff", "tiff", Fidelity::Lossless),
                   Target("image/bmp", "bmp", Fidelity::Lossless)};

    CapabilitySet::Source jpg;
    jpg.extension = "jpg";
    jpg.targets = {Target("image/png", "png", Fidelity::Lossless),
                   Target("image/tiff", "tiff", Fidelity::Lossless),
                   Target("image/bmp", "bmp", Fidelity::Lossless)};

    set.sources = {std::move(docx), std::move(md), std::move(png), std::move(jpg)};
    return SerialiseCapabilities(set, 1);
}

std::vector<std::string> Titles(const Menu& menu) {
    std::vector<std::string> titles;
    for (const MenuEntry& entry : menu.entries) titles.push_back(entry.Title());
    return titles;
}

bool Offers(const Menu& menu, std::string_view profile) {
    return std::any_of(menu.entries.begin(), menu.entries.end(),
                       [profile](const MenuEntry& entry) { return entry.profile == profile; });
}

}  // namespace

TEST_CASE("One file offers everything it can become, best first", "[shellext][menu]") {
    const std::vector<std::byte> bytes = SampleTable();
    const CapabilityTable table = CapabilityTable::Attach(bytes.data(), bytes.size());

    const Menu menu = BuildMenu(table, {"docx"});
    REQUIRE_FALSE(menu.generic);
    REQUIRE(menu.entries.size() == 3u);

    // Lossless first, and the fidelity contract visible in the title: section
    // 8.8 wants the user told before they commit, and for a person who never
    // opens the app this menu is the only place it can happen.
    CHECK(Titles(menu) == std::vector<std::string>{"To HTML",
                                                   "To PDF (re-encoded once)",
                                                   "To MD (structure, not layout)"});
}

TEST_CASE("A mixed selection offers only what all of it can do", "[shellext][menu]") {
    const std::vector<std::byte> bytes = SampleTable();
    const CapabilityTable table = CapabilityTable::Attach(bytes.data(), bytes.size());

    // PNG and JPEG share TIFF and BMP. Offering PNG (which only the JPEGs can
    // become) would silently do nothing to half the selection.
    const Menu menu = BuildMenu(table, {"png", "jpg", "PNG", ".jpg"});
    REQUIRE_FALSE(menu.generic);
    CHECK(Offers(menu, "image/tiff"));
    CHECK(Offers(menu, "image/bmp"));
    CHECK_FALSE(Offers(menu, "image/png"));
    CHECK_FALSE(Offers(menu, "image/jpeg"));
}

TEST_CASE("A mixed selection reports the worst rating in it", "[shellext][menu]") {
    CapabilitySet set;
    CapabilitySet::Source a;
    a.extension = "aaa";
    a.targets = {Target("application/pdf", "pdf", Fidelity::Lossless, 1)};
    CapabilitySet::Source b;
    b.extension = "bbb";
    b.targets = {Target("application/pdf", "pdf", Fidelity::Rasterized, 4)};
    set.sources = {std::move(a), std::move(b)};

    const std::vector<std::byte> bytes = SerialiseCapabilities(set, 1);
    const CapabilityTable table = CapabilityTable::Attach(bytes.data(), bytes.size());

    const Menu menu = BuildMenu(table, {"aaa", "bbb"});
    REQUIRE(menu.entries.size() == 1u);
    // The promise has to hold for every file in the selection, so the worst one
    // sets the label.
    CHECK(menu.entries[0].Title() == "To PDF (as pixels)");
    CHECK(menu.entries[0].hopCount == 4u);
}

TEST_CASE("A selection with nothing in common falls back to the generic entry",
          "[shellext][menu]") {
    const std::vector<std::byte> bytes = SampleTable();
    const CapabilityTable table = CapabilityTable::Attach(bytes.data(), bytes.size());

    // A document and an image share no target.
    const Menu menu = BuildMenu(table, {"docx", "png"});
    CHECK(menu.generic);
    CHECK(menu.entries.empty());
}

TEST_CASE("A file Morph does not handle makes the menu generic", "[shellext][menu]") {
    const std::vector<std::byte> bytes = SampleTable();
    const CapabilityTable table = CapabilityTable::Attach(bytes.data(), bytes.size());

    CHECK(BuildMenu(table, {"xlsx"}).generic);
    // One unconvertible file in an otherwise convertible selection: offering the
    // menu would convert some of the selection and quietly skip the rest.
    CHECK(BuildMenu(table, {"png", "xlsx"}).generic);
}

TEST_CASE("Too many kinds of file in one selection is one entry, not forty",
          "[shellext][menu]") {
    // Section 9.3: "if a selection spans more than, say, six distinct input
    // families, show the generic 'Convert...' entry rather than a submenu with
    // forty children."
    CapabilitySet set;
    for (const char* extension : {"e1", "e2", "e3", "e4", "e5", "e6", "e7"}) {
        CapabilitySet::Source source;
        source.extension = extension;
        source.targets = {Target("application/pdf", "pdf", Fidelity::High)};
        set.sources.push_back(std::move(source));
    }
    const std::vector<std::byte> bytes = SerialiseCapabilities(set, 1);
    const CapabilityTable table = CapabilityTable::Attach(bytes.data(), bytes.size());

    CHECK_FALSE(BuildMenu(table, {"e1", "e2", "e3", "e4", "e5", "e6"}).generic);
    CHECK(BuildMenu(table, {"e1", "e2", "e3", "e4", "e5", "e6", "e7"}).generic);

    // Two hundred photographs are one question, not two hundred: duplicates do
    // not count toward the cap.
    const std::vector<std::string> many(200, "e1");
    CHECK_FALSE(BuildMenu(table, many).generic);
}

TEST_CASE("No table means the static entry, never a crash", "[shellext][menu]") {
    // Section 9.2: "If the map is absent -- broker never ran, or version
    // mismatch -- the extension shows a single static entry. It degrades, it
    // never hangs."
    const CapabilityTable nothing;
    CHECK(BuildMenu(nothing, {"png"}).generic);

    const std::vector<std::byte> bytes = SampleTable();
    const CapabilityTable table = CapabilityTable::Attach(bytes.data(), bytes.size());
    CHECK(BuildMenu(table, {}).generic);
    CHECK(BuildMenu(table, {""}).generic);
}

TEST_CASE("The menu is capped at a readable length", "[shellext][menu]") {
    CapabilitySet set;
    CapabilitySet::Source source;
    source.extension = "src";
    for (int i = 0; i < 20; ++i) {
        source.targets.push_back(Target("application/x-" + std::to_string(i),
                                        "t" + std::to_string(i),
                                        i < 3 ? Fidelity::Lossless : Fidelity::Rasterized));
    }
    set.sources.push_back(std::move(source));

    const std::vector<std::byte> bytes = SerialiseCapabilities(set, 1);
    const CapabilityTable table = CapabilityTable::Attach(bytes.data(), bytes.size());

    const Menu menu = BuildMenu(table, {"src"});
    REQUIRE_FALSE(menu.generic);
    CHECK(menu.entries.size() == kMaxTargets);
    // What survives the cap is the best of them, not the first eight the table
    // happened to store.
    CHECK(menu.entries.front().fidelity == static_cast<std::uint8_t>(Fidelity::Lossless));
}
