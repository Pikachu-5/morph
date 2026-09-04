#include "morph/core/probe_cache.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include <windows.h>

#include <shlobj.h>

namespace morph {
namespace {

constexpr std::string_view kStampPrefix = "stamp=";

// %LOCALAPPDATA%\Morph\probes.v1 -- per user, per machine, and not roamed: the
// whole point of the file is that it describes THIS machine's codecs.
std::filesystem::path CachePath() {
    PWSTR folder = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder)) ||
        folder == nullptr) {
        return {};
    }
    std::filesystem::path path(folder);
    ::CoTaskMemFree(folder);

    path /= L"Morph";
    return path / L"probes.v1";
}

// What has to be the same for a remembered answer to still be true: the Windows
// build (a codec pack arrives with one), and the binary that asked (a rebuilt
// Morph may ask different questions, or the same question differently).
std::string ComputeStamp() {
    OSVERSIONINFOEXW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    // GetVersionEx lies to unmanifested processes about the major version but
    // reports the BUILD number honestly, which is the part that matters: a codec
    // package arrives with a build.
    DWORD build = 0;
    {
        HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
            ntdll != nullptr ? ::GetProcAddress(ntdll, "RtlGetVersion") : nullptr);
        RTL_OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        if (rtlGetVersion != nullptr && rtlGetVersion(&info) == 0) {
            build = info.dwBuildNumber;
        }
    }

    // The module that contains this code, and when it was built.
    HMODULE self = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&ComputeStamp), &self);
    wchar_t path[MAX_PATH]{};
    ::GetModuleFileNameW(self, path, MAX_PATH);

    std::error_code ec;
    std::uintmax_t size = 0;
    std::int64_t written = 0;
    if (path[0] != L'\0') {
        size = std::filesystem::file_size(path, ec);
        if (ec) size = 0;
        const auto time = std::filesystem::last_write_time(path, ec);
        if (!ec) written = time.time_since_epoch().count();
    }

    // FNV-1a over the module path, so two installs do not share answers.
    std::uint64_t hash = 1469598103934665603ull;
    for (const wchar_t c : std::wstring_view(path)) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ull;
    }

    char text[128]{};
    std::snprintf(text, sizeof(text), "%lu.%llu.%lld.%016llx", static_cast<unsigned long>(build),
                  static_cast<unsigned long long>(size), static_cast<long long>(written),
                  static_cast<unsigned long long>(hash));
    return text;
}

}  // namespace

ProbeCache::ProbeCache() : stamp_(ComputeStamp()) {}

ProbeCache& ProbeCache::Instance() {
    static ProbeCache cache;
    if (!cache.loaded_) {
        cache.loaded_ = true;
        cache.Load();
    }
    return cache;
}

void ProbeCache::Load() {
    const std::filesystem::path path = CachePath();
    if (path.empty()) return;

    std::ifstream file(path);
    if (!file) return;

    std::string line;
    if (!std::getline(file, line)) return;
    // A stamp that does not match means every answer below it is about a
    // different machine, a different Windows, or a different build of Morph.
    if (line.rfind(kStampPrefix, 0) != 0 || line.substr(kStampPrefix.size()) != stamp_) return;

    while (std::getline(file, line)) {
        if (line.empty() || line.front() == '#') continue;
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (key.empty() || (value != "0" && value != "1")) continue;
        entries_.emplace_back(key, value == "1");
    }
}

std::optional<bool> ProbeCache::Get(std::string_view key) const {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [key](const std::pair<std::string, bool>& entry) {
                                        return entry.first == key;
                                    });
    if (found == entries_.end()) return std::nullopt;
    return found->second;
}

void ProbeCache::Set(std::string_view key, bool value) {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [key](const std::pair<std::string, bool>& entry) {
                                        return entry.first == key;
                                    });
    if (found != entries_.end()) {
        if (found->second == value) return;
        found->second = value;
    } else {
        entries_.emplace_back(std::string(key), value);
    }
    dirty_ = true;
}

void ProbeCache::Flush() {
    if (!dirty_) return;
    dirty_ = false;

    const std::filesystem::path path = CachePath();
    if (path.empty()) return;

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    // Written beside the target and moved into place, so a second worker reading
    // it while this one writes sees the old file or the new one and never half
    // of either. Two workers racing to write is fine: they are writing the same
    // answers.
    const std::filesystem::path temporary =
        path.parent_path() / (L"probes." + std::to_wstring(::GetCurrentProcessId()) + L".tmp");
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!file) return;
        file << kStampPrefix << stamp_ << "\n";
        file << "# What this machine could do when Morph last asked. Delete this file to\n"
                "# make it ask again; it is rewritten whenever the stamp above stops\n"
                "# matching the OS build and the binary that wrote it.\n";

        std::vector<std::pair<std::string, bool>> ordered = entries_;
        std::sort(ordered.begin(), ordered.end());
        for (const auto& [key, value] : ordered) file << key << '=' << (value ? '1' : '0') << "\n";
    }

    if (!::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::filesystem::remove(temporary, ec);
    }
}

void ProbeCache::Clear() {
    entries_.clear();
    dirty_ = false;

    const std::filesystem::path path = CachePath();
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

bool Cached(std::string_view key, const std::function<bool()>& probe) {
    ProbeCache& cache = ProbeCache::Instance();
    if (const std::optional<bool> known = cache.Get(key)) return *known;

    const bool answer = probe ? probe() : false;
    cache.Set(key, answer);
    return answer;
}

}  // namespace morph
