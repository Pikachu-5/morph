#include "morph/providers/opc.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>

#include <miniz.h>
#include <pugixml.hpp>

namespace morph::opc {
namespace {

std::string ToLowerAscii(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view Extension(std::string_view partName) {
    const auto dot = partName.find_last_of('.');
    const auto slash = partName.find_last_of('/');
    if (dot == std::string_view::npos) return {};
    if (slash != std::string_view::npos && dot < slash) return {};
    return partName.substr(dot + 1);
}

std::string XmlEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}

std::vector<std::byte> ToBytes(std::string_view text) {
    return std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                  reinterpret_cast<const std::byte*>(text.data()) + text.size());
}

// The state an inflate callback needs in order to enforce the caps as it goes
// rather than after the fact. Allocating on the declared uncompressed size is
// exactly the zip-bomb bug; this grows as bytes actually arrive.
struct InflateSink {
    std::vector<std::byte>* out = nullptr;
    std::uint64_t limit = 0;
    bool exceeded = false;
};

std::size_t OnInflated(void* opaque, std::uint64_t /*offset*/, const void* data,
                       std::size_t size) {
    auto* sink = static_cast<InflateSink*>(opaque);
    if (sink->exceeded) return 0;
    if (sink->out->size() + size > sink->limit) {
        sink->exceeded = true;
        return 0;  // a short write aborts the extraction
    }
    const auto* bytes = static_cast<const std::byte*>(data);
    sink->out->insert(sink->out->end(), bytes, bytes + size);
    return size;
}

}  // namespace

std::string Package::Canonicalise(std::string_view partName) {
    if (partName.empty()) return {};

    // A backslash is never a separator in OPC and is the classic way to smuggle
    // a Windows path past a check that only looks for '/'.
    if (partName.find('\\') != std::string_view::npos) return {};

    // A drive letter or a UNC prefix means this is an absolute filesystem path,
    // not a part name.
    if (partName.size() >= 2 && partName[1] == ':') return {};

    std::vector<std::string_view> segments;
    std::size_t at = (partName.front() == '/') ? 1 : 0;

    while (at <= partName.size()) {
        const auto end = std::min(partName.find('/', at), partName.size());
        const std::string_view segment = partName.substr(at, end - at);
        at = end + 1;

        if (segment.empty() || segment == ".") continue;
        // Rejected rather than resolved. A part name that needs to climb out of
        // its own directory is either malformed or an attack, and neither
        // deserves a best effort.
        if (segment == "..") return {};
        // Reserved device names, per section 15.2's path handling rule.
        const std::string upper = [&segment] {
            std::string s(segment);
            const auto dot = s.find('.');
            if (dot != std::string::npos) s.resize(dot);
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return s;
        }();
        static constexpr std::string_view kReserved[] = {
            "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6",
            "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7",
            "LPT8", "LPT9"};
        if (std::find(std::begin(kReserved), std::end(kReserved), upper) != std::end(kReserved)) {
            return {};
        }
        segments.push_back(segment);
    }

    if (segments.empty()) return {};

    std::string out;
    for (const std::string_view segment : segments) {
        out.push_back('/');
        out.append(segment);
    }
    return out;
}

std::string Package::RelationshipPartFor(std::string_view partName) {
    const std::string canonical = Canonicalise(partName);
    if (canonical.empty()) return "/_rels/.rels";  // the package root

    const auto slash = canonical.find_last_of('/');
    const std::string_view directory = std::string_view(canonical).substr(0, slash);
    const std::string_view file = std::string_view(canonical).substr(slash + 1);

    std::string out(directory);
    out += "/_rels/";
    out.append(file);
    out += ".rels";
    return out;
}

std::string Package::ResolveTarget(std::string_view sourcePart, std::string_view target) {
    if (target.empty()) return {};

    // An absolute target is already a part name.
    if (target.front() == '/') return Canonicalise(target);

    const std::string canonical = Canonicalise(sourcePart);
    // Relative to the SOURCE PART'S DIRECTORY, not to the package root. Getting
    // this wrong makes every image in word/media/ resolve to /media/ and load
    // nothing.
    const auto slash = canonical.find_last_of('/');
    const std::string base = (slash == std::string::npos) ? std::string()
                                                          : canonical.substr(0, slash);

    // Resolve "." and ".." here rather than rejecting them: unlike a part name,
    // a RELATIVE target legitimately uses ".." to reach a sibling directory, and
    // "../media/image1.png" from /word/_rels/ is completely ordinary.
    std::vector<std::string_view> segments;
    for (std::size_t at = 1; at <= base.size();) {
        const auto end = std::min(base.find('/', at), base.size());
        if (end > at) segments.push_back(std::string_view(base).substr(at, end - at));
        at = end + 1;
    }

    for (std::size_t at = 0; at <= target.size();) {
        const auto end = std::min(target.find('/', at), target.size());
        const std::string_view segment = target.substr(at, end - at);
        at = end + 1;

        if (segment.empty() || segment == ".") continue;
        if (segment == "..") {
            // Climbing above the package root is the escape we refuse.
            if (segments.empty()) return {};
            segments.pop_back();
            continue;
        }
        segments.push_back(segment);
    }

    std::string out;
    for (const std::string_view segment : segments) {
        out.push_back('/');
        out.append(segment);
    }
    return Canonicalise(out);
}

Result Package::Open(std::span<const std::byte> zip, Package& into, const Limits& limits) {
    if (zip.size() < 22) {  // smaller than an empty ZIP's end-of-directory record
        return Result::Error(Status::kMalformedInput,
                             "This file is too small to be a Word document.");
    }

    mz_zip_archive archive{};
    if (!mz_zip_reader_init_mem(&archive, zip.data(), zip.size(), 0)) {
        return Result::Error(Status::kMalformedInput,
                             "This file is not a valid Word document: its ZIP container could "
                             "not be read.");
    }
    struct Guard {
        mz_zip_archive* archive;
        ~Guard() { mz_zip_reader_end(archive); }
    } guard{&archive};

    const mz_uint count = mz_zip_reader_get_num_files(&archive);
    if (count > limits.maxParts) {
        return Result::Error(Status::kResourceLimit,
                             "This document contains " + std::to_string(count) +
                                 " parts, which is more than Morph will process.");
    }

    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive, i, &stat)) {
            return Result::Error(Status::kMalformedInput,
                                 "This document's ZIP directory is damaged.");
        }
        if (mz_zip_reader_is_file_a_directory(&archive, i)) continue;

        const std::string name = Canonicalise(stat.m_filename);
        if (name.empty()) {
            // Zip slip. Named specifically, because "the file is corrupt" would
            // send the user looking for the wrong problem.
            return Result::Error(Status::kMalformedInput,
                                 std::string("This document contains an unsafe internal path (") +
                                     stat.m_filename + ") and was not opened.");
        }

        if (stat.m_uncomp_size > limits.maxPartBytes) {
            return Result::Error(Status::kResourceLimit,
                                 "A part of this document is larger than Morph will process.");
        }
        if (into.totalBytes_ + stat.m_uncomp_size > limits.maxTotalUncompressedBytes) {
            return Result::Error(Status::kResourceLimit,
                                 "This document expands to more than 500 MB and was rejected as "
                                 "a possible compression bomb.");
        }
        // The declared sizes are attacker-controlled, so this check is an early
        // out, not the defence. The real defence is the streaming cap below.
        if (stat.m_comp_size >= limits.ratioCheckFloorBytes && stat.m_comp_size > 0 &&
            stat.m_uncomp_size / stat.m_comp_size > limits.maxCompressionRatio) {
            return Result::Error(Status::kResourceLimit,
                                 "This document contains a part compressed more than 200:1 and "
                                 "was rejected as a possible compression bomb.");
        }

        Part part;
        part.name = name;

        const std::uint64_t remaining = limits.maxTotalUncompressedBytes - into.totalBytes_;
        InflateSink sink{&part.bytes, std::min(remaining, limits.maxPartBytes), false};
        // Extract through a callback so the buffer grows with the bytes that
        // actually arrive. Extracting to a heap block sized from the header is
        // the zip-bomb bug in its purest form.
        if (!mz_zip_reader_extract_to_callback(&archive, i, &OnInflated, &sink, 0)) {
            if (sink.exceeded) {
                return Result::Error(Status::kResourceLimit,
                                     "This document expands to more than Morph will process and "
                                     "was rejected as a possible compression bomb.");
            }
            return Result::Error(Status::kMalformedInput,
                                 std::string("A part of this document could not be decompressed (") +
                                     name + ").");
        }

        into.totalBytes_ += part.bytes.size();
        into.parts_.emplace(ToLowerAscii(name), std::move(part));
    }

    if (Result result = into.LoadContentTypes(); !result) return result;
    if (Result result = into.LoadRelationships(); !result) return result;
    return Result::Ok();
}

Result Package::LoadContentTypes() {
    const Part* types = Find("/[Content_Types].xml");
    if (types == nullptr) {
        return Result::Error(Status::kMalformedInput,
                             "This file is not a valid Word document: it has no content-type "
                             "map.");
    }

    pugi::xml_document xml;
    // pugixml does not expand external entities, which is why it was chosen
    // (section 8.4). parse_default excludes DTD processing entirely.
    const pugi::xml_parse_result parsed =
        xml.load_buffer(types->bytes.data(), types->bytes.size(), kOoxmlParseFlags);
    if (!parsed) {
        return Result::Error(Status::kMalformedInput,
                             "This document's content-type map is not valid XML.");
    }

    const pugi::xml_node root = xml.child("Types");
    for (const pugi::xml_node node : root.children()) {
        const std::string_view name = node.name();
        if (name == "Default") {
            defaultsByExtension_.insert_or_assign(
                ToLowerAscii(node.attribute("Extension").as_string()),
                node.attribute("ContentType").as_string());
        } else if (name == "Override") {
            const std::string part = Canonicalise(node.attribute("PartName").as_string());
            if (part.empty()) continue;  // an unsafe override names nothing we hold
            overridesByPart_.insert_or_assign(ToLowerAscii(part),
                                              node.attribute("ContentType").as_string());
        }
    }

    // Section 15.2: "Part names are validated against [Content_Types].xml before
    // any read." Resolve every part's type now, so a later lookup cannot be the
    // first place we notice a part nothing declared.
    for (auto& [key, part] : parts_) {
        if (const auto it = overridesByPart_.find(key); it != overridesByPart_.end()) {
            part.contentType = it->second;
            continue;
        }
        const auto it = defaultsByExtension_.find(ToLowerAscii(Extension(part.name)));
        if (it != defaultsByExtension_.end()) part.contentType = it->second;
    }
    return Result::Ok();
}

Result Package::LoadRelationships() {
    for (const auto& [key, part] : parts_) {
        // A .rels part is one whose name ends in ".rels" inside a "_rels"
        // directory. Matching on the directory too avoids treating a content
        // part that merely ends in .rels as relationship metadata.
        if (!key.ends_with(".rels")) continue;
        if (key.find("/_rels/") == std::string::npos) continue;

        pugi::xml_document xml;
        if (!xml.load_buffer(part.bytes.data(), part.bytes.size(), kOoxmlParseFlags)) {
            // A damaged .rels part costs us that part's links, not the document.
            continue;
        }

        // "/word/_rels/document.xml.rels" describes "/word/document.xml".
        // "/_rels/.rels" describes the package root, spelled "".
        const auto rels_at = part.name.rfind("/_rels/");
        const std::string directory = part.name.substr(0, rels_at);
        const std::string file =
            part.name.substr(rels_at + 7, part.name.size() - rels_at - 7 - 5);
        const std::string source = file.empty() ? std::string() : directory + "/" + file;

        std::vector<Relationship> list;
        for (const pugi::xml_node node : xml.child("Relationships").children("Relationship")) {
            Relationship relationship;
            relationship.id = node.attribute("Id").as_string();
            relationship.type = node.attribute("Type").as_string();
            relationship.target = node.attribute("Target").as_string();
            relationship.external =
                std::string_view(node.attribute("TargetMode").as_string()) == "External";
            if (!relationship.id.empty()) list.push_back(std::move(relationship));
        }
        relationships_.insert_or_assign(ToLowerAscii(source), std::move(list));
    }
    return Result::Ok();
}

const Part* Package::Find(std::string_view partName) const {
    const std::string canonical = Canonicalise(partName);
    if (canonical.empty()) return nullptr;
    const auto it = parts_.find(ToLowerAscii(canonical));
    return it == parts_.end() ? nullptr : &it->second;
}

const std::vector<Relationship>& Package::RelationshipsOf(std::string_view sourcePart) const {
    static const std::vector<Relationship> kNone;

    std::string key;
    if (!sourcePart.empty() && sourcePart != "/") {
        key = ToLowerAscii(Canonicalise(sourcePart));
    }
    const auto it = relationships_.find(key);
    return it == relationships_.end() ? kNone : it->second;
}

const Relationship* Package::FindRelationship(std::string_view sourcePart,
                                              std::string_view id) const {
    for (const Relationship& relationship : RelationshipsOf(sourcePart)) {
        if (relationship.id == id) return &relationship;
    }
    return nullptr;
}

const Relationship* Package::FindRelationshipByType(std::string_view sourcePart,
                                                    std::string_view type) const {
    for (const Relationship& relationship : RelationshipsOf(sourcePart)) {
        if (relationship.type == type) return &relationship;
    }
    return nullptr;
}

// --- PackageWriter -----------------------------------------------------------

void PackageWriter::AddDefault(std::string extension, std::string contentType) {
    defaultsByExtension_.insert_or_assign(ToLowerAscii(extension), std::move(contentType));
}

void PackageWriter::AddPart(std::string partName, std::string contentType,
                            std::vector<std::byte> bytes) {
    Part part;
    part.name = Package::Canonicalise(partName);
    part.contentType = std::move(contentType);
    part.bytes = std::move(bytes);
    if (part.name.empty()) return;  // refuse to write what we would refuse to read
    parts_.insert_or_assign(part.name, std::move(part));
}

void PackageWriter::AddPart(std::string partName, std::string contentType,
                            std::string_view text) {
    AddPart(std::move(partName), std::move(contentType), ToBytes(text));
}

void PackageWriter::AddRelationship(std::string sourcePart, Relationship relationship) {
    const std::string key =
        (sourcePart.empty() || sourcePart == "/") ? std::string()
                                                  : Package::Canonicalise(sourcePart);
    relationships_[key].push_back(std::move(relationship));
}

std::string PackageWriter::AddRelationship(std::string sourcePart, std::string type,
                                           std::string target, bool external) {
    const std::string key =
        (sourcePart.empty() || sourcePart == "/") ? std::string()
                                                  : Package::Canonicalise(sourcePart);
    const std::string id = "rId" + std::to_string(relationships_[key].size() + 1);
    relationships_[key].push_back(
        Relationship{id, std::move(type), std::move(target), external});
    return id;
}

std::string PackageWriter::BuildContentTypes() const {
    std::string xml =
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        "\n"
        R"(<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">)";

    // std::map iteration is sorted, so this is deterministic without sorting.
    for (const auto& [extension, contentType] : defaultsByExtension_) {
        xml += R"(<Default Extension=")";
        xml += XmlEscape(extension);
        xml += R"(" ContentType=")";
        xml += XmlEscape(contentType);
        xml += R"("/>)";
    }
    for (const auto& [name, part] : parts_) {
        if (part.contentType.empty()) continue;  // covered by a Default
        xml += R"(<Override PartName=")";
        xml += XmlEscape(name);
        xml += R"(" ContentType=")";
        xml += XmlEscape(part.contentType);
        xml += R"("/>)";
    }
    xml += "</Types>";
    return xml;
}

std::string PackageWriter::BuildRelationships(const std::vector<Relationship>& list) const {
    std::string xml =
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        "\n"
        R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)";
    for (const Relationship& relationship : list) {
        xml += R"(<Relationship Id=")";
        xml += XmlEscape(relationship.id);
        xml += R"(" Type=")";
        xml += XmlEscape(relationship.type);
        xml += R"(" Target=")";
        xml += XmlEscape(relationship.target);
        if (relationship.external) xml += R"(" TargetMode="External)";
        xml += R"("/>)";
    }
    xml += "</Relationships>";
    return xml;
}

Result PackageWriter::Finish(std::vector<std::byte>& into) {
    mz_zip_archive archive{};
    if (!mz_zip_writer_init_heap(&archive, 0, 64 * 1024)) {
        return Result::Error(Status::kInternal, "Could not start writing the Word document.");
    }
    struct Guard {
        mz_zip_archive* archive;
        ~Guard() { mz_zip_writer_end(archive); }
    } guard{&archive};

    // Every entry gets the same timestamp so the output is byte-reproducible
    // (section 8.4), which is what makes golden-file testing possible.
    //
    // The subtlety: miniz converts a time_t to a DOS timestamp through
    // localtime, so a fixed time_t would still write different bytes in
    // different timezones. Building the time_t with mktime -- which reads a
    // struct tm as LOCAL time -- inverts that conversion, so the DOS fields come
    // out identical on every machine regardless of where it is.
    std::tm calendar{};
    calendar.tm_year = 100;  // 2000. Comfortably inside the DOS range in any
    calendar.tm_mon = 0;     // timezone, unlike the 1980 epoch itself.
    calendar.tm_mday = 1;
    calendar.tm_isdst = -1;
    MZ_TIME_T fixed_time = std::mktime(&calendar);

    // Assemble the full name list first, so the write order is one sorted pass
    // over everything rather than three passes in an incidental order.
    std::map<std::string, std::vector<std::byte>, std::less<>> entries;
    entries.emplace("[Content_Types].xml", ToBytes(BuildContentTypes()));

    for (const auto& [source, list] : relationships_) {
        if (list.empty()) continue;
        const std::string name = Package::RelationshipPartFor(source);
        entries.emplace(name.substr(1), ToBytes(BuildRelationships(list)));
    }
    for (const auto& [name, part] : parts_) {
        entries.emplace(name.substr(1), part.bytes);
    }

    for (const auto& [name, bytes] : entries) {
        if (!mz_zip_writer_add_mem_ex_v2(&archive, name.c_str(),
                                         bytes.empty() ? nullptr : bytes.data(), bytes.size(),
                                         /*pComment=*/nullptr, /*comment_size=*/0,
                                         static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION),
                                         /*uncomp_size=*/0,
                                         /*uncomp_crc32=*/0, &fixed_time,
                                         /*user_extra_data_local=*/nullptr, 0,
                                         /*user_extra_data_central=*/nullptr, 0)) {
            return Result::Error(Status::kInternal,
                                 "Could not write the part " + name + " into the document.");
        }
    }

    void* buffer = nullptr;
    std::size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&archive, &buffer, &size)) {
        return Result::Error(Status::kInternal, "Could not finish writing the Word document.");
    }

    const auto* bytes = static_cast<const std::byte*>(buffer);
    into.assign(bytes, bytes + size);
    mz_free(buffer);
    return Result::Ok();
}

}  // namespace morph::opc
