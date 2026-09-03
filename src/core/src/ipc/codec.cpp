#include "morph/core/ipc/codec.h"

#include <cstring>
#include <string>
#include <string_view>

#include <msgpack.h>

namespace morph::ipc {
namespace {

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

void PackStrings(Packer& packer, std::string_view key, const std::vector<std::string>& values) {
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

const msgpack_object* Find(const msgpack_object& map, std::string_view key) {
    if (map.type != MSGPACK_OBJECT_MAP) return nullptr;
    for (std::uint32_t i = 0; i < map.via.map.size; ++i) {
        const msgpack_object_kv& kv = map.via.map.ptr[i];
        if (kv.key.type != MSGPACK_OBJECT_STR) continue;
        if (std::string_view(kv.key.via.str.ptr, kv.key.via.str.size) == key) return &kv.val;
    }
    return nullptr;
}

std::string GetStr(const msgpack_object& map, std::string_view key) {
    const msgpack_object* value = Find(map, key);
    if (value == nullptr || value->type != MSGPACK_OBJECT_STR) return {};
    return std::string(value->via.str.ptr, value->via.str.size);
}

std::uint64_t GetU64(const msgpack_object& map, std::string_view key,
                     std::uint64_t fallback = 0) {
    const msgpack_object* value = Find(map, key);
    if (value == nullptr) return fallback;
    if (value->type == MSGPACK_OBJECT_POSITIVE_INTEGER) return value->via.u64;
    // A value that arrives as a negative integer where an unsigned was expected
    // is a peer bug, not something to wrap around silently.
    if (value->type == MSGPACK_OBJECT_NEGATIVE_INTEGER) return fallback;
    return fallback;
}

std::int64_t GetI64(const msgpack_object& map, std::string_view key, std::int64_t fallback = 0) {
    const msgpack_object* value = Find(map, key);
    if (value == nullptr) return fallback;
    if (value->type == MSGPACK_OBJECT_POSITIVE_INTEGER) {
        return static_cast<std::int64_t>(value->via.u64);
    }
    if (value->type == MSGPACK_OBJECT_NEGATIVE_INTEGER) return value->via.i64;
    return fallback;
}

double GetDbl(const msgpack_object& map, std::string_view key, double fallback = 0.0) {
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

bool GetBool(const msgpack_object& map, std::string_view key, bool fallback = false) {
    const msgpack_object* value = Find(map, key);
    if (value == nullptr || value->type != MSGPACK_OBJECT_BOOLEAN) return fallback;
    return value->via.boolean;
}

std::uint8_t GetU8(const msgpack_object& map, std::string_view key) {
    const std::uint64_t raw = GetU64(map, key);
    return static_cast<std::uint8_t>(raw > 0xFFu ? 0u : raw);
}

std::uint32_t GetU32(const msgpack_object& map, std::string_view key) {
    const std::uint64_t raw = GetU64(map, key);
    return static_cast<std::uint32_t>(raw > 0xFFFFFFFFull ? 0u : raw);
}

std::vector<std::string> GetStrings(const msgpack_object& map, std::string_view key) {
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

}  // namespace

// --- Hello -------------------------------------------------------------------

std::vector<std::byte> Encode(const Hello& message) {
    Packer packer;
    packer.Map(3);
    packer.Field("protocolVersion", static_cast<std::uint64_t>(message.protocolVersion));
    packer.Field("kind", static_cast<std::uint64_t>(message.kind));
    packer.Field("clientVersion", message.clientVersion);
    return packer.Take();
}

bool Decode(std::span<const std::byte> payload, Hello& into) {
    const Unpacker unpacker(payload);
    if (!unpacker.ok()) return false;

    into.protocolVersion = GetU32(unpacker.root(), "protocolVersion");
    into.kind = static_cast<ClientKind>(GetU8(unpacker.root(), "kind"));
    into.clientVersion = GetStr(unpacker.root(), "clientVersion");
    return true;
}

std::vector<std::byte> Encode(const HelloAck& message) {
    Packer packer;
    packer.Map(3);
    packer.Field("protocolVersion", static_cast<std::uint64_t>(message.protocolVersion));
    packer.Field("brokerVersion", message.brokerVersion);
    packer.Field("capabilityGeneration", message.capabilityGeneration);
    return packer.Take();
}

bool Decode(std::span<const std::byte> payload, HelloAck& into) {
    const Unpacker unpacker(payload);
    if (!unpacker.ok()) return false;

    into.protocolVersion = GetU32(unpacker.root(), "protocolVersion");
    into.brokerVersion = GetStr(unpacker.root(), "brokerVersion");
    into.capabilityGeneration = GetU64(unpacker.root(), "capabilityGeneration");
    return true;
}

// --- QueryTargets / Targets --------------------------------------------------

std::vector<std::byte> Encode(const QueryTargets& message) {
    Packer packer;
    packer.Map(2);
    PackStrings(packer, "sourceProfiles", message.sourceProfiles);
    packer.Field("largestInputBytes", message.largestInputBytes);
    return packer.Take();
}

bool Decode(std::span<const std::byte> payload, QueryTargets& into) {
    const Unpacker unpacker(payload);
    if (!unpacker.ok()) return false;

    into.sourceProfiles = GetStrings(unpacker.root(), "sourceProfiles");
    into.largestInputBytes = GetU64(unpacker.root(), "largestInputBytes");
    return true;
}

std::vector<std::byte> Encode(const Targets& message) {
    Packer packer;
    packer.Map(1);
    packer.Key("targets");
    packer.Array(message.targets.size());
    for (const TargetOption& target : message.targets) {
        packer.Map(5);
        packer.Field("profile", target.profile);
        packer.Field("fidelity", static_cast<std::uint64_t>(target.fidelity));
        packer.Field("hopCount", static_cast<std::uint64_t>(target.hopCount));
        packer.Field("estimatedMs", target.estimatedMs);
        packer.Field("requiresDisk", target.requiresDisk);
    }
    return packer.Take();
}

bool Decode(std::span<const std::byte> payload, Targets& into) {
    const Unpacker unpacker(payload);
    if (!unpacker.ok()) return false;

    ForEachElement(unpacker.root(), "targets", [&into](const msgpack_object& element) {
        TargetOption target;
        target.profile = GetStr(element, "profile");
        target.fidelity = GetU8(element, "fidelity");
        target.hopCount = GetU32(element, "hopCount");
        target.estimatedMs = GetDbl(element, "estimatedMs");
        target.requiresDisk = GetBool(element, "requiresDisk");
        into.targets.push_back(std::move(target));
    });
    return true;
}

// --- SubmitJob ---------------------------------------------------------------

std::vector<std::byte> Encode(const SubmitJob& message) {
    Packer packer;
    packer.Map(5);

    PackStrings(packer, "inputPaths", message.inputPaths);
    packer.Field("targetProfile", message.targetProfile);

    packer.Key("options");
    packer.Array(message.options.size());
    for (const OptionValue& option : message.options) {
        packer.Map(3);
        packer.Field("key", option.key);
        packer.Field("kind", static_cast<std::uint64_t>(option.kind));
        packer.Key("value");
        switch (option.kind) {
            case 0: packer.Bool(option.boolValue); break;
            case 1: packer.I64(option.intValue); break;
            case 2: packer.Dbl(option.doubleValue); break;
            default: packer.Str(option.stringValue); break;
        }
    }

    packer.Key("output");
    packer.Map(3);
    packer.Field("directory", message.output.directory);
    packer.Field("namingTemplate", message.output.namingTemplate);
    packer.Field("overwrite", static_cast<std::uint64_t>(message.output.overwrite));

    packer.Field("interactive", message.interactive);
    return packer.Take();
}

bool Decode(std::span<const std::byte> payload, SubmitJob& into) {
    const Unpacker unpacker(payload);
    if (!unpacker.ok()) return false;

    into.inputPaths = GetStrings(unpacker.root(), "inputPaths");
    into.targetProfile = GetStr(unpacker.root(), "targetProfile");
    into.interactive = GetBool(unpacker.root(), "interactive", true);

    ForEachElement(unpacker.root(), "options", [&into](const msgpack_object& element) {
        OptionValue option;
        option.key = GetStr(element, "key");
        if (option.key.empty()) return;  // an option with no name configures nothing

        option.kind = GetU8(element, "kind");
        switch (option.kind) {
            case 0: option.boolValue = GetBool(element, "value"); break;
            case 1: option.intValue = GetI64(element, "value"); break;
            case 2: option.doubleValue = GetDbl(element, "value"); break;
            default:
                option.kind = 3;
                option.stringValue = GetStr(element, "value");
                break;
        }
        into.options.push_back(std::move(option));
    });

    if (const msgpack_object* output = Find(unpacker.root(), "output");
        output != nullptr && output->type == MSGPACK_OBJECT_MAP) {
        into.output.directory = GetStr(*output, "directory");
        into.output.namingTemplate = GetStr(*output, "namingTemplate");
        if (into.output.namingTemplate.empty()) into.output.namingTemplate = "{name}.{ext}";
        const std::uint8_t rule = GetU8(*output, "overwrite");
        // An unrecognised rule falls back to renaming, which is the only choice
        // that cannot destroy a file.
        into.output.overwrite =
            rule <= static_cast<std::uint8_t>(OverwriteRule::kReplace)
                ? static_cast<OverwriteRule>(rule)
                : OverwriteRule::kRename;
    }
    return true;
}

// --- JobAccepted / Progress / TaskDone / JobDone -----------------------------

std::vector<std::byte> Encode(const JobAccepted& message) {
    Packer packer;
    packer.Map(5);
    packer.Field("jobId", message.jobId);
    PackStrings(packer, "planProfiles", message.planProfiles);
    packer.Field("fidelity", static_cast<std::uint64_t>(message.fidelity));
    packer.Field("estimatedMs", message.estimatedMs);
    packer.Field("taskCount", static_cast<std::uint64_t>(message.taskCount));
    return packer.Take();
}

bool Decode(std::span<const std::byte> payload, JobAccepted& into) {
    const Unpacker unpacker(payload);
    if (!unpacker.ok()) return false;

    into.jobId = GetU64(unpacker.root(), "jobId");
    into.planProfiles = GetStrings(unpacker.root(), "planProfiles");
    into.fidelity = GetU8(unpacker.root(), "fidelity");
    into.estimatedMs = GetDbl(unpacker.root(), "estimatedMs");
    into.taskCount = GetU32(unpacker.root(), "taskCount");
    return true;
}

std::vector<std::byte> Encode(const Progress& message) {
    Packer packer;
    packer.Map(5);
    packer.Field("jobId", message.jobId);
    packer.Field("taskIndex", static_cast<std::uint64_t>(message.taskIndex));
    packer.Field("fraction", message.fraction);
    packer.Field("currentHop", static_cast<std::uint64_t>(message.currentHop));
    packer.Field("bytesWritten", message.bytesWritten);
    return packer.Take();
}

bool Decode(std::span<const std::byte> payload, Progress& into) {
    const Unpacker unpacker(payload);
    if (!unpacker.ok()) return false;

    into.jobId = GetU64(unpacker.root(), "jobId");
    into.taskIndex = GetU32(unpacker.root(), "taskIndex");
    into.fraction = GetDbl(unpacker.root(), "fraction");
    into.currentHop = GetU32(unpacker.root(), "currentHop");
    into.bytesWritten = GetU64(unpacker.root(), "bytesWritten");
    return true;
}

std::vector<std::byte> Encode(const TaskDone& message) {
    Packer packer;
    packer.Map(8);
    packer.Field("jobId", message.jobId);
    packer.Field("taskIndex", static_cast<std::uint64_t>(message.taskIndex));
    packer.Field("status", static_cast<std::uint64_t>(message.status));
    packer.Field("inputPath", message.inputPath);
    packer.Field("outputPath", message.outputPath);
    packer.Field("fidelity", static_cast<std::uint64_t>(message.fidelity));

    packer.Key("notes");
    packer.Array(message.notes.size());
    for (const NoteWire& note : message.notes) {
        packer.Map(3);
        packer.Field("code", note.code);
        packer.Field("detail", note.detail);
        packer.Field("location", note.location);
    }

    packer.Field("error", message.error);
    return packer.Take();
}

bool Decode(std::span<const std::byte> payload, TaskDone& into) {
    const Unpacker unpacker(payload);
    if (!unpacker.ok()) return false;

    into.jobId = GetU64(unpacker.root(), "jobId");
    into.taskIndex = GetU32(unpacker.root(), "taskIndex");
    into.status = GetU8(unpacker.root(), "status");
    into.inputPath = GetStr(unpacker.root(), "inputPath");
    into.outputPath = GetStr(unpacker.root(), "outputPath");
    into.fidelity = GetU8(unpacker.root(), "fidelity");
    into.error = GetStr(unpacker.root(), "error");

    ForEachElement(unpacker.root(), "notes", [&into](const msgpack_object& element) {
        NoteWire note;
        note.code = GetStr(element, "code");
        note.detail = GetStr(element, "detail");
        note.location = GetStr(element, "location");
        into.notes.push_back(std::move(note));
    });
    return true;
}

std::vector<std::byte> Encode(const JobDone& message) {
    Packer packer;
    packer.Map(4);
    packer.Field("jobId", message.jobId);
    packer.Field("succeeded", static_cast<std::uint64_t>(message.succeeded));
    packer.Field("failed", static_cast<std::uint64_t>(message.failed));
    packer.Field("cancelled", static_cast<std::uint64_t>(message.cancelled));
    return packer.Take();
}

bool Decode(std::span<const std::byte> payload, JobDone& into) {
    const Unpacker unpacker(payload);
    if (!unpacker.ok()) return false;

    into.jobId = GetU64(unpacker.root(), "jobId");
    into.succeeded = GetU32(unpacker.root(), "succeeded");
    into.failed = GetU32(unpacker.root(), "failed");
    into.cancelled = GetU32(unpacker.root(), "cancelled");
    return true;
}

// --- Cancel / Error ----------------------------------------------------------

std::vector<std::byte> Encode(const Cancel& message) {
    Packer packer;
    packer.Map(2);
    packer.Field("jobId", message.jobId);
    packer.Field("taskIndex", message.taskIndex);
    return packer.Take();
}

bool Decode(std::span<const std::byte> payload, Cancel& into) {
    const Unpacker unpacker(payload);
    if (!unpacker.ok()) return false;

    into.jobId = GetU64(unpacker.root(), "jobId");
    into.taskIndex = GetI64(unpacker.root(), "taskIndex", -1);
    return true;
}

std::vector<std::byte> Encode(const Error& message) {
    Packer packer;
    packer.Map(3);
    packer.Field("jobId", message.jobId);
    packer.Field("status", static_cast<std::uint64_t>(message.status));
    packer.Field("message", message.message);
    return packer.Take();
}

bool Decode(std::span<const std::byte> payload, Error& into) {
    const Unpacker unpacker(payload);
    if (!unpacker.ok()) return false;

    into.jobId = GetU64(unpacker.root(), "jobId");
    into.status = GetU8(unpacker.root(), "status");
    into.message = GetStr(unpacker.root(), "message");
    return true;
}

}  // namespace morph::ipc
