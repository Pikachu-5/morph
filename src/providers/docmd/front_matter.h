// YAML front matter for the Markdown reader.
//
// Not a YAML parser, and not trying to be. Our own writer emits exactly this
// shape for document metadata, and without a reader for it a md -> docx -> md
// round trip turns the block into a setext heading reading
// "title: Reportauthor: A. Person".
//
// A writer that emits something its own reader cannot read is a bug regardless
// of what any specification says -- and CommonMark says nothing about front
// matter at all, so there is no specification to defer to here.
//
// Only the keys the writer produces are interpreted. Anything else is kept in
// Metadata::extra so it survives a round trip rather than being discarded, which
// is design rule 1 applied to metadata.
#pragma once

#include <string>
#include <string_view>

#include "morph/core/doc/document.h"

namespace morph::docmd {

// Returns the text with any front-matter block removed, having read what it
// understood into `meta`. Returns the input unchanged when there is no block.
inline std::string_view StripFrontMatter(std::string_view text, doc::Metadata& meta) {
    if (!text.starts_with("---")) return text;

    // The opening fence has to be a line of its own.
    const std::size_t firstBreak = text.find('\n');
    if (firstBreak == std::string_view::npos) return text;
    if (text.substr(0, firstBreak).find_first_not_of("-\r") != std::string_view::npos) {
        return text;
    }

    // Find the closing fence: a line that is exactly "---" or "...".
    std::size_t end = std::string_view::npos;
    for (std::size_t at = firstBreak + 1; at < text.size();) {
        const std::size_t lineEnd = (std::min)(text.find('\n', at), text.size());
        std::string_view line = text.substr(at, lineEnd - at);
        while (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        if (line == "---" || line == "...") {
            end = lineEnd < text.size() ? lineEnd + 1 : text.size();
            break;
        }
        at = lineEnd + 1;
    }
    // No closing fence means this was never front matter. Most likely it is a
    // horizontal rule followed by prose, which must be left completely alone.
    if (end == std::string_view::npos) return text;

    // A CLOSING FENCE IS NOT ENOUGH. A document that opens with a thematic break
    // and has another one further down looks exactly like front matter from
    // here, and treating it as such deletes everything between the two rules --
    // which is most of the document. So the block also has to LOOK like YAML:
    // every non-blank line is "key: value", or a "- item" under one.
    {
        bool sawKey = false;
        for (std::size_t scan = firstBreak + 1; scan < end;) {
            const std::size_t lineEnd = (std::min)(text.find('\n', scan), end);
            std::string_view line = text.substr(scan, lineEnd - scan);
            scan = lineEnd + 1;

            while (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.empty() || line == "---" || line == "...") continue;

            const std::size_t firstText = line.find_first_not_of(" \t");
            if (firstText == std::string_view::npos) continue;
            if (line[firstText] == '-' && sawKey) continue;  // a sequence item
            if (line.find(':') == std::string_view::npos) return text;
            sawKey = true;
        }
        if (!sawKey) return text;
    }

    std::string pendingKey;
    for (std::size_t scan = firstBreak + 1; scan < end;) {
        const std::size_t lineEnd = (std::min)(text.find('\n', scan), end);
        std::string_view line = text.substr(scan, lineEnd - scan);
        scan = lineEnd + 1;

        while (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line == "---" || line == "...") continue;

        const std::size_t firstText = line.find_first_not_of(" \t");
        if (firstText == std::string_view::npos) continue;

        // A sequence item belonging to the key on the line above: "  - Someone".
        if (line[firstText] == '-' && !pendingKey.empty()) {
            std::string_view value = line.substr(firstText + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            if (!value.empty() && (pendingKey == "authors" || pendingKey == "author")) {
                meta.authors.emplace_back(value);
            }
            continue;
        }

        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) continue;

        std::string key(line.substr(firstText, colon - firstText));
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        if (key.empty()) continue;

        std::string_view value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }

        // A key with no value introduces a sequence on the lines that follow.
        pendingKey = value.empty() ? key : std::string();
        if (value.empty()) continue;

        if (key == "title") {
            meta.title = std::string(value);
        } else if (key == "author") {
            meta.authors.emplace_back(value);
        } else if (key == "lang" || key == "language") {
            meta.lang = std::string(value);
        } else if (key == "date") {
            meta.created = std::string(value);
        } else {
            meta.extra.emplace(std::move(key), std::string(value));
        }
    }

    return text.substr(end);
}

}  // namespace morph::docmd
