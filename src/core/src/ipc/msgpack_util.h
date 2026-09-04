// Morph — msgpack packing helpers shared by the two codecs (architecture 10.2).
//
// The client-facing codec (codec.cpp) and the broker-to-worker codec
// (worker_codec.cpp) encode different message sets over the same rules: maps
// with string keys, total decoding, absent keys taking their default. These are
// those rules, factored out so the second codec cannot drift from the first.
//
// Internal to morph.core. It is under src/ rather than include/ on purpose --
// msgpack.h must not leak into anything that includes a public header.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <msgpack.h>

namespace morph::ipc::detail {

// --- Packing -----------------------------------------------------------------

class Packer {
public:
    Packer() {
        msgpack_sbuffer_init(&buffer_);
        msgpack_packer_init(&packer_, &buffer_, msgpack_sbuffer_write);
    }
    ~Packer() { msgpack_sbuffer_destroy(&buffer_); }

    Packer(const Packer&) = delete;
    Packer& operator=(const Packer&) = delete;

    void Map(std::size_t entries) { msgpack_pack_map(&packer_, entries); }
    void Array(std::size_t entries) { msgpack_pack_array(&packer_, entries); }

    void Key(std::string_view key) { Str(key); }

    void Str(std::string_view value) {
        msgpack_pack_str(&packer_, value.size());
        if (!value.empty()) msgpack_pack_str_body(&packer_, value.data(), value.size());
    }
    void U64(std::uint64_t value) { msgpack_pack_uint64(&packer_, value); }
    void I64(std::int64_t value) { msgpack_pack_int64(&packer_, value); }
    void Dbl(double value) { msgpack_pack_double(&packer_, value); }
    void Bool(bool value) {
        value ? msgpack_pack_true(&packer_) : msgpack_pack_false(&packer_);
    }

    void Field(std::string_view key, std::string_view value) { Key(key); Str(value); }
    void Field(std::string_view key, std::uint64_t value) { Key(key); U64(value); }
    void Field(std::string_view key, std::int64_t value) { Key(key); I64(value); }
    void Field(std::string_view key, double value) { Key(key); Dbl(value); }
    void Field(std::string_view key, bool value) { Key(key); Bool(value); }

    [[nodiscard]] std::vector<std::byte> Take() const {
        const auto* begin = reinterpret_cast<const std::byte*>(buffer_.data);
        return std::vector<std::byte>(begin, begin + buffer_.size);
    }

private:
    msgpack_sbuffer buffer_{};
    msgpack_packer packer_{};
};

inline void PackStrings(Packer& packer, std::string_view key,
                        const std::vector<std::string>& values) {
    packer.Key(key);
    packer.Array(values.size());
    for (const std::string& value : values) packer.Str(value);
}

// --- Unpacking ---------------------------------------------------------------

// Owns the msgpack zone for one decode. Every accessor below returns a default
// rather than failing, so a message missing a field decodes to a message with
// that field at its default -- which is the forward-compatibility rule.
class Unpacker {
public:
    explicit Unpacker(std::span<const std::byte> payload) {
        msgpack_unpacked_init(&unpacked_);
        const msgpack_unpack_return rc =
            msgpack_unpack_next(&unpacked_, reinterpret_cast<const char*>(payload.data()),
                                payload.size(), nullptr);
        ok_ = rc == MSGPACK_UNPACK_SUCCESS && unpacked_.data.type == MSGPACK_OBJECT_MAP;
    }
    ~Unpacker() { msgpack_unpacked_destroy(&unpacked_); }

    Unpacker(const Unpacker&) = delete;
    Unpacker& operator=(const Unpacker&) = delete;

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] const msgpack_object& root() const noexcept { return unpacked_.data; }

private:
    msgpack_unpacked unpacked_{};
    bool ok_ = false;
};

inline const msgpack_object* Find(const msgpack_object& map, std::string_view key) {
    if (map.type != MSGPACK_OBJECT_MAP) return nullptr;
    for (std::uint32_t i = 0; i < map.via.map.size; ++i) {
        const msgpack_object_kv& kv = map.via.map.ptr[i];
        if (kv.key.type != MSGPACK_OBJECT_STR) continue;
        if (std::string_view(kv.key.via.str.ptr, kv.key.via.str.size) == key) return &kv.val;
    }
    return nullptr;
}

inline std::string GetStr(const msgpack_object& map, std::string_view key) {
    const msgpack_object* value = Find(map, key);
    if (value == nullptr || value->type != MSGPACK_OBJECT_STR) return {};
    return std::string(value->via.str.ptr, value->via.str.size);
}

inline std::uint64_t GetU64(const msgpack_object& map, std::string_view key,
                     std::uint64_t fallback = 0) {
    const msgpack_object* value = Find(map, key);
    if (value == nullptr) return fallback;
    if (value->type == MSGPACK_OBJECT_POSITIVE_INTEGER) return value->via.u64;
    // A value that arrives as a negative integer where an unsigned was expected
    // is a peer bug, not something to wrap around silently.
    if (value->type == MSGPACK_OBJECT_NEGATIVE_INTEGER) return fallback;
    return fallback;
}

inline std::int64_t GetI64(const msgpack_object& map, std::string_view key,
                           std::int64_t fallback = 0) {
    const msgpack_object* value = Find(map, key);
    if (value == nullptr) return fallback;
    if (value->type == MSGPACK_OBJECT_POSITIVE_INTEGER) {
        return static_cast<std::int64_t>(value->via.u64);
    }
    if (value->type == MSGPACK_OBJECT_NEGATIVE_INTEGER) return value->via.i64;
    return fallback;
}

inline double GetDbl(const msgpack_object& map, std::string_view key,
                     double fallback = 0.0) {
    const msgpack_object* value = Find(map, key);
    if (value == nullptr) return fallback;
    if (value->type == MSGPACK_OBJECT_FLOAT32 || value->type == MSGPACK_OBJECT_FLOAT64) {
        return value->via.f64;
    }
    if (value->type == MSGPACK_OBJECT_POSITIVE_INTEGER) {
        return static_cast<double>(value->via.u64);
    }
    if (value->type == MSGPACK_OBJECT_NEGATIVE_INTEGER) {
        return static_cast<double>(value->via.i64);
    }
    return fallback;
}

inline bool GetBool(const msgpack_object& map, std::string_view key,
                    bool fallback = false) {
    const msgpack_object* value = Find(map, key);
    if (value == nullptr || value->type != MSGPACK_OBJECT_BOOLEAN) return fallback;
    return value->via.boolean;
}

inline std::uint8_t GetU8(const msgpack_object& map, std::string_view key) {
    const std::uint64_t raw = GetU64(map, key);
    return static_cast<std::uint8_t>(raw > 0xFFu ? 0u : raw);
}

inline std::uint32_t GetU32(const msgpack_object& map, std::string_view key) {
    const std::uint64_t raw = GetU64(map, key);
    return static_cast<std::uint32_t>(raw > 0xFFFFFFFFull ? 0u : raw);
}

inline std::vector<std::string> GetStrings(const msgpack_object& map, std::string_view key) {
    std::vector<std::string> out;
    const msgpack_object* value = Find(map, key);
    if (value == nullptr || value->type != MSGPACK_OBJECT_ARRAY) return out;

    out.reserve(value->via.array.size);
    for (std::uint32_t i = 0; i < value->via.array.size; ++i) {
        const msgpack_object& element = value->via.array.ptr[i];
        if (element.type != MSGPACK_OBJECT_STR) continue;
        out.emplace_back(element.via.str.ptr, element.via.str.size);
    }
    return out;
}

// Applies `read` to each element of an array field, skipping non-map elements.
template <typename Fn>
void ForEachElement(const msgpack_object& map, std::string_view key, Fn&& read) {
    const msgpack_object* value = Find(map, key);
    if (value == nullptr || value->type != MSGPACK_OBJECT_ARRAY) return;
    for (std::uint32_t i = 0; i < value->via.array.size; ++i) {
        const msgpack_object& element = value->via.array.ptr[i];
        if (element.type != MSGPACK_OBJECT_MAP) continue;
        read(element);
    }
}

}  // namespace morph::ipc::detail
