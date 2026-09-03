#include "morph/core/ipc/frame.h"

#include <algorithm>
#include <cstring>

namespace morph::ipc {
namespace {

void PutU16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFFu));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
}

void PutU32(std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFFu));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::byte>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::byte>((value >> 24) & 0xFFu));
}

std::uint16_t ReadU16(const std::byte* at) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(at[0]) |
                                      (static_cast<std::uint16_t>(at[1]) << 8));
}

std::uint32_t ReadU32(const std::byte* at) {
    return static_cast<std::uint32_t>(at[0]) | (static_cast<std::uint32_t>(at[1]) << 8) |
           (static_cast<std::uint32_t>(at[2]) << 16) | (static_cast<std::uint32_t>(at[3]) << 24);
}

bool KnownType(std::uint16_t raw) {
    return raw >= static_cast<std::uint16_t>(MessageType::kHello) &&
           raw <= static_cast<std::uint16_t>(MessageType::kError);
}

}  // namespace

std::string_view Name(MessageType type) noexcept {
    switch (type) {
        case MessageType::kInvalid:      return "invalid";
        case MessageType::kHello:        return "Hello";
        case MessageType::kHelloAck:     return "HelloAck";
        case MessageType::kQueryTargets: return "QueryTargets";
        case MessageType::kTargets:      return "Targets";
        case MessageType::kSubmitJob:    return "SubmitJob";
        case MessageType::kJobAccepted:  return "JobAccepted";
        case MessageType::kProgress:     return "Progress";
        case MessageType::kTaskDone:     return "TaskDone";
        case MessageType::kJobDone:      return "JobDone";
        case MessageType::kCancel:       return "Cancel";
        case MessageType::kError:        return "Error";
    }
    return "unknown";
}

std::vector<std::byte> EncodeFrame(MessageType type, std::span<const std::byte> payload) {
    std::vector<std::byte> out;
    out.reserve(kFrameHeaderBytes + payload.size());

    PutU32(out, static_cast<std::uint32_t>(payload.size()));
    PutU16(out, static_cast<std::uint16_t>(type));
    PutU16(out, 0);  // flags, reserved

    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::optional<FrameHeader> DecodeFrameHeader(std::span<const std::byte> bytes) {
    if (bytes.size() < kFrameHeaderBytes) return std::nullopt;

    FrameHeader header;
    header.payloadBytes = ReadU32(bytes.data());
    if (header.payloadBytes > kMaxPayloadBytes) return std::nullopt;

    const std::uint16_t rawType = ReadU16(bytes.data() + 4);
    // An unknown type is not a decode failure: a newer peer may send a message
    // this build has never heard of, and the receiver skips it. The version
    // handshake is what decides whether that should be happening at all.
    header.type = KnownType(rawType) ? static_cast<MessageType>(rawType) : MessageType::kInvalid;
    header.flags = ReadU16(bytes.data() + 6);
    return header;
}

bool FrameReader::Append(std::span<const std::byte> bytes) {
    if (failed_) return false;

    // Reclaim the consumed prefix rather than growing without bound. Doing it
    // here, before appending, keeps the amortised cost linear across a long
    // stream of small progress messages.
    if (consumed_ > 0 && consumed_ == buffer_.size()) {
        buffer_.clear();
        consumed_ = 0;
    } else if (consumed_ > 64 * 1024 && consumed_ * 2 > buffer_.size()) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(consumed_));
        consumed_ = 0;
    }

    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());

    // A declared length beyond the cap is unrecoverable: there is no way to know
    // where the next frame starts, so resynchronising would be guessing.
    if (buffer_.size() - consumed_ >= kFrameHeaderBytes) {
        const std::uint32_t declared = ReadU32(buffer_.data() + consumed_);
        if (declared > kMaxPayloadBytes) {
            failed_ = true;
            return false;
        }
    }
    return true;
}

std::optional<FrameReader::Frame> FrameReader::Next() {
    if (failed_) return std::nullopt;

    const std::size_t available = buffer_.size() - consumed_;
    if (available < kFrameHeaderBytes) return std::nullopt;

    const std::span<const std::byte> view(buffer_.data() + consumed_, available);
    const std::optional<FrameHeader> header = DecodeFrameHeader(view);
    if (!header) {
        failed_ = true;
        return std::nullopt;
    }
    if (available < kFrameHeaderBytes + header->payloadBytes) return std::nullopt;

    Frame frame;
    frame.type = header->type;
    frame.flags = header->flags;
    const std::byte* payload = buffer_.data() + consumed_ + kFrameHeaderBytes;
    frame.payload.assign(payload, payload + header->payloadBytes);

    consumed_ += kFrameHeaderBytes + header->payloadBytes;
    return frame;
}

void FrameReader::Reset() {
    buffer_.clear();
    consumed_ = 0;
    failed_ = false;
}

}  // namespace morph::ipc
