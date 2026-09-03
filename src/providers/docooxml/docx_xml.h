// Internal helpers for walking WordprocessingML with pugixml.
//
// TWO THINGS THAT BITE, both handled here so that no other file has to think
// about them.
//
// NAMESPACE PREFIXES. pugixml has no namespace support: it sees "w:p" as an
// element whose name is literally "w:p". Almost every producer uses the "w"
// prefix for the main namespace, but nothing requires it -- the prefix is
// arbitrary and bound by an xmlns declaration. Matching on the LOCAL name (the
// part after the colon) is both simpler to read and correct for the producers
// that do something else.
//
// UNITS. OOXML measures the same document in three different units and mixes
// them freely. Getting one wrong produces a document that looks almost right,
// which is the worst kind of wrong.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <pugixml.hpp>

#include "morph/providers/opc.h"

namespace morph::docooxml {

// Parse flags for every OOXML part, defined by the OPC layer that owns XML
// parsing for this provider.
using opc::kOoxmlParseFlags;

// --- Namespace-agnostic access ----------------------------------------------

[[nodiscard]] inline std::string_view LocalName(std::string_view qualified) {
    const auto colon = qualified.find(':');
    return colon == std::string_view::npos ? qualified : qualified.substr(colon + 1);
}

[[nodiscard]] inline std::string_view LocalName(const pugi::xml_node& node) {
    return LocalName(std::string_view(node.name()));
}

// First child element with this local name, or an empty node.
[[nodiscard]] inline pugi::xml_node Child(const pugi::xml_node& parent, std::string_view local) {
    for (pugi::xml_node child = parent.first_child(); child; child = child.next_sibling()) {
        if (LocalName(child) == local) return child;
    }
    return {};
}

// Walks a chain of local names: Descend(style, "pPr", "ind") is
// style/w:pPr/w:ind however those elements happen to be prefixed.
template <typename... Names>
[[nodiscard]] pugi::xml_node Descend(const pugi::xml_node& parent, std::string_view first,
                                     Names... rest) {
    const pugi::xml_node child = Child(parent, first);
    if constexpr (sizeof...(rest) == 0) {
        return child;
    } else {
        return child ? Descend(child, rest...) : pugi::xml_node{};
    }
}

[[nodiscard]] inline std::string Attr(const pugi::xml_node& node, std::string_view local) {
    for (pugi::xml_attribute attribute = node.first_attribute(); attribute;
         attribute = attribute.next_attribute()) {
        if (LocalName(std::string_view(attribute.name())) == local) return attribute.value();
    }
    return {};
}

// The overwhelmingly common shape: <w:b w:val="0"/>, or <w:b/> meaning true.
// An absent element means "not specified", which is NOT the same as false and is
// why this returns an optional (design rule 2 in the IR).
[[nodiscard]] inline std::optional<bool> OnOffValue(const pugi::xml_node& node) {
    if (!node) return std::nullopt;
    const std::string value = Attr(node, "val");
    if (value.empty()) return true;  // present with no val is on
    return value != "0" && value != "false" && value != "off";
}

[[nodiscard]] inline std::optional<bool> OnOff(const pugi::xml_node& parent,
                                               std::string_view local) {
    return OnOffValue(Child(parent, local));
}

// Reads a signed integer attribute, returning nullopt when absent or malformed.
// Never throws and never guesses: a malformed value is "not specified".
[[nodiscard]] inline std::optional<std::int64_t> IntAttr(const pugi::xml_node& node,
                                                         std::string_view local) {
    const std::string value = Attr(node, local);
    if (value.empty()) return std::nullopt;

    std::size_t consumed = 0;
    std::int64_t parsed = 0;
    try {
        parsed = std::stoll(value, &consumed);
    } catch (...) {
        return std::nullopt;
    }
    if (consumed != value.size()) return std::nullopt;
    return parsed;
}

// --- Units -------------------------------------------------------------------
//
// A twip is a twentieth of a point. An EMU is an English Metric Unit, 914400 to
// the inch, chosen so that both inches and centimetres divide it exactly. Font
// sizes are in HALF-points, which is the one that catches everybody: w:sz
// val="22" is 11pt, not 22pt.

[[nodiscard]] inline float TwipsToPoints(std::int64_t twips) {
    return static_cast<float>(twips) / 20.0f;
}

[[nodiscard]] inline std::int64_t PointsToTwips(float points) {
    return static_cast<std::int64_t>(points * 20.0f + (points < 0 ? -0.5f : 0.5f));
}

[[nodiscard]] inline float EmuToPoints(std::int64_t emu) {
    return static_cast<float>(emu) / 12700.0f;  // 914400 EMU/inch / 72 pt/inch
}

[[nodiscard]] inline std::int64_t PointsToEmu(float points) {
    return static_cast<std::int64_t>(points * 12700.0f + (points < 0 ? -0.5f : 0.5f));
}

[[nodiscard]] inline float HalfPointsToPoints(std::int64_t halfPoints) {
    return static_cast<float>(halfPoints) / 2.0f;
}

[[nodiscard]] inline std::int64_t PointsToHalfPoints(float points) {
    return static_cast<std::int64_t>(points * 2.0f + (points < 0 ? -0.5f : 0.5f));
}

// --- XML writing -------------------------------------------------------------

[[nodiscard]] inline std::string XmlEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                // XML 1.0 forbids most control characters outright. A document
                // carrying one is either damaged or hostile, and emitting it
                // would produce a file Word refuses to open.
                if (static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n' && c != '\r') {
                    break;
                }
                out.push_back(c);
                break;
        }
    }
    return out;
}

}  // namespace morph::docooxml
