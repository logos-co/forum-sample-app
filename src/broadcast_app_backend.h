#pragma once

#include <QString>

#include "forum_message.h"
#include "logos_ui_plugin_context.h"
#include "rep_broadcast_app_source.h"

/**
 * @brief UI backend for the Broadcast Forum (universal authoring model).
 *
 * A single forum runs over one hard-coded delivery content topic (`kTopic`).
 * Every post — a topic creation or a reply — is a JSON `ForumMessage` envelope
 * (see forum_message.h) published with `delivery_module.send` and received via
 * its `messageReceived` event:
 *
 *   - `createTopic` / `replyToTopic` encode an envelope, broadcast it, and
 *     locally echo it (the relay doesn't loop our own messages back).
 *   - inbound payloads are decoded and fanned out to the `topicReceived` /
 *     `replyReceived` signals, which the QML view threads into a forum.
 *
 * You write only this class and the `.rep` view contract. The `*Plugin` /
 * `*Interface` classes, `Q_PLUGIN_METADATA`, `initLogos` and QtRO registration
 * are generated around it.
 *
 * It derives:
 *   - `BroadcastAppSimpleSource` — generated from broadcast_app.rep; implement
 *     its slots and feed its PROPs (e.g. `setStatus(...)`), which auto-sync to
 *     every QML replica.
 *   - `LogosUiPluginContext` — gives `onContextReady()` plus `modules()`, the
 *     Qt-typed caller and event subscriptions for the `delivery_module`
 *     dependency declared in metadata.json.
 *
 * The C++ backend runs in its own isolated `ui-host` process; lifecycle hooks
 * and delivery events log to `std::cerr`, visible in the host's stderr stream.
 */
class BroadcastAppBackend : public BroadcastAppSimpleSource,
                            public LogosUiPluginContext {
public:
  BroadcastAppBackend();
  ~BroadcastAppBackend() override;

  // .rep SLOTs — broadcast a new forum topic / a reply on the shared topic.
  // Each returns an empty string on success, or an error description.
  QString createTopic(QString title, QString body) override;
  QString replyToTopic(QString topicId, QString body) override;

protected:
  // The backend's "start": fired once after the context is wired (so modules()
  // is live). Schedules bootstrap() off the return path.
  void onContextReady() override;

private:
  // Wires delivery_module events, then createNode + start + subscribe(kTopic).
  // Deferred off onContextReady() because node creation is synchronous and can
  // block briefly — returning promptly lets the QML replica reach Valid sooner.
  void bootstrap();

  // Encode `msg`, send it on kTopic, then locally echo it (the relay does not
  // loop our own messages back). Returns "" on success, or an error string.
  QString publish(const ForumMessage &msg);

  // Fan a decoded message out to the matching .rep signal (topicReceived for a
  // topic, replyReceived for a reply). `timestamp` is ns since the Unix epoch.
  void emitForumMessage(const ForumMessage &msg, qint64 timestamp);

  // The single LIP-23 content topic this forum lives on, so every instance of
  // the app shares one forum.
  static const QString kTopic;
};
