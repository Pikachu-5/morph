#include "morph/core/ipc/codec.h"

#include <cstring>
#include <string>
#include <string_view>

#include <msgpack.h>

#include "msgpack_util.h"

namespace morph::ipc {

using namespace morph::ipc::detail;

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
