#include "broadcast_app_backend.h"

#include <iostream>

// Generated umbrella: LogosModules (behind modules()) from
// metadata.json#dependencies — typed wrappers + typed event accessors.
// (No dependencies here, but include it once you add some.)
// #include "logos_sdk.h"

namespace {
// One consistently-tagged line per lifecycle hook / callback so the backend's
// activity is easy to spot (and grep) in the host's stderr stream.
void logEvent(const std::string &what) {
  std::cerr << "[broadcast_app backend] " << what << std::endl;
}
} // namespace

BroadcastAppBackend::BroadcastAppBackend() {
  // Runs in the ui-host process before the context is wired.
  logEvent("ctor — backend constructed (context not yet wired)");
}

BroadcastAppBackend::~BroadcastAppBackend() {
  logEvent("dtor — backend destroyed");
}

int BroadcastAppBackend::add(int a, int b) {
  int result = a + b;
  logEvent("add(" + std::to_string(a) + ", " + std::to_string(b) +
           ") = " + std::to_string(result));
  // PROP from the .rep — QtRO pushes every setStatus to the QML replica.
  setStatus(QStringLiteral("%1 + %2 = %3").arg(a).arg(b).arg(result));
  return result;
}

void BroadcastAppBackend::onContextReady() {
  logEvent("onContextReady — context wired, starting tick timer");

  // Tick once per second while the backend is alive, incrementing the count
  // and pushing it to every QML replica as a synced PROP. The timer only runs
  // as long as this backend (and its event loop) is active.
  QObject::connect(&m_tickTimer, &QTimer::timeout, this,
                   &BroadcastAppBackend::onTick);
  m_tickTimer.start(1000);
}

void BroadcastAppBackend::onTick() {
  setBackendElapsedSeconds(++m_elapsedSeconds);
  logEvent("onTick — backendElapsedSeconds=" + std::to_string(m_elapsedSeconds));
}
