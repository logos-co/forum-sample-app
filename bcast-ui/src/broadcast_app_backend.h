#pragma once

#include <QTimer>

#include "logos_ui_plugin_context.h"
#include "rep_broadcast_app_source.h"

/**
 * @brief The hand-written UI backend (universal authoring model).
 *
 * You write only this class and the `.rep` view contract. The `*Plugin` and
 * `*Interface` classes — `Q_PLUGIN_METADATA`, `initLogos` wiring, QtRO
 * registration — are generated around it.
 *
 * It derives:
 *   - `BroadcastAppSimpleSource` — generated from broadcast_app.rep; implement
 * its slots and feed its PROPs (e.g. `setStatus(...)`), which auto-sync to
 *     every QML replica.
 *   - `LogosUiPluginContext` — gives `onContextReady()` plus `modules()`, the
 *     Qt-typed callers and event subscriptions for any `dependencies` you
 *     declare (none here; see the typed-backend doc-test for a worked example).
 *     A UI plugin is a view, not a module, so that is all the context carries.
 */
class BroadcastAppBackend : public BroadcastAppSimpleSource,
                            public LogosUiPluginContext {
public:
  int add(int a, int b) override;

protected:
  // The backend's "start": fired once after the context is wired (see
  // logos_module_context.h). Starts the per-second tick.
  void onContextReady() override;

private:
  // Ticks once per second while the backend is alive, pushing the running
  // count to every QML replica via the backendElapsedSeconds PROP.
  QTimer m_tickTimer;
  int m_elapsedSeconds = 0;
};
