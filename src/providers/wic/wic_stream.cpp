#include "wic_stream.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace morph::wic {
namespace {

// The largest image Morph will hold in memory while encoding. A 40000x40000 PNG
// is section 6.2's example of a file that "must fail cleanly, not OOM the
// worker"; the job object catches the decode side, and this catches the encode.
constexpr std::uint64_t kMaxEncodedBytes = 1ull * 1024 * 1024 * 1024;

}  // namespace

// --- SourceStream ------------------------------------------------------------

HRESULT SourceStream::QueryInterface(REFIID riid, void** out) {
    if (out == nullptr) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IStream || riid == IID_ISequentialStream) {
        *out = static_cast<IStream*>(this);
        AddRef();
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG SourceStream::AddRef() { return static_cast<ULONG>(::InterlockedIncrement(&references_)); }

ULONG SourceStream::Release() {
    // Never deletes: the object is owned by the provider's stack frame and only
    // borrowed by WIC. Returning the count keeps COM's contract without giving
    // a decoder the ability to free something it does not own.
    return static_cast<ULONG>(::InterlockedDecrement(&references_));
}

HRESULT SourceStream::Read(void* buffer, ULONG bytes, ULONG* read) {
    if (buffer == nullptr) return E_POINTER;
    if (read != nullptr) *read = 0;
    if (bytes == 0) return S_OK;

    auto* out = static_cast<std::byte*>(buffer);
    std::size_t total = 0;
    while (total < bytes) {
        const std::size_t got = source_->Read(std::span(out + total, bytes - total));
        if (got == 0) break;  // end of input; a short read is not an error
        total += got;
    }
    position_ += total;
    if (read != nullptr) *read = static_cast<ULONG>(total);
    return total == bytes ? S_OK : S_FALSE;
}

HRESULT SourceStream::Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* position) {
    std::uint64_t target = 0;
    switch (origin) {
        case STREAM_SEEK_SET:
            if (move.QuadPart < 0) return STG_E_INVALIDFUNCTION;
            target = static_cast<std::uint64_t>(move.QuadPart);
            break;
        case STREAM_SEEK_CUR:
            if (move.QuadPart < 0 &&
                static_cast<std::uint64_t>(-move.QuadPart) > position_) {
                return STG_E_INVALIDFUNCTION;
            }
            target = position_ + static_cast<std::uint64_t>(move.QuadPart);
            break;
        case STREAM_SEEK_END: {
            const std::optional<std::uint64_t> size = source_->Size();
            if (!size) return STG_E_INVALIDFUNCTION;
            if (move.QuadPart < 0 && static_cast<std::uint64_t>(-move.QuadPart) > *size) {
                return STG_E_INVALIDFUNCTION;
            }
            target = *size + static_cast<std::uint64_t>(move.QuadPart);
            break;
        }
        default:
            return STG_E_INVALIDFUNCTION;
    }

    if (!source_->Seek(target)) return STG_E_INVALIDFUNCTION;
    position_ = target;
    if (position != nullptr) position->QuadPart = target;
    return S_OK;
}

HRESULT SourceStream::Stat(STATSTG* stat, DWORD /*flags*/) {
    if (stat == nullptr) return E_POINTER;
    *stat = STATSTG{};
    stat->type = STGTY_STREAM;
    // A decoder that cannot learn the length still works; one that is told a
    // wrong length does not. An unknown size is reported as zero, which WIC
    // treats as "read until it ends".
    stat->cbSize.QuadPart = source_->Size().value_or(0);
    return S_OK;
}

// --- MemoryStream ------------------------------------------------------------

HRESULT MemoryStream::QueryInterface(REFIID riid, void** out) {
    if (out == nullptr) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IStream || riid == IID_ISequentialStream) {
        *out = static_cast<IStream*>(this);
        AddRef();
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG MemoryStream::AddRef() { return static_cast<ULONG>(::InterlockedIncrement(&references_)); }

ULONG MemoryStream::Release() { return static_cast<ULONG>(::InterlockedDecrement(&references_)); }

HRESULT MemoryStream::Read(void* buffer, ULONG bytes, ULONG* read) {
    if (buffer == nullptr) return E_POINTER;

    const std::uint64_t available = position_ >= bytes_.size() ? 0 : bytes_.size() - position_;
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(bytes, available));
    if (count > 0) {
        std::memcpy(buffer, bytes_.data() + position_, count);
        position_ += count;
    }
    if (read != nullptr) *read = static_cast<ULONG>(count);
    return count == bytes ? S_OK : S_FALSE;
}

HRESULT MemoryStream::Write(const void* buffer, ULONG bytes, ULONG* written) {
    if (buffer == nullptr) return E_POINTER;
    if (written != nullptr) *written = 0;
    if (bytes == 0) return S_OK;

    if (position_ + bytes > kMaxEncodedBytes) return STG_E_MEDIUMFULL;

    const auto end = static_cast<std::size_t>(position_ + bytes);
    if (end > bytes_.size()) bytes_.resize(end);
    std::memcpy(bytes_.data() + position_, buffer, bytes);
    position_ = end;
    if (written != nullptr) *written = bytes;
    return S_OK;
}

HRESULT MemoryStream::Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* position) {
    std::int64_t base = 0;
    switch (origin) {
        case STREAM_SEEK_SET: base = 0; break;
        case STREAM_SEEK_CUR: base = static_cast<std::int64_t>(position_); break;
        case STREAM_SEEK_END: base = static_cast<std::int64_t>(bytes_.size()); break;
        default: return STG_E_INVALIDFUNCTION;
    }

    const std::int64_t target = base + move.QuadPart;
    if (target < 0) return STG_E_INVALIDFUNCTION;
    if (static_cast<std::uint64_t>(target) > kMaxEncodedBytes) return STG_E_MEDIUMFULL;

    // Seeking past the end is legal and does not extend the stream; the next
    // write does. That is what an encoder patching a header expects.
    position_ = static_cast<std::uint64_t>(target);
    if (position != nullptr) position->QuadPart = position_;
    return S_OK;
}

HRESULT MemoryStream::SetSize(ULARGE_INTEGER size) {
    if (size.QuadPart > kMaxEncodedBytes) return STG_E_MEDIUMFULL;
    bytes_.resize(static_cast<std::size_t>(size.QuadPart));
    return S_OK;
}

HRESULT MemoryStream::Stat(STATSTG* stat, DWORD /*flags*/) {
    if (stat == nullptr) return E_POINTER;
    *stat = STATSTG{};
    stat->type = STGTY_STREAM;
    stat->cbSize.QuadPart = bytes_.size();
    return S_OK;
}

}  // namespace morph::wic
