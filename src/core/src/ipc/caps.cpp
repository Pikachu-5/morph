#include "morph/core/caps.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

#include <windows.h>

#include <sddl.h>

#include "morph/core/ipc/pipe.h"

namespace morph {
namespace {

// Interns strings into one pool, so "application/pdf" is stored once however
// many sources can produce it.
class StringPool {
public:
    std::uint32_t Add(const std::string& text) {
        const auto found = offsets_.find(text);
        if (found != offsets_.end()) return found->second;

        const auto offset = static_cast<std::uint32_t>(bytes_.size());
        bytes_.insert(bytes_.end(), text.begin(), text.end());
        bytes_.push_back('\0');
        offsets_.emplace(text, offset);
        return offset;
    }

    [[nodiscard]] const std::vector<char>& bytes() const noexcept { return bytes_; }

private:
    std::map<std::string, std::uint32_t> offsets_;
    std::vector<char> bytes_;
};

bool BuildSecurityAttributes(SECURITY_ATTRIBUTES& attributes, PSECURITY_DESCRIPTOR& descriptor) {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    struct Closer {
        HANDLE handle;
        ~Closer() { ::CloseHandle(handle); }
    } closer{token};

    DWORD needed = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) return false;

    std::vector<std::byte> buffer(needed);
    if (!::GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed)) return false;

    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR sidText = nullptr;
    if (!::ConvertSidToStringSidW(user->User.Sid, &sidText) || sidText == nullptr) return false;
    const std::wstring sid(sidText);
    ::LocalFree(sidText);

    // Section 9.2 calls the table "read-only, ACL'd to the user SID". The user
    // gets full access because the broker runs as that user and has to write it;
    // everyone else -- including NETWORK, denied first -- gets nothing.
    const std::wstring sddl = L"D:(D;;GA;;;NU)(A;;GA;;;OW)(A;;GA;;;" + sid + L")";
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                                &descriptor, nullptr)) {
        return false;
    }

    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    return true;
}

}  // namespace

// --- Serialisation -----------------------------------------------------------

std::vector<std::byte> SerialiseCapabilities(const CapabilitySet& set, std::uint64_t generation) {
    // Sorted, deduplicated, and with empty sources dropped: the reader binary
    // searches this array, and an extension that can become nothing would put an
    // empty submenu in Explorer.
    std::vector<const CapabilitySet::Source*> sources;
    sources.reserve(set.sources.size());
    for (const CapabilitySet::Source& source : set.sources) {
        if (source.extension.empty() || source.targets.empty()) continue;
        sources.push_back(&source);
    }
    std::sort(sources.begin(), sources.end(),
              [](const CapabilitySet::Source* a, const CapabilitySet::Source* b) {
                  return a->extension < b->extension;
              });
    sources.erase(std::unique(sources.begin(), sources.end(),
                              [](const CapabilitySet::Source* a, const CapabilitySet::Source* b) {
                                  return a->extension == b->extension;
                              }),
                  sources.end());

    StringPool pool;
    std::vector<CapsExtension> index;
    std::vector<CapsTarget> targets;
    index.reserve(sources.size());

    for (const CapabilitySet::Source* source : sources) {
        CapsExtension entry;
        entry.nameOff = pool.Add(source->extension);
        entry.targetStart = static_cast<std::uint32_t>(targets.size());
        entry.targetCount = static_cast<std::uint32_t>(source->targets.size());
        index.push_back(entry);

        for (const CapabilitySet::Target& target : source->targets) {
            CapsTarget wire;
            wire.profileOff = pool.Add(target.profile);
            wire.extensionOff = pool.Add(target.extension);
            wire.fidelity = target.fidelity;
            wire.hopCount = target.hopCount;
            wire.requiresDisk = target.requiresDisk ? 1u : 0u;
            targets.push_back(wire);
        }
    }

    CapsHeader header;
    header.generation = generation;
    header.extCount = static_cast<std::uint32_t>(index.size());
    header.targetCount = static_cast<std::uint32_t>(targets.size());
    header.extIndexOff = sizeof(CapsHeader);
    header.targetTableOff =
        header.extIndexOff + static_cast<std::uint32_t>(index.size() * sizeof(CapsExtension));
    header.stringPoolOff =
        header.targetTableOff + static_cast<std::uint32_t>(targets.size() * sizeof(CapsTarget));
    header.stringPoolBytes = static_cast<std::uint32_t>(pool.bytes().size());
    header.totalBytes = header.stringPoolOff + header.stringPoolBytes;

    if (header.totalBytes > kCapsCapacityBytes) return {};

    std::vector<std::byte> out(header.totalBytes);
    std::memcpy(out.data(), &header, sizeof(header));
    if (!index.empty()) {
        std::memcpy(out.data() + header.extIndexOff, index.data(),
                    index.size() * sizeof(CapsExtension));
    }
    if (!targets.empty()) {
        std::memcpy(out.data() + header.targetTableOff, targets.data(),
                    targets.size() * sizeof(CapsTarget));
    }
    if (!pool.bytes().empty()) {
        std::memcpy(out.data() + header.stringPoolOff, pool.bytes().data(),
                    pool.bytes().size());
    }
    return out;
}

// --- Reading -----------------------------------------------------------------

CapabilityTable CapabilityTable::Attach(const void* data, std::size_t bytes) noexcept {
    CapabilityTable table;
    if (data == nullptr || bytes < sizeof(CapsHeader)) return table;

    const auto* base = static_cast<const std::byte*>(data);
    const auto* header = reinterpret_cast<const CapsHeader*>(base);

    if (header->magic != kCapsMagic) return table;
    // A layout this reader does not know is not an error and not a table: the
    // caller shows its static menu, exactly as section 9.2 prescribes.
    if (header->layoutVersion != kCapsLayoutVersion) return table;
    if (header->totalBytes > bytes || header->totalBytes < sizeof(CapsHeader)) return table;

    // Generation zero means "nobody has finished publishing this". Publish
    // clears it before writing the body and sets it afterwards, so a reader that
    // arrives mid-write sees no table rather than half of one -- and a mapping
    // that was created but never filled reads as zeroes, which lands here too.
    if (header->generation == 0) return table;

    // Everything below is arithmetic on numbers a hostile writer chose, so each
    // step is checked against totalBytes with no opportunity to overflow: the
    // counts are widened to 64 bits before being multiplied.
    const std::uint64_t indexBytes =
        static_cast<std::uint64_t>(header->extCount) * sizeof(CapsExtension);
    const std::uint64_t targetBytes =
        static_cast<std::uint64_t>(header->targetCount) * sizeof(CapsTarget);

    if (header->extIndexOff < sizeof(CapsHeader)) return table;
    if (header->extIndexOff + indexBytes > header->totalBytes) return table;
    if (header->targetTableOff + targetBytes > header->totalBytes) return table;
    if (static_cast<std::uint64_t>(header->stringPoolOff) + header->stringPoolBytes >
        header->totalBytes) {
        return table;
    }

    // The pool must end in a NUL, or String() could run off the end of it.
    if (header->stringPoolBytes > 0 &&
        base[header->stringPoolOff + header->stringPoolBytes - 1] != std::byte{0}) {
        return table;
    }

    const auto* index = reinterpret_cast<const CapsExtension*>(base + header->extIndexOff);
    for (std::uint32_t i = 0; i < header->extCount; ++i) {
        if (index[i].nameOff >= header->stringPoolBytes) return table;
        const std::uint64_t last =
            static_cast<std::uint64_t>(index[i].targetStart) + index[i].targetCount;
        if (last > header->targetCount) return table;
    }

    const auto* targets = reinterpret_cast<const CapsTarget*>(base + header->targetTableOff);
    for (std::uint32_t i = 0; i < header->targetCount; ++i) {
        if (targets[i].profileOff >= header->stringPoolBytes) return table;
        if (targets[i].extensionOff >= header->stringPoolBytes) return table;
    }

    table.header_ = header;
    table.base_ = base;
    table.bytes_ = bytes;
    return table;
}

std::uint64_t CapabilityTable::Generation() const noexcept {
    return Valid() ? header_->generation : 0;
}

std::span<const CapsExtension> CapabilityTable::Extensions() const noexcept {
    if (!Valid()) return {};
    return std::span<const CapsExtension>(
        reinterpret_cast<const CapsExtension*>(base_ + header_->extIndexOff), header_->extCount);
}

std::string_view CapabilityTable::String(std::uint32_t offset) const noexcept {
    if (!Valid() || offset >= header_->stringPoolBytes) return {};
    const char* text = reinterpret_cast<const char*>(base_ + header_->stringPoolOff + offset);
    // Bounded by the pool, which Attach proved ends in a NUL.
    const std::size_t limit = header_->stringPoolBytes - offset;
    const std::size_t length = ::strnlen(text, limit);
    return std::string_view(text, length);
}

std::span<const CapsTarget> CapabilityTable::TargetsFor(
    std::string_view extension) const noexcept {
    if (!Valid()) return {};

    // Normalised without allocating: the caller is inside Explorer's
    // right-click, and section 17's budget is measured in microseconds.
    if (!extension.empty() && extension.front() == '.') extension.remove_prefix(1);
    char lowered[32]{};
    if (extension.empty() || extension.size() >= sizeof(lowered)) return {};
    for (std::size_t i = 0; i < extension.size(); ++i) {
        lowered[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(extension[i])));
    }
    const std::string_view key(lowered, extension.size());

    const std::span<const CapsExtension> index = Extensions();
    const auto found = std::lower_bound(index.begin(), index.end(), key,
                                        [this](const CapsExtension& entry, std::string_view k) {
                                            return String(entry.nameOff) < k;
                                        });
    if (found == index.end() || String(found->nameOff) != key) return {};

    const auto* targets = reinterpret_cast<const CapsTarget*>(base_ + header_->targetTableOff);
    return std::span<const CapsTarget>(targets + found->targetStart, found->targetCount);
}

// --- The mapping -------------------------------------------------------------

std::wstring CapabilityMapName() {
    return L"Local\\morph.caps.v" + std::to_wstring(kCapsLayoutVersion) + L"." +
           ipc::InstallTag();
}

CapabilityMapping::~CapabilityMapping() { Close(); }

void CapabilityMapping::Close() noexcept {
    if (view_ != nullptr) ::UnmapViewOfFile(view_);
    if (section_ != nullptr) ::CloseHandle(static_cast<HANDLE>(section_));
    view_ = nullptr;
    section_ = nullptr;
    writable_ = false;
}

Result CapabilityMapping::Create() {
    Close();

    SECURITY_ATTRIBUTES attributes{};
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!BuildSecurityAttributes(attributes, descriptor)) {
        return Result::Error(Status::kInternal,
                             "Could not build the capability table's security descriptor. "
                             "Refusing to publish it with default permissions.");
    }
    struct Freer {
        PSECURITY_DESCRIPTOR descriptor;
        ~Freer() { if (descriptor != nullptr) ::LocalFree(descriptor); }
    } freer{descriptor};

    const HANDLE section = ::CreateFileMappingW(
        INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0,
        static_cast<DWORD>(kCapsCapacityBytes), CapabilityMapName().c_str());
    if (section == nullptr) {
        return Result::Error(Status::kInternal,
                             "Could not create the capability table (Windows error " +
                                 std::to_string(::GetLastError()) + ").");
    }
    section_ = section;

    view_ = ::MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, kCapsCapacityBytes);
    if (view_ == nullptr) {
        const DWORD code = ::GetLastError();
        Close();
        return Result::Error(Status::kInternal,
                             "Could not map the capability table (Windows error " +
                                 std::to_string(code) + ").");
    }
    writable_ = true;
    return Result::Ok();
}

Result CapabilityMapping::OpenForRead() {
    Close();

    const HANDLE section =
        ::OpenFileMappingW(FILE_MAP_READ, FALSE, CapabilityMapName().c_str());
    if (section == nullptr) {
        // No broker has ever published in this session. Section 9.2: the caller
        // shows a static entry. Starting a broker to fill the table in is
        // exactly the pipe round trip the table exists to avoid.
        return Result::Error(Status::kMissingCapability,
                             "No capability table has been published.");
    }
    section_ = section;

    view_ = ::MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0);
    if (view_ == nullptr) {
        const DWORD code = ::GetLastError();
        Close();
        return Result::Error(Status::kInternal,
                             "Could not map the capability table (Windows error " +
                                 std::to_string(code) + ").");
    }
    return Result::Ok();
}

Result CapabilityMapping::Publish(const CapabilitySet& set, std::uint64_t generation) {
    if (!writable_ || view_ == nullptr) {
        return Result::Error(Status::kInternal, "The capability table is not open for writing.");
    }

    const std::vector<std::byte> bytes = SerialiseCapabilities(set, generation);
    if (bytes.empty()) {
        return Result::Error(Status::kResourceLimit,
                             "The capability table does not fit in its mapping.");
    }

    auto* target = static_cast<std::byte*>(view_);

    // The generation goes down last, after a barrier. A reader that snapshots it
    // before this store and re-checks after sees a change and reads again;
    // without the barrier the compiler or the processor could publish the
    // generation before the body it describes.
    CapsHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    header.generation = 0;

    std::memcpy(target, &header, sizeof(header));
    std::memcpy(target + sizeof(header), bytes.data() + sizeof(header),
                bytes.size() - sizeof(header));

    ::MemoryBarrier();
    reinterpret_cast<volatile CapsHeader*>(target)->generation = generation;
    return Result::Ok();
}

CapabilityTable CapabilityMapping::Table() const noexcept {
    if (view_ == nullptr) return {};
    return CapabilityTable::Attach(view_, kCapsCapacityBytes);
}

}  // namespace morph
