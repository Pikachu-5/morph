// Morph — MessagePack encoding for the broker protocol (architecture 10.2).
//
// Every message is a MessagePack map with STRING keys, not an array with
// positional fields. That costs a few bytes per message and buys the forward
// compatibility section 10.2 needs while "the schema churns heavily for the
// first six months": an unknown key is ignored, an absent key takes its default,
// and neither side has to be rebuilt in lockstep with the other for a purely
// additive change.
//
// Decoding is total. Every Decode returns false rather than throwing, and a
// malformed payload can only produce a rejected message -- never a partially
// applied one. The broker's pipe is ACL'd to the current user (section 10.1), so
// this is not the primary defence, but a protocol decoder that trusts its input
// is a bad habit regardless of who is on the other end.
#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "morph/core/ipc/protocol.h"

namespace morph::ipc {

[[nodiscard]] std::vector<std::byte> Encode(const Hello& message);
[[nodiscard]] std::vector<std::byte> Encode(const HelloAck& message);
[[nodiscard]] std::vector<std::byte> Encode(const IdentifyFiles& message);
[[nodiscard]] std::vector<std::byte> Encode(const Identified& message);
[[nodiscard]] std::vector<std::byte> Encode(const QueryTargets& message);
[[nodiscard]] std::vector<std::byte> Encode(const Targets& message);
[[nodiscard]] std::vector<std::byte> Encode(const SubmitJob& message);
[[nodiscard]] std::vector<std::byte> Encode(const JobAccepted& message);
[[nodiscard]] std::vector<std::byte> Encode(const Progress& message);
[[nodiscard]] std::vector<std::byte> Encode(const TaskDone& message);
[[nodiscard]] std::vector<std::byte> Encode(const JobDone& message);
[[nodiscard]] std::vector<std::byte> Encode(const Cancel& message);
[[nodiscard]] std::vector<std::byte> Encode(const Error& message);

[[nodiscard]] bool Decode(std::span<const std::byte> payload, Hello& into);
[[nodiscard]] bool Decode(std::span<const std::byte> payload, HelloAck& into);
[[nodiscard]] bool Decode(std::span<const std::byte> payload, IdentifyFiles& into);
[[nodiscard]] bool Decode(std::span<const std::byte> payload, Identified& into);
[[nodiscard]] bool Decode(std::span<const std::byte> payload, QueryTargets& into);
[[nodiscard]] bool Decode(std::span<const std::byte> payload, Targets& into);
[[nodiscard]] bool Decode(std::span<const std::byte> payload, SubmitJob& into);
[[nodiscard]] bool Decode(std::span<const std::byte> payload, JobAccepted& into);
[[nodiscard]] bool Decode(std::span<const std::byte> payload, Progress& into);
[[nodiscard]] bool Decode(std::span<const std::byte> payload, TaskDone& into);
[[nodiscard]] bool Decode(std::span<const std::byte> payload, JobDone& into);
[[nodiscard]] bool Decode(std::span<const std::byte> payload, Cancel& into);
[[nodiscard]] bool Decode(std::span<const std::byte> payload, Error& into);

// The type tag a message travels under, so a sender never has to name it twice.
template <typename T>
[[nodiscard]] constexpr MessageType TypeOf() noexcept;

template <> constexpr MessageType TypeOf<Hello>() noexcept { return MessageType::kHello; }
template <> constexpr MessageType TypeOf<HelloAck>() noexcept { return MessageType::kHelloAck; }
template <> constexpr MessageType TypeOf<IdentifyFiles>() noexcept { return MessageType::kIdentifyFiles; }
template <> constexpr MessageType TypeOf<Identified>() noexcept { return MessageType::kIdentified; }
template <> constexpr MessageType TypeOf<QueryTargets>() noexcept { return MessageType::kQueryTargets; }
template <> constexpr MessageType TypeOf<Targets>() noexcept { return MessageType::kTargets; }
template <> constexpr MessageType TypeOf<SubmitJob>() noexcept { return MessageType::kSubmitJob; }
template <> constexpr MessageType TypeOf<JobAccepted>() noexcept { return MessageType::kJobAccepted; }
template <> constexpr MessageType TypeOf<Progress>() noexcept { return MessageType::kProgress; }
template <> constexpr MessageType TypeOf<TaskDone>() noexcept { return MessageType::kTaskDone; }
template <> constexpr MessageType TypeOf<JobDone>() noexcept { return MessageType::kJobDone; }
template <> constexpr MessageType TypeOf<Cancel>() noexcept { return MessageType::kCancel; }
template <> constexpr MessageType TypeOf<Error>() noexcept { return MessageType::kError; }

}  // namespace morph::ipc
