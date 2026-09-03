// Morph — the broker protocol (architecture section 10.2).
//
// The message set is section 10.2's table, unchanged. What this file adds is the
// discipline section 12.3 asks for:
//
//   "Design the broker protocol so this remains possible; do not put C++-only
//    types in the client-facing headers."
//
// The escape hatch it is protecting is rewriting Morph.App.exe in C# with a
// C++/WinRT projection if the XAML tooling becomes a schedule risk. That only
// stays possible if the WIRE FORMAT is the contract rather than these structs.
// So every field below is a string, an integer, a float, a bool, or a sequence
// of those -- shapes MessagePack expresses natively and any language can read.
// No variants, no optionals, no pointers, no inheritance.
//
// FORWARD COMPATIBILITY. Section 10.2 expects the schema to "churn heavily for
// the first six months", so fields are keyed by NAME on the wire, not by
// position. A decoder ignores keys it does not know and defaults keys that are
// absent, which means a newer broker and an older shell extension keep working
// while the version handshake decides whether they should.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace morph::ipc {

// Bumped when a change is not backward compatible. Section 10.2: "Version
// mismatch closes the pipe rather than negotiating." Negotiation across versions
// is a source of bugs that only appear in the field, and both halves of Morph
// ship in the same package.
inline constexpr std::uint32_t kProtocolVersion = 1;

enum class MessageType : std::uint16_t {
    kInvalid = 0,

    kHello = 1,          // client -> broker
    kHelloAck = 2,       // broker -> client
    kQueryTargets = 3,   // client -> broker
    kTargets = 4,        // broker -> client
    kSubmitJob = 5,      // client -> broker
    kJobAccepted = 6,    // broker -> client
    kProgress = 7,       // broker -> client
    kTaskDone = 8,       // broker -> client
    kJobDone = 9,        // broker -> client
    kCancel = 10,        // client -> broker
    kError = 11,         // broker -> client
};

[[nodiscard]] std::string_view Name(MessageType type) noexcept;

// Which client is talking. The broker uses this for nothing security-related --
// a client can claim anything -- but it makes logs readable and lets the broker
// apply the section 11.2 CPU weighting, where an interactive request outranks a
// background batch.
enum class ClientKind : std::uint8_t {
    kUnknown = 0,
    kShellExtension = 1,
    kApp = 2,
    kCommandLine = 3,
};

enum class OverwriteRule : std::uint8_t {
    kRename = 0,  // "report (1).pdf"; the default, because it never destroys anything
    kSkip = 1,
    kReplace = 2,
};

// --- Client to broker --------------------------------------------------------

struct Hello {
    std::uint32_t protocolVersion = kProtocolVersion;
    ClientKind kind = ClientKind::kUnknown;
    std::string clientVersion;
};

struct HelloAck {
    std::uint32_t protocolVersion = kProtocolVersion;
    std::string brokerVersion;
    // The capability table generation the shell extension's memory map should be
    // at (section 9.2). A client holding an older one re-reads it.
    std::uint64_t capabilityGeneration = 0;
};

// "list of source profiles -> available target profiles with fidelity ratings.
//  Used by the app, never by the shell extension." The shell extension reads the
//  memory-mapped capability table instead, because asking here would mean a pipe
//  round trip while the user's right-click hangs (section 9.2).
struct QueryTargets {
    std::vector<std::string> sourceProfiles;
    // Bytes of the largest input, so the estimate comes back scaled. Zero means
    // "do not bother estimating".
    std::uint64_t largestInputBytes = 0;
};

struct TargetOption {
    std::string profile;
    std::uint8_t fidelity = 0;  // morph::Fidelity, worst along the path
    std::uint32_t hopCount = 0;
    double estimatedMs = 0.0;
    bool requiresDisk = false;
};

struct Targets {
    std::vector<TargetOption> targets;
};

// The output policy, kept as its own shape because it is the part users get
// wrong and support questions are about.
struct OutputPolicy {
    // Empty means "beside each input", which is what the shell extension wants.
    std::string directory;
    // "{name}.{ext}" with {name}, {ext}, {index} and {target} substitutions.
    std::string namingTemplate = "{name}.{ext}";
    OverwriteRule overwrite = OverwriteRule::kRename;
};

// One resolved option. A map would be the obvious shape, but MessagePack maps
// with heterogeneous value types are awkward to read from a strongly typed
// language, and section 12.3 cares about that.
struct OptionValue {
    std::string key;
    // 0 = bool, 1 = int, 2 = double, 3 = string. Exactly one of the fields
    // below is meaningful.
    std::uint8_t kind = 0;
    bool boolValue = false;
    std::int64_t intValue = 0;
    double doubleValue = 0.0;
    std::string stringValue;
};

struct SubmitJob {
    std::vector<std::string> inputPaths;
    std::string targetProfile;
    std::vector<OptionValue> options;
    OutputPolicy output;
    // True when a person is waiting, which sets the job object's CPU rate weight
    // to 3 rather than 9 (section 11.2).
    bool interactive = true;
};

struct Cancel {
    std::uint64_t jobId = 0;
    // -1 cancels the whole job; otherwise the one task.
    std::int64_t taskIndex = -1;
};

// --- Broker to client --------------------------------------------------------

struct JobAccepted {
    std::uint64_t jobId = 0;
    // "the resolved plan (the hop list, so the UI can show it)". Profile ids in
    // order, source first.
    std::vector<std::string> planProfiles;
    std::uint8_t fidelity = 0;
    double estimatedMs = 0.0;
    std::uint32_t taskCount = 0;
};

struct Progress {
    std::uint64_t jobId = 0;
    std::uint32_t taskIndex = 0;
    double fraction = 0.0;  // 0..1 across the whole task, hops already weighted
    std::uint32_t currentHop = 0;
    std::uint64_t bytesWritten = 0;
};

// A fidelity note actually incurred, as opposed to the one the plan predicted.
struct NoteWire {
    std::string code;
    std::string detail;
    std::string location;
};

struct TaskDone {
    std::uint64_t jobId = 0;
    std::uint32_t taskIndex = 0;
    std::uint8_t status = 0;  // morph::Status
    std::string inputPath;
    std::string outputPath;
    std::uint8_t fidelity = 0;
    std::vector<NoteWire> notes;
    // Section 12.2: "Failures listed with a real reason, never 'conversion
    // failed'." Empty when status is ok.
    std::string error;
};

struct JobDone {
    std::uint64_t jobId = 0;
    std::uint32_t succeeded = 0;
    std::uint32_t failed = 0;
    std::uint32_t cancelled = 0;
};

struct Error {
    // Set when the failure belongs to a job; 0 for a protocol-level failure.
    std::uint64_t jobId = 0;
    std::uint8_t status = 0;  // morph::Status
    std::string message;
};

}  // namespace morph::ipc
