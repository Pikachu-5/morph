// The document IR and its three design rules (architecture section 8.2).
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "morph/core/doc/document.h"
#include "morph/core/doc/pipeline.h"

using namespace morph;
using namespace morph::doc;

namespace {

std::vector<std::byte> Bytes(std::string_view text) {
    return std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                  reinterpret_cast<const std::byte*>(text.data()) + text.size());
}

Block Para(std::string text) {
    return Block::Of(Paragraph{{Inline::Of(Text{std::move(text), {}})}, {}, {}, std::nullopt});
}

}  // namespace

// --- Rule 2: styles resolved on read ----------------------------------------

TEST_CASE("Underlay fills gaps without overwriting", "[ir][style]") {
    // This is the whole of style resolution: apply docDefaults, then each
    // basedOn ancestor from the root down, then direct formatting last.
    TextStyle direct;
    direct.fontSizePt = 14.0f;

    TextStyle inherited;
    inherited.fontSizePt = 11.0f;
    inherited.fontFamily = "Calibri";

    direct.Underlay(inherited);

    CHECK(direct.fontSizePt == 14.0f);      // direct wins
    CHECK(direct.fontFamily == "Calibri");  // gap filled
}

TEST_CASE("an unset style is empty, not defaulted", "[ir][style]") {
    // An unset optional means "the producing format did not specify this". A
    // writer must be able to tell that from "specified as zero".
    const TextStyle unset;
    CHECK(unset.Empty());

    TextStyle explicit_zero;
    explicit_zero.letterSpacingPt = 0.0f;
    CHECK_FALSE(explicit_zero.Empty());
}

TEST_CASE("OOXML auto colour stays unset", "[ir][style]") {
    // Returning black here would invent a colour the document never specified.
    CHECK_FALSE(Colour::Parse("auto").has_value());
    CHECK_FALSE(Colour::Parse("").has_value());

    const auto red = Colour::Parse("FF0000");
    REQUIRE(red.has_value());
    CHECK(red->r == 0xFF);
    CHECK(red->ToHex() == "#ff0000");
}

TEST_CASE("colour parsing accepts the forms our formats actually write",
          "[ir][style]") {
    CHECK(Colour::Parse("#abc")->ToHex() == "#aabbcc");
    CHECK(Colour::Parse("#AABBCC")->ToHex() == "#aabbcc");
    CHECK(Colour::Parse("#aabbcc80")->ToHex() == "#aabbcc80");
    CHECK_FALSE(Colour::Parse("#gg0000").has_value());
    CHECK_FALSE(Colour::Parse("#12345").has_value());
}

// --- The resource store ------------------------------------------------------

TEST_CASE("identical bytes are stored once", "[ir][resource]") {
    // A logo in a header on forty pages arrives forty times.
    ResourceStore store;
    const auto png = Bytes("\x89PNG\r\n\x1a\n....payload....");

    const ResourceId a = store.Add(png, "image/png", "logo.png");
    const ResourceId b = store.Add(png, "image/png", "header-image.png");

    CHECK(a == b);
    CHECK(store.Size() == 1);
    CHECK(store.Find(a)->hint == "logo.png");  // first insert's metadata wins
}

TEST_CASE("different bytes get different ids", "[ir][resource]") {
    ResourceStore store;
    store.Add(Bytes("one"), "image/png");
    store.Add(Bytes("two"), "image/png");
    CHECK(store.Size() == 2);
}

TEST_CASE("the content hash is stable and hex", "[ir][resource]") {
    const std::string a = ContentHash(Bytes("morph"));
    const std::string b = ContentHash(Bytes("morph"));
    CHECK(a == b);
    CHECK(a.size() == 32);
    CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(ContentHash(Bytes("morph")) != ContentHash(Bytes("morpg")));
}

TEST_CASE("image type is sniffed from bytes, never from a name", "[ir][resource]") {
    CHECK(SniffImageMime(Bytes("\x89PNG\r\n\x1a\n")) == "image/png");
    CHECK(SniffImageMime(Bytes("\xFF\xD8\xFF\xE0")) == "image/jpeg");
    CHECK(SniffImageMime(Bytes("GIF89a")) == "image/gif");
    CHECK(SniffImageMime(Bytes("BM....")) == "image/bmp");
    CHECK(SniffImageMime(Bytes("RIFF____WEBPVP8 ")) == "image/webp");
    CHECK(SniffImageMime(Bytes("____ftypavif")) == "image/avif");
    CHECK(SniffImageMime(Bytes("____ftypheic")) == "image/heif");
    CHECK(SniffImageMime(Bytes("not an image")).empty());
}

TEST_CASE("PNG dimensions are read from the header", "[ir][resource]") {
    std::string png = "\x89PNG\r\n\x1a\n";
    png += std::string(4, '\0');       // IHDR length
    png += "IHDR";
    png += std::string({'\0', '\0', '\x02', '\x00'});  // width 512
    png += std::string({'\0', '\0', '\x01', '\x80'});  // height 384

    const PixelSize size = SniffImageSize(Bytes(png));
    CHECK(size.width == 512);
    CHECK(size.height == 384);
}

TEST_CASE("an unrecognised image reports no size rather than guessing",
          "[ir][resource]") {
    // {0,0} means "ask the decoder", which is exactly right for HEIF and TIFF.
    const PixelSize size = SniffImageSize(Bytes("____ftypheic and then some"));
    CHECK(size.width == 0);
    CHECK(size.height == 0);
}

TEST_CASE("Retain drops what nothing references", "[ir][resource]") {
    ResourceStore store;
    const ResourceId kept = store.Add(Bytes("kept"), "image/png");
    store.Add(Bytes("orphan"), "image/png");
    REQUIRE(store.Size() == 2);

    store.Retain({kept});
    CHECK(store.Size() == 1);
    CHECK(store.Contains(kept));
}

TEST_CASE("extensions are never guessed from a filename", "[ir][resource]") {
    CHECK(ExtensionForMime("image/png") == ".png");
    CHECK(ExtensionForMime("image/jpeg") == ".jpg");
    CHECK(ExtensionForMime("application/x-unknown") == ".bin");
}

// --- Traversal ---------------------------------------------------------------

TEST_CASE("traversal reaches blocks nested in every container", "[ir][traverse]") {
    // A container this misses is a container writers will silently drop.
    Blocks blocks;
    blocks.push_back(Para("top"));
    blocks.push_back(Block::Of(BlockQuote{{Para("in quote")}}));
    blocks.push_back(Block::Of(ListBlock{false, 1, true, {ListItem{{Para("in list")}, {}}}}));
    blocks.push_back(Block::Of(Footnote{"1", {Para("in footnote")}}));

    Table table;
    table.rows.push_back(TableRow{{TableCell{{Para("in cell")}, 1, 1, std::nullopt, false}}});
    blocks.push_back(Block::Of(std::move(table)));

    std::vector<std::string> seen;
    ForEachBlock(blocks, [&](const Block& block) {
        if (const auto* p = block.As<Paragraph>()) {
            seen.push_back(p->inlines.front().As<Text>()->text);
        }
    });

    CHECK(seen == std::vector<std::string>{"top", "in quote", "in list", "in footnote",
                                           "in cell"});
}

TEST_CASE("referenced resources are collected in first-appearance order",
          "[ir][traverse]") {
    Document document;
    const ResourceId first = document.res.Add(Bytes("a"), "image/png");
    const ResourceId second = document.res.Add(Bytes("b"), "image/png");
    document.res.Add(Bytes("unused"), "image/png");

    document.blocks.push_back(Block::Of(ImageBlock{first, "alt", "", 0, 0}));
    document.blocks.push_back(Block::Of(Paragraph{
        {Inline::Of(InlineImage{second, "alt", "", 0, 0})}, {}, {}, std::nullopt}));
    // A second reference to the same resource must not appear twice.
    document.blocks.push_back(Block::Of(ImageBlock{first, "again", "", 0, 0}));

    CHECK(ReferencedResources(document) == std::vector<ResourceId>{first, second});
}

TEST_CASE("depth and count are measured for the security caps", "[ir][traverse]") {
    Blocks inner{Para("deep")};
    for (int i = 0; i < 5; ++i) inner = Blocks{Block::Of(BlockQuote{std::move(inner)})};

    CHECK(MaxDepth(inner) == 6);
    CHECK(CountBlocks(inner) == 6);
    CHECK(MaxDepth({Para("flat")}) == 1);
}

TEST_CASE("plain text extracts words and nothing else", "[ir][traverse]") {
    Blocks blocks;
    Paragraph paragraph;
    paragraph.inlines.push_back(Inline::Of(Text{"plain ", {}}));
    paragraph.inlines.push_back(Inline::Of(Strong{{Inline::Of(Text{"bold", {}})}}));
    paragraph.inlines.push_back(Inline::Of(Text{" and ", {}}));
    paragraph.inlines.push_back(
        Inline::Of(Link{"https://x", "", {Inline::Of(Text{"a link", {}})}}));
    blocks.push_back(Block::Of(std::move(paragraph)));

    CHECK(PlainText(blocks) == "plain bold and a link\n");
}

// --- Rule 3: the pressure valve ---------------------------------------------

TEST_CASE("a RawBlock keeps its bytes untouched", "[ir][raw]") {
    const std::string original = "<w:sdt><w:sdtPr/></w:sdt>";
    RawBlock raw{"ooxml", Bytes(original), "content control"};

    CHECK(std::string(reinterpret_cast<const char*>(raw.bytes.data()), raw.bytes.size()) ==
          original);
    CHECK(raw.description == "content control");
}

// --- The pipeline seam -------------------------------------------------------

TEST_CASE("the IR profile is a real graph node", "[ir][pipeline]") {
    const MediaProfile ir = IrProfile();
    CHECK(ir.Id() == "application/morph-ir");
    CHECK(ir.Family() == "application");
}

TEST_CASE("a document provider must have a reader or a writer", "[ir][pipeline]") {
    CHECK_THROWS_AS(DocumentProvider("x", nullptr, nullptr), std::invalid_argument);
    CHECK_THROWS_AS(DocumentProvider("", nullptr, nullptr), std::invalid_argument);
}
