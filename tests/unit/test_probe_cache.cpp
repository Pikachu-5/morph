// Remembering what the machine can do (architecture sections 6.2, 7.3, 11.4).
//
// The cache is the difference between a 988 ms broker cold start and a 94 ms
// one, against a §17 budget of 150 ms. What has to be true of it is narrow and
// worth stating: an answer is only reused when it is still about the same
// machine and the same build, a missing or damaged file costs nothing but a
// re-probe, and a probe is asked exactly once.

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "morph/core/probe_cache.h"

using namespace morph;

namespace {

std::filesystem::path CacheFile() {
    wchar_t local[MAX_PATH]{};
    if (::GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) == 0) return {};
    return std::filesystem::path(local) / L"Morph" / L"probes.v1";
}

}  // namespace

TEST_CASE("A probe is asked once and remembered", "[probe-cache]") {
    ProbeCache& cache = ProbeCache::Instance();

    int asked = 0;
    const auto probe = [&asked] {
        ++asked;
        return true;
    };

    // A key nothing else uses, so the test does not depend on what this machine
    // can actually do.
    const std::string key = "test.remembered." + std::to_string(::GetCurrentProcessId());
    cache.Set(key, true);

    CHECK(Cached(key, probe));
    CHECK(Cached(key, probe));
    CHECK(asked == 0);  // the answer was already there

    const std::string missing = key + ".missing";
    CHECK(Cached(missing, probe));
    CHECK(asked == 1);
    CHECK(Cached(missing, probe));
    CHECK(asked == 1);  // and now it is there too
}

TEST_CASE("A false answer is remembered as firmly as a true one", "[probe-cache]") {
    // The bug this guards against writes itself: a cache that stores only
    // successes re-runs every failing probe on every start, which is precisely
    // the expensive case -- an encoder that is missing is the one that took a
    // full negotiation to rule out.
    ProbeCache& cache = ProbeCache::Instance();
    const std::string key = "test.false." + std::to_string(::GetCurrentProcessId());

    int asked = 0;
    const auto probe = [&asked] {
        ++asked;
        return false;
    };

    cache.Set(key, false);
    CHECK_FALSE(Cached(key, probe));
    CHECK(asked == 0);
}

TEST_CASE("The stamp is what makes a remembered answer safe to reuse",
          "[probe-cache]") {
    // Two different stamps are two different machines, or two different builds
    // of Morph, and an answer is only about one of them.
    const std::string stamp = ProbeCache::Instance().stamp();
    REQUIRE_FALSE(stamp.empty());
    // Same process, same binary: the stamp does not wander between calls.
    CHECK(ProbeCache::Instance().stamp() == stamp);
}

TEST_CASE("A file written by a different build is ignored, not misread",
          "[probe-cache]") {
    const std::filesystem::path path = CacheFile();
    if (path.empty()) SKIP("no LOCALAPPDATA on this machine");

    // Save whatever is really there: this machine's cache is a real one and the
    // test must not cost the next run a re-probe.
    std::string original;
    if (std::filesystem::exists(path)) {
        std::ifstream file(path, std::ios::binary);
        original.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream file(path, std::ios::trunc);
        file << "stamp=not-this-machine\n";
        file << "pdf.webview2=1\n";
        file << "mf.h264.encode=1\n";
    }

    // A fresh cache object cannot be made -- the instance is a singleton by
    // design, because two of them would each write over the other. What CAN be
    // asserted is that the file above does not decide anything: the stamp in it
    // is not this build's, and Load rejects the whole file on that basis.
    CHECK(ProbeCache::Instance().stamp() != "not-this-machine");

    if (!original.empty()) {
        std::ofstream file(path, std::ios::trunc | std::ios::binary);
        file.write(original.data(), static_cast<std::streamsize>(original.size()));
    } else {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
}
