// Morph — the streaming source and sink (architecture section 5).
//
// "Streaming, so no provider ever assumes the whole file fits in memory."
//
// These two interfaces are also what makes multi-hop conversion affordable: hops
// are connected by an ISource/ISink pair rather than by a temp file, so a
// four-hop conversion of a 2 GB video does not need 8 GB of scratch space
// (section 4.3). Only an edge that declares requiresDisk gets a real file.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace morph {

struct ISource {
    virtual ~ISource() = default;

    // Fills as much of `into` as it can. Returns 0 only at end of input; a short
    // read is not the end. Throws nothing — I/O failure surfaces as a Status
    // from the provider, because a throw across a provider boundary would have
    // to be caught by the worker's job loop anyway.
    virtual std::size_t Read(std::span<std::byte> into) = 0;

    // False means forward-only. A reader that needs to seek must say so through
    // its edge declaration, and the executor will interpose a spill file.
    virtual bool Seek(std::uint64_t position) = 0;

    // nullopt when the length is genuinely unknown, e.g. a pipe from a previous
    // hop. Callers must not assume a size is available.
    [[nodiscard]] virtual std::optional<std::uint64_t> Size() const = 0;
};

struct ISink {
    virtual ~ISink() = default;

    virtual void Write(std::span<const std::byte> bytes) = 0;

    // Atomic rename on success. Until this is called the output does not exist
    // at its final path, which is what makes cancellation safe: section 11.3
    // requires that cancelling never leaves a half-written file where the user
    // expects a real one.
    virtual void Commit() = 0;

    // Discards whatever was written. Must be safe to call after Commit, after a
    // failed Write, and from a destructor during unwinding.
    virtual void Abandon() noexcept = 0;
};

// --- Concrete implementations -----------------------------------------------

// Reads a file with a sequential-scan hint. Opens with FILE_SHARE_READ so that
// converting a file the user has open in another application works.
class FileSource final : public ISource {
public:
    // Throws std::system_error if the file cannot be opened.
    explicit FileSource(const std::filesystem::path& path);
    ~FileSource() override;

    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    std::size_t Read(std::span<std::byte> into) override;
    bool Seek(std::uint64_t position) override;
    [[nodiscard]] std::optional<std::uint64_t> Size() const override;

private:
    void* handle_;  // HANDLE, kept opaque so this header stays Windows-free
    std::uint64_t size_ = 0;
};

// Writes to a sibling temp file and renames on Commit. The temp file lives next
// to the destination rather than in %TEMP% so the rename stays on one volume and
// therefore stays atomic.
class FileSink final : public ISink {
public:
    explicit FileSink(std::filesystem::path destination);
    ~FileSink() override;

    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;

    void Write(std::span<const std::byte> bytes) override;
    void Commit() override;
    void Abandon() noexcept override;

    [[nodiscard]] const std::filesystem::path& destination() const noexcept {
        return destination_;
    }
    [[nodiscard]] std::uint64_t written() const noexcept { return written_; }

private:
    std::filesystem::path destination_;
    std::filesystem::path scratch_;
    void* handle_;
    std::uint64_t written_ = 0;
    bool committed_ = false;
};

// In-memory source. For tests, and for the small intermediates that genuinely do
// fit: a 40-page document's HTML is a few hundred KiB.
class MemorySource final : public ISource {
public:
    explicit MemorySource(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {}
    explicit MemorySource(std::string_view text);

    std::size_t Read(std::span<std::byte> into) override;
    bool Seek(std::uint64_t position) override;
    [[nodiscard]] std::optional<std::uint64_t> Size() const override;

private:
    std::vector<std::byte> bytes_;
    std::uint64_t position_ = 0;
};

// In-memory sink. Commit is a no-op; the buffer is the output.
class MemorySink final : public ISink {
public:
    void Write(std::span<const std::byte> bytes) override;
    void Commit() override { committed_ = true; }
    void Abandon() noexcept override {
        bytes_.clear();
        committed_ = false;
    }

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool committed() const noexcept { return committed_; }
    [[nodiscard]] std::string_view text() const noexcept;

private:
    std::vector<std::byte> bytes_;
    bool committed_ = false;
};

// --- Helpers ----------------------------------------------------------------

// Reads at most `limit` bytes. Used by Probe, which section 5 caps at 8 KiB: it
// runs on every file in a selection and a user can select ten thousand files.
[[nodiscard]] std::vector<std::byte> ReadAtMost(ISource& source, std::size_t limit);

// Drains a source into a sink in fixed-size chunks. The chunk size is a
// deliberate compromise: large enough that syscall overhead disappears, small
// enough that a cancellation check between chunks stays inside the 100 ms
// granularity section 5 requires.
inline constexpr std::size_t kStreamChunkBytes = 256 * 1024;

void WriteAll(ISink& sink, std::span<const std::byte> bytes);
void WriteAll(ISink& sink, std::string_view text);

}  // namespace morph
