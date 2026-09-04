// Morph — remembering what the machine can do (architecture sections 6.2, 7.3
// and 11.4).
//
// Three providers ask expensive questions at startup, and they have to: section
// 6.2 and section 7.3 both require an edge to be absent rather than failing
// mid-job, and the only reliable way to know whether an encoder works is to ask
// it to encode something. On this machine that costs about 965 ms -- a WebView2
// detection, three trial image encodes, and five media type negotiations -- out
// of a broker cold start that section 17 budgets at 150 ms in total.
//
// So the answers are stored. This is the same idea section 11.4 applies to cost
// estimates: "calibrated at first run with a short synthetic benchmark and
// stored, so the numbers get better on the machine they actually run on".
//
// WHAT MAKES IT SAFE TO CACHE is the stamp. A codec package can be installed or
// removed at any time, so an answer is only reused when the OS build, the
// binary that asked, and its build time all match what was recorded. Anything
// else re-probes -- including a Morph that was rebuilt, which is what makes this
// invisible during development.
//
// A cache that cannot be read or written is not an error anywhere: the probes
// run, the process is slower, and nothing else changes.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace morph {

class ProbeCache {
public:
    // Loaded from disk on first use, per process.
    [[nodiscard]] static ProbeCache& Instance();

    // The remembered answer, or nullopt when there is none for this stamp.
    [[nodiscard]] std::optional<bool> Get(std::string_view key) const;

    void Set(std::string_view key, bool value);

    // Writes the file if anything changed. Called once, after startup probing;
    // a process that never calls it simply re-probes next time.
    void Flush();

    // Forgets everything, on disk and in memory. For the tests, and for a
    // future "Morph is behaving as though a codec is missing" support answer.
    void Clear();

    [[nodiscard]] const std::string& stamp() const noexcept { return stamp_; }

private:
    ProbeCache();

    void Load();

    std::string stamp_;
    // Small enough that a vector of pairs beats a map, and ordered output makes
    // the file readable by a person diagnosing exactly this.
    std::vector<std::pair<std::string, bool>> entries_;
    bool dirty_ = false;
    bool loaded_ = false;
};

// The answer to `key`, asking `probe` only when it is not already known.
//
// This is the whole interface most callers want:
//
//     const bool present = Cached("wic.heif.encode", [] { return TrialEncode(); });
[[nodiscard]] bool Cached(std::string_view key, const std::function<bool()>& probe);

}  // namespace morph
