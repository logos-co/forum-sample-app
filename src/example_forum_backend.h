#pragma once

#include <QString>

#include "forum_message.h"
#include "logos_ui_plugin_context.h"
#include "rep_example_forum_source.h"

/**
 * @brief UI backend for Example Forum (universal authoring model).
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
 *   - `ExampleForumSimpleSource` — generated from example_forum.rep; implement
 *     its slots and feed its PROPs (e.g. `setStatus(...)`), which auto-sync to
 *     every QML replica.
 *   - `LogosUiPluginContext` — gives `onContextReady()` plus `modules()`, the
 *     Qt-typed caller and event subscriptions for the `delivery_module` and
 *     `keystore_signer` dependencies declared in metadata.json.
 *
 * Posts are signed under a per-install identity: ensureIdentity() creates (or
 * reopens) a keystore_signer key under this app's local data dir on first
 * bootstrap, and publish() signs every outgoing message with it before
 * sending. See ensureIdentity()'s doc comment for the on-disk layout and its
 * known limitations.
 *
 * The C++ backend runs in its own isolated `ui-host` process; lifecycle hooks
 * and delivery events log to `std::cerr`, visible in the host's stderr stream.
 */
class ExampleForumBackend : public ExampleForumSimpleSource,
                            public LogosUiPluginContext {
public:
  ExampleForumBackend();
  ~ExampleForumBackend() override;

  // .rep SLOTs — broadcast a new forum topic / a reply on the shared topic.
  // Each returns an empty string on success, or an error description.
  QString createTopic(QString title, QString body) override;
  QString replyToTopic(QString topicId, QString body) override;

  // Locally restore a placeholder topic from its (hash-verified) title. See the
  // .rep contract. Returns "" on success, or an error description.
  QString reconstructTopic(QString topicId, QString title) override;

protected:
  // The backend's "start": fired once after the context is wired (so modules()
  // is live). Schedules bootstrap() off the return path.
  void onContextReady() override;

private:
  // Wires delivery_module events, then createNode + start + subscribe(kTopic).
  // Deferred off onContextReady() because node creation is synchronous and can
  // block briefly — returning promptly lets the QML replica reach Valid sooner.
  void bootstrap();

  // Opens (or, on first run, creates) this install's keystore-signer-module identity.
  // Sets m_keyId / m_credential / myAddress PROP on success.
  //
  // Layout under identityDir() (an app-private dir this plugin picks itself —
  // ui_qml plugins get no host-provisioned instancePersistencePath, unlike
  // core modules; keystore-signer-module's own key storage lives in *its*
  // instancePersistencePath, not here):
  //   keystore_signer_credential     — 256-bit bearer secret, generated on first run, hex on disk
  //   key_id                         — hex-encoded key identifier, persisted after key creation
  //
  // Both files are only meaningful against the keystore they were minted
  // against, so a reload is validated with keystore_signer.publicKey() and a
  // fresh identity is minted if that keystore no longer holds the key (e.g.
  // running against a Basecamp --user-dir whose module_data is new). Losing
  // the identity means posting under a new author id — recoverable — whereas
  // keeping a stale one makes every publish() fail to sign.
  //
  // The credential is not a user secret — there is no login — it only exists
  // because keystore-signer-module partitions key namespaces by caller secret.
  // Storing it next to the keystore it isolates protects against nothing; it's
  // structural plumbing for per-caller isolation, not a security boundary. A real
  // secret-at-rest story (OS keychain, user passphrase, etc.) is a follow-up if
  // this identity ever needs to resist a local attacker.
  //
  // keystore-signer-module automatically isolates each caller's keys by their
  // credential, so different consumers in the same host process get completely
  // separate key namespaces. No singleton-lock problems unlike accounts_module.
  // publish() signs directly with the credential and key_id without re-init
  // workarounds.
  void ensureIdentity();

  // This app's private data directory (not shared with other Logos modules),
  // scoped to the Basecamp data tree (LOGOS_USER_DIR) when there is one so the
  // identity tracks the keystore_signer instance that holds its key.
  QString identityDir() const;

  // Encode `msg`, send it on kTopic, then locally echo it (the relay does not
  // loop our own messages back). Returns "" on success, or an error string.
  // Signs `msg` (setting its author/sig fields) before encoding; fails if the
  // identity isn't ready yet (see ensureIdentity()).
  QString publish(ForumMessage msg);

  // Fan a decoded message out to the matching .rep signal (topicReceived for a
  // topic, replyReceived for a reply). `timestamp` is ns since the Unix epoch.
  void emitForumMessage(const ForumMessage &msg, qint64 timestamp);

  // The single LIP-23 content topic this forum lives on, so every instance of
  // the app shares one forum.
  static const QString kTopic;

  // This install's signing key ID (hex-encoded), set once ensureIdentity() completes.
  // Empty until then — publish() gates on this the same way it gates on
  // nodeReady(). Displayed to the user via myAddress PROP.
  QString m_keyId;

  // This install's keystore-signer-module credential (256-bit bearer secret),
  // kept in memory so publish() can pass it to the signing call directly.
  // Enables per-caller key isolation in keystore-signer-module.
  QByteArray m_credential;
};
