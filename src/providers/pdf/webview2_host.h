// Internal interface to the WebView2 host thread.
//
// Kept separate from pdf.h so that nothing outside this provider sees the
// request shape, and so the provider itself does not have to include windows.h.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "morph/core/result.h"

namespace morph::pdf {

// One unit of work for the host thread. Shared rather than owned by the caller:
// a request that times out is still owned by the host, and the callback that
// eventually arrives must not write into a destroyed object.
struct Request {
    // Exactly one of these is used. A data: or file: URL when useString is
    // false; the document text when it is true.
    std::wstring navigateTo;
    std::wstring html;
    bool useString = true;

    double pageWidthInches = 8.27;  // A4
    double pageHeightInches = 11.69;
    double marginInches = 0.4;
    bool printBackgrounds = true;
    bool landscape = false;

    // Written by the host thread, read by the caller once `finished` is set.
    Result result = Result::Ok();
    std::vector<std::byte> pdf;

    std::mutex mutex;
    std::condition_variable done;
    bool finished = false;
};

// Starts the host thread and creates the environment if that has not happened
// yet. Cheap and idempotent after the first call.
[[nodiscard]] Result StartHost();

// Renders one request. Blocks until the host finishes or the timeout expires.
[[nodiscard]] Result RenderRequest(const std::shared_ptr<Request>& request);

// The installed runtime version, or empty when there is none.
[[nodiscard]] std::string HostVersion();

// Timeout for a single render. A malformed or hostile document must not be able
// to hold the serialised WebView2 slot forever; section 11.3's escalation is the
// outer backstop and this is the inner one.
inline constexpr int kRenderTimeoutMs = 60'000;

}  // namespace morph::pdf
