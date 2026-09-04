#include "morph/core/ipc/worker_protocol.h"

#include <string>
#include <string_view>

#include <msgpack.h>

#include "msgpack_util.h"

namespace morph::ipc {

using namespace morph::ipc::detail;

// --- WorkerHello -------------------------------------------------------------

std::vector<std::byte> Encode(const WorkerHello& message) {
    Packer packer;
    packer.Map(3);
    packer.Field("protocolVersion", static_cast<std::uint64_t>(message.protocolVersion));
    packer.Field("workerVersion", message.workerVersion);
    packer.Field("processId", message.processId);
    return packer.Take();
}

bool Decode(std::span<const std::byte> payload, WorkerHello& into) {
    const Unpacker unpacker(payload);
    if (!unpacker.ok()) return false;

    into.protocolVersion = GetU32(unpacker.root(), "protocolVersion");
    into.workerVersion = GetStr(unpacker.root(), "workerVersion");
    into.processId = GetU64(unpacker.root(), "processId");
    return true;
}

// --- DescribeGraph / GraphDescription ----------------------------------------

// A request with no fields is still an empty MAP rather than nil, so that adding
// a field later is the same additive change every other message allows.
std::vector<std::byte> Encode(const DescribeGraph& /*message*/) {
    Packer packer;
    packer.Map(0);
    return packer.Take();
}

bool Decode(std::span<const std::byte> payload, DescribeGraph& /*into*/) {
    const Unpacker unpacker(payload);
    return unpacker.ok();
}

std::vector<std::byte> Encode(const GraphDescription& message) {
    Packer packer;
    packer.Map(2);

    packer.Key("edges");
    packer.Array(message.edges.size());
    for (const EdgeWire& edge : message.edges) {
        packer.Map(8);
        packer.Field("from", edge.from);
        packer.Field("to", edge.to);
        packer.Field("provider", edge.provider);
        packer.Field("fidelity", static_cast<std::uint64_t>(edge.fidelity));
        packer.Field("generational", static_cast<std::uint64_t>(edge.generational));
        packer.Field("cpuPerMiB", edge.cpuPerMiB);
        packer.Field("memPeakFactor", edge.memPeakFactor);
        packer.Field("requiresDisk", edge.requiresDisk);
    }

    PackStrings(packer, "capabilities", message.capabilities);
    return packer.Take();
}

bool Decode(std::span<const std::byte> payload, GraphDescription& into) {
    const Unpacker unpacker(payload);
    if (!unpacker.ok()) return false;

    into.edges.clear();
    ForEachElement(unpacker.root(), "edges", [&into](const msgpack_object& element) {
        EdgeWire edge;
        edge.from = GetStr(element, "from");
        edge.to = GetStr(element, "to");
        edge.provider = GetStr(element, "provider");
        edge.fidelity = GetU8(element, "fidelity");
        edge.generational = GetU8(element, "generational");
        edge.cpuPerMiB = GetDbl(element, "cpuPerMiB");
        edge.memPeakFactor = GetDbl(element, "memPeakFactor", 1.0);
        edge.requiresDisk = GetBool(element, "requiresDisk");
        // An edge missing an endpoint cannot be added to a graph, and silently
        // dropping it is better than interning an empty profile.
        if (edge.from.empty() || edge.to.empty()) return;
        into.edges.push_back(std::move(edge));
    });

    into.capabilities = GetStrings(unpacker.root(), "capabilities");
    return true;
}

// --- ProbeFiles / ProbeResult ------------------------------------------------

std::vector<std::byte> Encode(const ProbeFiles& message) {
    Packer packer;
    packer.Map(1);
    PackStrings(packer, "paths", message.paths);
    return packer.Take();
}

bool Decode(std::span<const std::byte> payload, ProbeFiles& into) {
    const Unpacker unpacker(payload);
    if (!unpacker.ok()) return false;

    into.paths = GetStrings(unpacker.root(), "paths");
    return true;
}

std::vector<std::byte> Encode(const ProbeResult& message) {
    Packer packer;
    packer.Map(1);
    PackStrings(packer, "profiles", message.profiles);
    return packer.Take();
}

bool Decode(std::span<const std::byte> payload, ProbeResult& into) {
    const Unpacker unpacker(payload);
    if (!unpacker.ok()) return false;

    // GetStrings skips non-string elements, which would silently shorten the
    // answer and misalign it with the request. The caller checks the length
    // against the paths it asked about, so a short answer is caught rather than
    // matched up wrongly.
    into.profiles = GetStrings(unpacker.root(), "profiles");
    return true;
}

// --- RunTask -----------------------------------------------------------------

std::vector<std::byte> Encode(const RunTask& message) {
    Packer packer;
    packer.Map(6);
    packer.Field("jobId", message.jobId);
    packer.Field("taskIndex", static_cast<std::uint64_t>(message.taskIndex));
    packer.Field("inputPath", message.inputPath);
    packer.Field("outputPath", message.outputPath);
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
    return packer.Take();
}

bool Decode(std::span<const std::byte> payload, RunTask& into) {
    const Unpacker unpacker(payload);
    if (!unpacker.ok()) return false;

    into.jobId = GetU64(unpacker.root(), "jobId");
    into.taskIndex = GetU32(unpacker.root(), "taskIndex");
    into.inputPath = GetStr(unpacker.root(), "inputPath");
    into.outputPath = GetStr(unpacker.root(), "outputPath");
    into.targetProfile = GetStr(unpacker.root(), "targetProfile");

    into.options.clear();
    ForEachElement(unpacker.root(), "options", [&into](const msgpack_object& element) {
        OptionValue option;
        option.key = GetStr(element, "key");
        if (option.key.empty()) return;
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
    return true;
}

}  // namespace morph::ipc
