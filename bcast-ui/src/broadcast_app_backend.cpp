#include "broadcast_app_backend.h"

#include <chrono>

// Generated umbrella: LogosModules (behind modules()) from
// metadata.json#dependencies — typed wrappers + typed event accessors.
// (No dependencies here, but include it once you add some.)
// #include "logos_sdk.h"

int BroadcastAppBackend::add(int a, int b)
{
    int result = a + b;
    // PROP from the .rep — QtRO pushes every setStatus to the QML replica.
    setStatus(QStringLiteral("%1 + %2 = %3").arg(a).arg(b).arg(result));
    return result;
}

void BroadcastAppBackend::onContextReady()
{
    // Wall-clock (Unix-epoch ms) so the QML replica — running in the same
    // ui-host process — can subtract it from JS Date.now(). std::chrono only;
    // no Qt time types. Published once as a synced PROP, so QML does the
    // per-frame counting with zero IPC per tick.
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    setBackendStartedAtMs(static_cast<double>(nowMs));
}
