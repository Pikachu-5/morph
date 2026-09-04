#include "morph/core/stream.h"

#include "morph/core/path_safety.h"

#include <algorithm>
#include <cstring>
#include <system_error>

#include <windows.h>

namespace morph {
namespace {

[[noreturn]] void ThrowLastError(const char* what, const std::filesystem::path& path) {
    const DWORD code = ::GetLastError();
    throw std::system_error(static_cast<int>(code), std::system_category(),
                            std::string(what) + ": " + path.string());
}

// Section 15.2 requires long paths and \\?\ prefixing. std::filesystem produces
// a native path already; this adds the prefix that lifts MAX_PATH for absolute
// paths that do not already have it.
std::wstring ExtendedPath(const std::filesystem::path& path) {
    std::wstring native = path.native();
    if (native.size() < 248) return native;
    if (native.starts_with(LR"(\\?\)")) return native;
    if (native.starts_with(LR"(\\)")) return LR"(\\?\UNC\)" + native.substr(2);
    if (path.is_absolute()) return LR"(\\?\)" + native;
    return native;
}

HANDLE AsHandle(void* h) { return static_cast<HANDLE>(h); }

}  // namespace

// --- FileSource --------------------------------------------------------------

FileSource::FileSource(const std::filesystem::path& path) : handle_(INVALID_HANDLE_VALUE) {
    const HANDLE h = ::CreateFileW(
        ExtendedPath(path).c_str(), GENERIC_READ,
        // Converting a file the user has open elsewhere must work. Deny only
        // deletion, so the path cannot vanish underneath us mid-read.
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) ThrowLastError("cannot open for reading", path);

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size)) {
        ::CloseHandle(h);
        ThrowLastError("cannot determine size", path);
    }

    handle_ = h;
    size_ = static_cast<std::uint64_t>(size.QuadPart);
}

FileSource::~FileSource() {
    if (handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(AsHandle(handle_));
}

std::size_t FileSource::Read(std::span<std::byte> into) {
    if (into.empty()) return 0;

    // ReadFile takes a DWORD. Clamp rather than looping here: the caller already
    // reads in a loop, and a partial read is explicitly not end-of-input.
    const DWORD want = static_cast<DWORD>(
        std::min<std::size_t>(into.size(), 64u * 1024u * 1024u));
    DWORD got = 0;
    if (!::ReadFile(AsHandle(handle_), into.data(), want, &got, nullptr)) return 0;
    return got;
}

bool FileSource::Seek(std::uint64_t position) {
    LARGE_INTEGER to{};
    to.QuadPart = static_cast<LONGLONG>(position);
    return ::SetFilePointerEx(AsHandle(handle_), to, nullptr, FILE_BEGIN) != 0;
}

std::optional<std::uint64_t> FileSource::Size() const { return size_; }

// --- FileSink ----------------------------------------------------------------

FileSink::FileSink(std::filesystem::path destination)
    : destination_(std::move(destination)), handle_(INVALID_HANDLE_VALUE) {
    // Section 15.2, checked at the last place a name can still be refused. Every
    // caller above this -- the broker's OutputPolicy, the CLI, a provider
    // writing a sidecar -- reaches CreateFileW through here, and a device name
    // that gets this far opens successfully and swallows the whole conversion.
    if (const auto verdict = io::CheckDestination(destination_); !verdict.ok) {
        throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                verdict.reason);
    }

    // Scratch file next to the destination, so Commit's rename stays on one
    // volume and is therefore atomic. A scratch file in %TEMP% would degrade to
    // a copy across volumes, which is neither atomic nor cheap.
    scratch_ = destination_;
    scratch_ += L".morph-partial";

    const HANDLE h = ::CreateFileW(ExtendedPath(scratch_).c_str(), GENERIC_WRITE,
                                   /*share=*/0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) ThrowLastError("cannot open for writing", scratch_);
    handle_ = h;
}

FileSink::~FileSink() {
    // Abandon is idempotent and safe during unwinding, so an uncommitted sink
    // cleans up after itself no matter how the provider left the stack.
    Abandon();
}

void FileSink::Write(std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
        const DWORD want = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size(), 64u * 1024u * 1024u));
        DWORD put = 0;
        if (!::WriteFile(AsHandle(handle_), bytes.data(), want, &put, nullptr) || put == 0) {
            ThrowLastError("write failed", scratch_);
        }
        bytes = bytes.subspan(put);
        written_ += put;
    }
}

void FileSink::Commit() {
    if (committed_) return;

    if (handle_ != INVALID_HANDLE_VALUE) {
        ::CloseHandle(AsHandle(handle_));
        handle_ = INVALID_HANDLE_VALUE;
    }
    // MoveFileEx with REPLACE_EXISTING, as section 11.3 specifies. The output
    // becomes visible at its real path in one step or not at all.
    if (!::MoveFileExW(ExtendedPath(scratch_).c_str(), ExtendedPath(destination_).c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        ThrowLastError("cannot commit output", destination_);
    }
    committed_ = true;
}

void FileSink::Abandon() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) {
        ::CloseHandle(AsHandle(handle_));
        handle_ = INVALID_HANDLE_VALUE;
    }
    if (!committed_) {
        std::error_code ignored;
        std::filesystem::remove(scratch_, ignored);
    }
}

// --- MemorySource / MemorySink ----------------------------------------------

MemorySource::MemorySource(std::string_view text)
    : bytes_(reinterpret_cast<const std::byte*>(text.data()),
             reinterpret_cast<const std::byte*>(text.data()) + text.size()) {}

std::size_t MemorySource::Read(std::span<std::byte> into) {
    if (position_ >= bytes_.size()) return 0;
    const auto available = static_cast<std::size_t>(bytes_.size() - position_);
    const std::size_t take = std::min(available, into.size());
    std::memcpy(into.data(), bytes_.data() + position_, take);
    position_ += take;
    return take;
}

bool MemorySource::Seek(std::uint64_t position) {
    if (position > bytes_.size()) return false;
    position_ = position;
    return true;
}

std::optional<std::uint64_t> MemorySource::Size() const { return bytes_.size(); }

void MemorySink::Write(std::span<const std::byte> bytes) {
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
}

std::string_view MemorySink::text() const noexcept {
    return std::string_view(reinterpret_cast<const char*>(bytes_.data()), bytes_.size());
}

// --- Helpers -----------------------------------------------------------------

std::vector<std::byte> ReadAtMost(ISource& source, std::size_t limit) {
    std::vector<std::byte> out;
    out.resize(limit);

    std::size_t filled = 0;
    while (filled < limit) {
        const std::size_t got = source.Read(std::span(out).subspan(filled));
        if (got == 0) break;
        filled += got;
    }
    out.resize(filled);
    return out;
}

void WriteAll(ISink& sink, std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
        const std::size_t take = std::min(bytes.size(), kStreamChunkBytes);
        sink.Write(bytes.first(take));
        bytes = bytes.subspan(take);
    }
}

void WriteAll(ISink& sink, std::string_view text) {
    WriteAll(sink, std::span(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

}  // namespace morph
