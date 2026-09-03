#include "morph/core/temp_store.h"

#include <atomic>
#include <system_error>

#include <windows.h>

namespace morph {
namespace {

// Unique without being guessable-adjacent: the process id keeps two concurrent
// workers apart, and the counter keeps two stores inside one worker apart.
std::string UniqueSuffix() {
    static std::atomic<unsigned> sequence{0};
    return std::to_string(::GetCurrentProcessId()) + "-" +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

// Keeps only the extension from a caller's hint, and only if it looks like one.
// The hint can carry a filename derived from user input, and letting that reach
// the filesystem is exactly the path-handling problem section 15.2 is about.
std::string SafeExtension(std::string_view hint) {
    const auto dot = hint.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 == hint.size()) return {};

    const std::string_view extension = hint.substr(dot);
    if (extension.size() > 8) return {};
    for (const char c : extension.substr(1)) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!ok) return {};
    }
    return std::string(extension);
}

}  // namespace

ScopedTempStore::ScopedTempStore(std::string_view label) {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) return;  // Acquire will then return empty paths and callers report IO errors

    std::string name = "morph-";
    for (const char c : label) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '-' || c == '_';
        if (ok) name.push_back(c);
    }
    name += "-";
    name += UniqueSuffix();

    const std::filesystem::path candidate = base / name;
    if (std::filesystem::create_directories(candidate, ec); !ec) root_ = candidate;
}

ScopedTempStore::~ScopedTempStore() { Reap(); }

std::filesystem::path ScopedTempStore::Acquire(std::string_view hint) {
    if (root_.empty()) return {};

    std::lock_guard<std::mutex> lock(mutex_);
    const std::string name = "t" + std::to_string(counter_++) + SafeExtension(hint);
    return root_ / name;
}

void ScopedTempStore::Reap() noexcept {
    if (root_.empty()) return;

    std::error_code ignored;
    // remove_all rather than removing what Acquire handed out: a provider may
    // have created siblings of its own next to the file it asked for, and a
    // WebView2 user data folder does exactly that.
    std::filesystem::remove_all(root_, ignored);
    root_.clear();
}

}  // namespace morph
