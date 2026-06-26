#include "broadcast_app_backend.h"

// Generated umbrella: LogosModules (behind modules()) from
// metadata.json#dependencies — typed wrappers + typed event accessors.
// (No dependencies here, but include it once you add some.)
// #include "logos_sdk.h"

int BroadcastAppBackend::add(int a, int b) {
  int result = a + b;
  // PROP from the .rep — QtRO pushes every setStatus to the QML replica.
  setStatus(QStringLiteral("%1 + %2 = %3").arg(a).arg(b).arg(result));
  return result;
}

void BroadcastAppBackend::onContextReady() {
  // Tick once per second while the backend is alive, incrementing the count
  // and pushing it to every QML replica as a synced PROP. The timer only runs
  // as long as this backend (and its event loop) is active.
  QObject::connect(&m_tickTimer, &QTimer::timeout, [this]() {
    setBackendElapsedSeconds(++m_elapsedSeconds);
  });
  m_tickTimer.start(1000);
}
