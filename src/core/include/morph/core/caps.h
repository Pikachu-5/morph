// Morph — the capability table (architecture section 9.2).
//
// "The obvious implementation asks the broker 'what can I convert these files
//  to?' and builds a submenu from the answer. That is wrong: it starts a process
//  and does a pipe round-trip while the user's right-click hangs.
//
//  Instead, the broker publishes a versioned, read-only memory-mapped capability
//  table at startup and whenever the codec probe result changes. The shell
//  extension opens it lazily, reads a header with a version stamp and a
//  format-to-targets index, and builds the submenu from a handful of pointer
//  dereferences. No allocation, no IPC, no file IO."
//
// Three properties follow from that sentence and shape everything below.
//
// KEYED BY EXTENSION, not by media profile. The shell extension has a selection
// of file names and a latency budget (section 17) that forbids opening any of
// them, so the only key it can compute is the extension. This is the one place
// in Morph where an extension decides anything, and it decides a MENU, never a
// conversion: the worker still probes the bytes, and a file whose extension lied
// gets the right answer late rather than the wrong answer early.
//
// NO POINTERS ON THE WIRE. Every reference is a byte offset from the start of
// the mapping, because the reader maps it at whatever address it likes.
//
// VALIDATED ON ATTACH. The mapping is writable by the user who owns it, which is
// the same user the shell extension runs as, so a corrupt table is reachable
// without any privilege boundary being crossed. Attach checks every count and
// offset against the mapped size before the reader can dereference anything. A
// table that fails validation is treated exactly like a missing one -- section
// 9.2's "it degrades, it never hangs".
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "morph/core/result.h"

namespace morph {

// 'MRPH' little-endian.
inline constexpr std::uint32_t kCapsMagic = 0x4850524Du;

// Section 9.2: "bump = shellext ignores the map and shows a static menu".
inline constexpr std::uint32_t kCapsLayoutVersion = 1;

// The mapping is a fixed size so that a republish can reuse it in place: a
// resized mapping would need a new name, and a reader holding the old name would
// keep seeing the old table forever. 256 KiB is roughly a hundred times what the
// v1 format set needs.
inline constexpr std::size_t kCapsCapacityBytes = 256 * 1024;

#pragma pack(push, 4)

struct CapsHeader {
    std::uint32_t magic = kCapsMagic;
    std::uint32_t layoutVersion = kCapsLayoutVersion;
    // Incremented on every republish. Readers snapshot it, read the table, and
    // check it again; a change means the table moved under them and the read is
    // repeated. Written LAST on publish, which is what makes that check work.
    std::uint64_t generation = 0;
    std::uint32_t extCount = 0;
    std::uint32_t targetCount = 0;
    std::uint32_t extIndexOff = 0;
    std::uint32_t targetTableOff = 0;
    std::uint32_t stringPoolOff = 0;
    std::uint32_t stringPoolBytes = 0;
    std::uint32_t totalBytes = 0;
};

// One source extension. Sorted by name so the lookup is a binary search over a
// contiguous array -- no allocation, which is the section 17 budget's real
// constraint.
struct CapsExtension {
    std::uint32_t nameOff = 0;      // into the string pool; lowercase, no dot
    std::uint32_t targetStart = 0;  // index into the target table
    std::uint32_t targetCount = 0;
};

// One target a source extension can become.
struct CapsTarget {
    std::uint32_t profileOff = 0;    // "application/pdf"
    std::uint32_t extensionOff = 0;  // "pdf", for the menu label
    std::uint8_t fidelity = 0;       // morph::Fidelity, worst along the path
    std::uint8_t hopCount = 0;
    std::uint8_t requiresDisk = 0;
    std::uint8_t reserved = 0;
};

#pragma pack(pop)

static_assert(sizeof(CapsHeader) == 44, "the header layout is a wire format");
static_assert(sizeof(CapsExtension) == 12, "the index layout is a wire format");
static_assert(sizeof(CapsTarget) == 12, "the target layout is a wire format");

// A validated read-only view over a mapped table. Holds no ownership: the
// mapping outlives it, which in the shell extension means a process-lifetime
// mapping and a view created per menu.
class CapabilityTable {
public:
    CapabilityTable() = default;

    // Validates the whole structure and returns an invalid table on any
    // inconsistency. Never throws, never allocates.
    [[nodiscard]] static CapabilityTable Attach(const void* data, std::size_t bytes) noexcept;

    [[nodiscard]] bool Valid() const noexcept { return header_ != nullptr; }
    [[nodiscard]] std::uint64_t Generation() const noexcept;

    // The targets for a file extension, with or without the dot, in any case.
    // Empty when Morph does not handle that extension.
    [[nodiscard]] std::span<const CapsTarget> TargetsFor(
        std::string_view extension) const noexcept;

    // Resolves a string offset. Returns empty for an offset that did not survive
    // validation, which cannot happen on a valid table but costs nothing to say.
    [[nodiscard]] std::string_view String(std::uint32_t offset) const noexcept;

    [[nodiscard]] std::span<const CapsExtension> Extensions() const noexcept;

private:
    const CapsHeader* header_ = nullptr;
    const std::byte* base_ = nullptr;
    std::size_t bytes_ = 0;
};

// What a publisher hands to the serialiser. Ordinary C++ types: this side runs
// in the broker, where allocation is fine and the layout rules should not leak.
struct CapabilitySet {
    struct Target {
        std::string profile;
        std::string extension;
        std::uint8_t fidelity = 0;
        std::uint8_t hopCount = 0;
        bool requiresDisk = false;
    };
    struct Source {
        std::string extension;
        std::vector<Target> targets;
    };

    std::vector<Source> sources;
};

// Serialises `set` into the layout above. Sorts the sources by extension and
// interns every string once. Returns an empty vector when the result would
// exceed kCapsCapacityBytes, which the caller must treat as a failure to publish
// rather than as an empty table.
[[nodiscard]] std::vector<std::byte> SerialiseCapabilities(const CapabilitySet& set,
                                                           std::uint64_t generation);

// The named section the table lives in.
//
// Section 9.2 writes it "\Sessions\<n>\BaseNamedObjects\morph.caps.v1", which is
// what a "Local\" name resolves to. The install tag is the same one the pipe name
// carries, for the same reason: two builds on one machine must not share a table.
[[nodiscard]] std::wstring CapabilityMapName();

// The mapping itself. The broker creates one and keeps it for its lifetime; the
// shell extension opens one read-only and keeps it for the life of the surrogate.
class CapabilityMapping {
public:
    CapabilityMapping() = default;
    ~CapabilityMapping();

    CapabilityMapping(const CapabilityMapping&) = delete;
    CapabilityMapping& operator=(const CapabilityMapping&) = delete;

    // Broker side. Creates (or reopens) the section with the section 10.1
    // descriptor -- this user only, no NETWORK -- and maps it writable.
    [[nodiscard]] Result Create();

    // Reader side. Fails with kMissingCapability when no broker has published,
    // which is not an error: section 9.2 requires the menu to degrade to a
    // static entry rather than start a broker to fill it in.
    [[nodiscard]] Result OpenForRead();

    // Writes a new table and bumps the generation. Broker side only.
    [[nodiscard]] Result Publish(const CapabilitySet& set, std::uint64_t generation);

    [[nodiscard]] CapabilityTable Table() const noexcept;
    [[nodiscard]] bool Mapped() const noexcept { return view_ != nullptr; }

    void Close() noexcept;

private:
    void* section_ = nullptr;  // HANDLE
    void* view_ = nullptr;
    bool writable_ = false;
};

}  // namespace morph
