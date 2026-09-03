// Morph — frame encoding and decoding (architecture section 10.2).
//
// "Length-prefixed frames carrying MessagePack (msgpack-c, Boost Software
//  License -- permissive). MessagePack rather than JSON because progress
//  messages fly at 10 Hz per job and parsing cost adds up; MessagePack rather
//  than FlatBuffers because the schema will churn heavily for the first six
//  months and the zero-copy win is irrelevant at these message sizes."
//
// A named pipe in message mode already preserves boundaries, so the length
// prefix looks redundant. It is not, for three reasons: a client can be handed a
// byte-mode handle by mistake and the failure would be silent corruption rather
// than an error; the same codec is used by the tests over a plain buffer; and a
// length that disagrees with the frame is the cheapest possible tripwire for a
// desynchronised or hostile peer.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "morph/core/ipc/protocol.h"

namespace morph::ipc {

// [u32 payloadBytes][u16 type][u16 flags]. Little-endian, because every target
// is, and saying so beats leaving it implicit.
inline constexpr std::size_t kFrameHeaderBytes = 8;

// A frame this large is either a bug or an attack. Section 9.3 caps a selection
// long before it gets here -- ten thousand paths are marshalled in chunks -- so
// nothing legitimate approaches this.
inline constexpr std::uint32_t kMaxPayloadBytes = 16u * 1024 * 1024;

struct FrameHeader {
    std::uint32_t payloadBytes = 0;
    MessageType type = MessageType::kInvalid;
    std::uint16_t flags = 0;
};

// Writes the header for `payload` and appends the payload itself.
[[nodiscard]] std::vector<std::byte> EncodeFrame(MessageType type,
                                                 std::span<const std::byte> payload);

// Reads a header. Returns nullopt when there are fewer than kFrameHeaderBytes
// available or the declared length is beyond the cap -- the caller must treat a
// nullopt with enough bytes present as a protocol violation and close the pipe.
[[nodiscard]] std::optional<FrameHeader> DecodeFrameHeader(std::span<const std::byte> bytes);

// Accumulates bytes from a transport and hands back whole frames.
//
// Written as a separate object rather than folded into the pipe client because
// it is where a desynchronisation bug would live, and it is far easier to test
// something that takes bytes and returns frames than something that also owns a
// handle.
class FrameReader {
public:
    struct Frame {
        MessageType type = MessageType::kInvalid;
        std::uint16_t flags = 0;
        std::vector<std::byte> payload;
    };

    // Appends received bytes. Returns false when the stream is unusable -- a
    // frame beyond the cap, or a header that cannot be a frame -- after which
    // the connection must be closed rather than resynchronised.
    [[nodiscard]] bool Append(std::span<const std::byte> bytes);

    // The next complete frame, or nullopt when more bytes are needed.
    [[nodiscard]] std::optional<Frame> Next();

    [[nodiscard]] bool Failed() const noexcept { return failed_; }
    [[nodiscard]] std::size_t Buffered() const noexcept { return buffer_.size() - consumed_; }

    void Reset();

private:
    std::vector<std::byte> buffer_;
    std::size_t consumed_ = 0;
    bool failed_ = false;
};

}  // namespace morph::ipc
