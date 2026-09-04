// IStream adapters (architecture section 5's ISource/ISink meeting COM).
//
// Windows speaks IStream and Morph speaks ISource/ISink, and neither is going to
// change for the other. These are the two adapters that meet in the middle.
//
// Shared rather than owned by one provider, because both COM-facing providers
// need exactly this: WIC decodes from an IStream, and Media Foundation wraps one
// with MFCreateMFByteStreamOnStream. They are in a provider-support library
// rather than in morph.core, which is deliberately free of COM (see
// src/core/CMakeLists.txt).
//
// Only the methods these callers actually use are implemented; the rest return
// E_NOTIMPL rather than pretending. An adapter that silently succeeds at
// something it did not do is worse than one that says no.
#pragma once

#include <cstdint>
#include <vector>

#include <windows.h>

#include <objidl.h>

#include "morph/core/stream.h"

namespace morph::comio {

// Read-only, over an ISource. Tracks its own position because ISource has a
// Seek but no Tell -- a decoder that seeks backwards for a directory entry (TIFF
// does this constantly) needs both.
class SourceStream final : public IStream {
public:
    explicit SourceStream(ISource& source) : source_(&source) {}

    // COM lifetime. These objects are stack-owned by the provider and handed to
    // WIC for the duration of one call, so the reference count exists to satisfy
    // COM rather than to manage anything.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE Read(void* buffer, ULONG bytes, ULONG* read) override;
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin,
                                   ULARGE_INTEGER* position) override;
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*,
                                     ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
    ISource* source_;
    std::uint64_t position_ = 0;
    LONG references_ = 1;
};

// Read/write, in memory, flushed to an ISink at the end.
//
// An encoder needs to seek backwards over what it has written -- PNG rewrites
// its IHDR, TIFF patches its IFD offsets -- and ISink is append-only by design,
// because that is what makes the temp-file-then-rename guarantee in section 11.3
// possible. So the encoder gets memory to work in and the sink gets one write.
//
// The cost is that the encoded image is held whole. That is bounded by the job
// object's memory limit (section 11.2), which is derived from the edge's
// memPeakFactor -- and the image edges declare a factor that accounts for it.
class MemoryStream final : public IStream {
public:
    MemoryStream() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE Read(void* buffer, ULONG bytes, ULONG* read) override;
    HRESULT STDMETHODCALLTYPE Write(const void* buffer, ULONG bytes, ULONG* written) override;
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin,
                                   ULARGE_INTEGER* position) override;
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override;
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*,
                                     ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }

private:
    std::vector<std::byte> bytes_;
    std::uint64_t position_ = 0;
    LONG references_ = 1;
};

}  // namespace morph::comio
