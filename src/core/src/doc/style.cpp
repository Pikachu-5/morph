#include "morph/core/doc/style.h"

#include <array>
#include <cctype>
#include <charconv>

namespace morph::doc {
namespace {

std::optional<std::uint8_t> HexByte(std::string_view two) {
    if (two.size() != 2) return std::nullopt;
    unsigned value = 0;
    const auto* begin = two.data();
    const auto [ptr, ec] = std::from_chars(begin, begin + 2, value, 16);
    if (ec != std::errc{} || ptr != begin + 2) return std::nullopt;
    return static_cast<std::uint8_t>(value);
}

char HexDigit(unsigned v) { return static_cast<char>(v < 10 ? '0' + v : 'a' + (v - 10)); }

void AppendHex(std::string& out, std::uint8_t v) {
    out.push_back(HexDigit(v >> 4));
    out.push_back(HexDigit(v & 0x0Fu));
}

// `over` wins where it is set; `under` fills the gaps.
template <typename T>
void UnderlayField(std::optional<T>& over, const std::optional<T>& under) {
    if (!over.has_value()) over = under;
}

}  // namespace

std::string Colour::ToHex() const {
    std::string out;
    out.reserve(9);
    out.push_back('#');
    AppendHex(out, r);
    AppendHex(out, g);
    AppendHex(out, b);
    if (a != 255) AppendHex(out, a);
    return out;
}

std::optional<Colour> Colour::Parse(std::string_view text) {
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);

    // OOXML writes "auto" for "let the consumer decide", which is genuinely
    // unset. Returning black here would silently invent a colour that the
    // document never specified, and rule 2 forbids inventing values.
    if (text == "auto" || text.empty()) return std::nullopt;

    Colour c;
    if (text.size() == 3) {
        // #rgb shorthand, each digit doubled.
        for (int i = 0; i < 3; ++i) {
            const std::array<char, 2> pair{text[static_cast<std::size_t>(i)],
                                           text[static_cast<std::size_t>(i)]};
            const auto v = HexByte(std::string_view(pair.data(), 2));
            if (!v) return std::nullopt;
            (i == 0 ? c.r : i == 1 ? c.g : c.b) = *v;
        }
        return c;
    }

    if (text.size() != 6 && text.size() != 8) return std::nullopt;

    const auto r = HexByte(text.substr(0, 2));
    const auto g = HexByte(text.substr(2, 2));
    const auto b = HexByte(text.substr(4, 2));
    if (!r || !g || !b) return std::nullopt;
    c.r = *r;
    c.g = *g;
    c.b = *b;

    if (text.size() == 8) {
        const auto a = HexByte(text.substr(6, 2));
        if (!a) return std::nullopt;
        c.a = *a;
    }
    return c;
}

bool TextStyle::Empty() const noexcept {
    return !fontFamily && !fontSizePt && !colour && !highlight && !underline && !vertical &&
           !smallCaps && !letterSpacingPt;
}

void TextStyle::Underlay(const TextStyle& under) {
    UnderlayField(fontFamily, under.fontFamily);
    UnderlayField(fontSizePt, under.fontSizePt);
    UnderlayField(colour, under.colour);
    UnderlayField(highlight, under.highlight);
    UnderlayField(underline, under.underline);
    UnderlayField(vertical, under.vertical);
    UnderlayField(smallCaps, under.smallCaps);
    UnderlayField(letterSpacingPt, under.letterSpacingPt);
}

bool ParagraphStyle::Empty() const noexcept {
    return !alignment && !spaceBeforePt && !spaceAfterPt && !lineHeight && !indentLeftPt &&
           !indentRightPt && !indentFirstLinePt && !keepWithNext && text.Empty();
}

void ParagraphStyle::Underlay(const ParagraphStyle& under) {
    UnderlayField(alignment, under.alignment);
    UnderlayField(spaceBeforePt, under.spaceBeforePt);
    UnderlayField(spaceAfterPt, under.spaceAfterPt);
    UnderlayField(lineHeight, under.lineHeight);
    UnderlayField(indentLeftPt, under.indentLeftPt);
    UnderlayField(indentRightPt, under.indentRightPt);
    UnderlayField(indentFirstLinePt, under.indentFirstLinePt);
    UnderlayField(keepWithNext, under.keepWithNext);
    text.Underlay(under.text);
}

void StyleTable::Set(std::string id, NamedStyle style) {
    styles_.insert_or_assign(std::move(id), std::move(style));
}

const NamedStyle* StyleTable::Find(std::string_view id) const {
    const auto it = styles_.find(id);
    return it == styles_.end() ? nullptr : &it->second;
}

}  // namespace morph::doc
