#pragma once

#include <QString>

#include "logos_ui_plugin_context.h"
#include "rep_broadcast_app_source.h"

/**
 * @brief UI backend for the Broadcast App (universal authoring model).
 *
 * Demonstrates consuming the `delivery_module` Logos module (declared as a
 * `dependencies` entry in metadata.json and a flake input) from a UI plugin:
 *
 *   - On startup it bootstraps a delivery node and **subscribes to a single
 *     hard-coded content topic** (`kTopic`), forwarding every message that
 *     arrives to the QML view as the `messageReceived` signal.
 *   - The QML view sends **plain text** to that same topic via the
 *     `sendMessage` slot (encoded UTF-8 → `delivery_module.send`).
 *
 * You write only this class and the `.rep` view contract. The `*Plugin` and
 * `*Interface` classes — `Q_PLUGIN_METADATA`, `initLogos` wiring, QtRO
 * registration — are generated around it.
 *
 * It derives:
 *   - `BroadcastAppSimpleSource` — generated from broadcast_app.rep; implement
 *     its slots and feed its PROPs (e.g. `setStatus(...)`), which auto-sync to
 *     every QML replica.
 *   - `LogosUiPluginContext` — gives `onContextReady()` plus `modules()`, the
 *     Qt-typed callers and event subscriptions for the `delivery_module`
 *     dependency declared in metadata.json.
 *
 * The C++ backend runs in its own isolated `ui-host` process; each lifecycle
 * hook and delivery event logs to `std::cerr` so its activity is visible in the
 * host's stderr stream.
 */
class BroadcastAppBackend : public BroadcastAppSimpleSource,
                            public LogosUiPluginContext {
public:
  BroadcastAppBackend();
  ~BroadcastAppBackend() override;

  // .rep SLOT — send `text` to the hard-coded topic as UTF-8 bytes via
  // delivery_module. Returns an empty string on success, or an error string.
  QString sendMessage(QString text) override;

protected:
  // The backend's "start": fired once after the context is wired (so modules()
  // is live). Schedules bootstrap() off the return path.
  void onContextReady() override;

private:
  // Wires delivery_module events, then createNode + start + subscribe(kTopic).
  // Deferred off onContextReady() because node creation is synchronous and can
  // block briefly — returning promptly lets the QML replica reach Valid sooner.
  void bootstrap();

  // The single LIP-23 content topic every instance of this app uses, so any
  // two instances on the network broadcast to one another.
  static const QString kTopic;
};
