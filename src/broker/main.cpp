// morph.broker.exe — the process that survives (architecture sections 3.1, 10.3).
//
// It is started on demand by whichever client fails to connect, holds the graph
// and the queue, and exits sixty seconds after the last client leaves. There is
// no window and no console output in normal operation: a broker that talks is a
// broker someone has to look at.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <windows.h>

#include "broker.h"

namespace {

morph::broker::Broker* gBroker = nullptr;

// Ctrl+C in a developer session, and the shutdown notification a packaged
// install gets. Both mean the same thing: stop accepting, let the pool wind
// down, exit. Killing the process outright would leave the workers to the job
// objects, which handles it but loses the temp directories.
BOOL WINAPI OnConsoleEvent(DWORD /*type*/) {
    if (gBroker != nullptr) gBroker->Stop();
    return TRUE;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool verbose = false;
    int idleSeconds = 60;
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument(argv[i]);
        if (argument == L"--verbose") {
            verbose = true;
        } else if (argument == L"--idle-seconds" && i + 1 < argc) {
            idleSeconds = std::max(1, _wtoi(argv[++i]));
        }
    }

    morph::broker::Broker broker;
    broker.SetIdleTimeout(std::chrono::seconds(idleSeconds));
    gBroker = &broker;
    ::SetConsoleCtrlHandler(OnConsoleEvent, TRUE);

    if (const morph::Result started = broker.Start(); !started) {
        // "A broker is already running" is the expected outcome of two clients
        // racing to start one, and is not a failure of anything. Exit quietly
        // with a distinct code so a client can tell it apart from a real error.
        if (started.status() == morph::Status::kResourceLimit) return 2;

        std::fwrite(started.message().data(), 1, started.message().size(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }

    if (verbose) {
        std::fprintf(stderr, "morph.broker listening; worker at %ls\n",
                     morph::broker::WorkerExecutablePath().c_str());
        std::fprintf(stderr, "%s\n", broker.Describe().c_str());
    }

    broker.Run();
    gBroker = nullptr;
    return 0;
}
