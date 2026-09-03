// Morph — the scoped temp store (architecture section 5).
//
// "ITempStore& temp;  // scoped, auto-reaped on process exit"
//
// Only an edge that declares requiresDisk should be reaching for this, and at
// the moment exactly one does: the WebView2 PDF hop (section 4.3).
//
// Reaping is belt and braces. The destructor removes the directory, and the
// worker also runs inside a job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
// (section 11.2), so a worker that dies mid-conversion cannot leave scratch
// behind either -- but only if the directory is somewhere the OS will clean.
#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

#include "morph/core/provider.h"

namespace morph {

class ScopedTempStore final : public ITempStore {
public:
    // Creates a uniquely named directory under the system temp path.
    // `label` appears in the directory name so that scratch left behind by a
    // hard kill can be attributed to Morph rather than looking like anyone's.
    explicit ScopedTempStore(std::string_view label = "job");
    ~ScopedTempStore() override;

    ScopedTempStore(const ScopedTempStore&) = delete;
    ScopedTempStore& operator=(const ScopedTempStore&) = delete;

    // The hint supplies the extension only. Some OS components -- WebView2 and
    // Media Foundation among them -- key off it, so a file called "render.html"
    // and one called "render" behave differently.
    [[nodiscard]] std::filesystem::path Acquire(std::string_view hint) override;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    // Removes everything created through Acquire. Called by the destructor;
    // exposed so a long-running worker can reclaim space between jobs.
    void Reap() noexcept;

private:
    std::filesystem::path root_;
    std::mutex mutex_;
    unsigned counter_ = 0;
};

}  // namespace morph
