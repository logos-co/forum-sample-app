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
 *     `accounts_module` dependencies declared in metadata.json.
 *
 * Posts are signed under a per-install identity: ensureIdentity() creates (or
 * reopens) an accounts_module keystore under this app's local data dir on
 * first bootstrap, and publish() signs every outgoing message with it before
 * sending. See ensureIdentity()'s doc comment for the keystore/passphrase
 * layout and its known limitations.
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

  // Opens (or, on first run, creates) this install's accounts_module keystore.
  // Sets m_myAddress / m_passphrase / myAddress PROP on success.
  //
  // Layout under identityDir() (an app-private dir this plugin picks itself —
  // ui_qml plugins get no host-provisioned instancePersistencePath, unlike
  // core modules):
  //   keystore/    — accounts_module's encrypted keystore (owned by it)
  //   passphrase   — random, generated on first run, plaintext on disk
  //   address      — cached copy of the account address NewAccount returned
  //
  // The passphrase is not a user secret — there is no login — it only exists
  // because accounts_module's keystore API requires one to encrypt-at-rest.
  // Storing it next to the keystore it unlocks protects against nothing; it's
  // structural plumbing, not a security boundary. A real secret-at-rest story
  // (OS keychain, user passphrase, etc.) is a follow-up if this identity ever
  // needs to resist a local attacker.
  //
  // Does NOT unlock the account — accounts_module holds exactly one keystore
  // handle for its whole process, shared by every app that depends on it
  // (same as delivery_module). Any consumer's initKeystore() call — including
  // ours, on a second bootstrap — closes whatever was previously open and
  // replaces it (see GoWSK_accounts_keystore_CloseKeyStore in
  // accounts_module_impl.cpp's initKeystore). An unlock from bootstrap time
  // has no durable guarantee: another consumer opening its own directory
  // in between silently locks ours again. publish() works around this by
  // reopening our directory and signing with the passphrase directly, back
  // to back, instead of relying on unlock state surviving between calls.
  void ensureIdentity();

  // This app's private data directory (not shared with other Logos modules).
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

  // This install's signing address, set once ensureIdentity() completes.
  // Empty until then — publish() gates on this the same way it gates on
  // nodeReady().
  QString m_myAddress;

  // This install's keystore passphrase, kept in memory so publish() can pass
  // it to keystoreSignHashWithPassphrase() directly rather than depending on
  // a prior keystoreUnlock() surviving (see ensureIdentity()'s doc comment).
  QString m_passphrase;
};
