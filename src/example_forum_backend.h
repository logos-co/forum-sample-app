#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

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
 * Posts are signed under one of this install's accounts: loadAccounts() opens
 * (or, on first run, mints) a set of keystore_signer keys on bootstrap and
 * selects one, and publish() signs every outgoing message with the selected
 * key before sending. The view can create, switch, rename and delete accounts
 * through the .rep slots. See loadAccounts()'s doc comment for the on-disk
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

  // .rep SLOTs — account management. Each returns "" on success or an error
  // description, and routes every state change through publishAccountState()
  // so myAddress / myLabel / accountsJson can never drift apart.
  //
  // createAccount mints a key under m_credential and selects it (an empty
  // label becomes "Account N"); selectAccount switches which key signs from
  // here on; renameAccount touches only app-side metadata; deleteAccount
  // destroys the key irreversibly, refuses the last remaining account, and
  // re-selects another if it removed the selected one.
  QString createAccount(QString label) override;
  QString selectAccount(QString keyId) override;
  QString renameAccount(QString keyId, QString label) override;
  QString deleteAccount(QString keyId) override;

protected:
  // The backend's "start": fired once after the context is wired (so modules()
  // is live). Schedules bootstrap() off the return path.
  void onContextReady() override;

private:
  // Wires delivery_module events, then createNode + subscribe + start.
  // Deferred off onContextReady() because node creation is synchronous and can
  // block briefly — returning promptly lets the QML replica reach Valid sooner.
  void bootstrap();

  // Subscribe to kTopic, flip nodeReady, and kick off the store backfill.
  // Retries itself on failure (up to kMaxSubscribeAttempts) — a subscribe that
  // fails once used to leave the app permanently unable to receive, with
  // composing disabled and no way back short of a restart.
  //
  // Called before start(), and again from nodeStarted if that first attempt
  // didn't take. Idempotent: several paths into it can legitimately land.
  void subscribeToForum();

  // Re-derive the status PROP from the last connectionStateChanged value. A
  // no-op until subscribeToForum() succeeds, since bootstrap's own progress
  // messages own the status until then.
  void refreshStatus();

  // Pull recent history for kTopic from a store service peer and replay it
  // through the normal decode/emit path, so a late joiner (or a node that spent
  // time with no mesh peers) doesn't see an empty forum. No-op unless
  // EXAMPLE_FORUM_STORE_PEER names a peer to ask.
  void backfillFromStore();

  // Resolve one of our own sends: map a delivery_module requestId back to the
  // ForumMessage id it was for and report `state` ("propagated" / "sent" /
  // "failed") to the view. Ignores request ids we don't know — delivery_module
  // is shared, so other apps' sends surface here too.
  void settleSend(const QString &requestId, const QString &state,
                  const QString &detail);

  // Opens (or, on first run, mints) this install's accounts and selects one.
  // Fills m_credential / m_accounts / m_keyId and publishes the account PROPs.
  //
  // Layout under identityDir() (an app-private dir this plugin picks itself —
  // ui_qml plugins get no host-provisioned instancePersistencePath, unlike
  // core modules; keystore-signer-module's own key storage lives in *its*
  // instancePersistencePath, not here):
  //   keystore_signer_credential — 256-bit bearer secret, generated on first run, hex on disk
  //   accounts.json              — { version, selected, accounts[{keyId,label,createdAt}] }
  //   key_id                     — legacy single-identity file, read once to
  //                                migrate a pre-accounts install, then ignored
  //
  // Every account is a key under the *one* credential, so the keystore itself
  // enumerates them: listKeys() is the source of truth for which accounts
  // exist, and accounts.json only adds the labels and the selection. That
  // makes reconciliation the load path's main job — accounts.json can name
  // keys the keystore has never seen (a Basecamp --user-dir whose module_data
  // is new), and the keystore can hold keys accounts.json has lost. Dropping
  // the former beats keeping identities whose every publish() would fail to
  // sign; adopting the latter beats minting duplicates beside them. Losing an
  // account label is recoverable; losing the key is not, so reconciliation
  // never deletes keys.
  //
  // The credential is not a user secret — there is no login — it only exists
  // because keystore-signer-module partitions key namespaces by caller secret.
  // Storing it next to the keystore it isolates protects against nothing; it's
  // structural plumbing for per-caller isolation, not a security boundary.
  // Holding several accounts under it does not change that: they share one
  // namespace and one plaintext credential, so they are separate *identities*,
  // not separate security domains. A real secret-at-rest story (OS keychain,
  // user passphrase, etc.) is a follow-up if this ever needs to resist a local
  // attacker.
  void loadAccounts();

  // Read-or-mint the bearer credential (m_credential). Returns false if it
  // could not be established, in which case no account work can proceed.
  bool ensureCredential();

  // Mint a fresh keystore_signer key and append it to m_accounts as `label`.
  // Returns "" on success (with *outKeyId set), else an error description.
  QString mintAccount(const QString &label, QString *outKeyId);

  // Persist m_accounts + the selection to accounts.json. Returns false on a
  // write failure (the in-memory state stands, so the session keeps working).
  bool saveAccounts();

  // Set m_keyId from the selection and push myAddress / myLabel / accountsJson
  // to the view. The single point that writes those three PROPs.
  void publishAccountState();

  // Index of `keyId` in m_accounts, or -1. Accounts are few, so linear is fine.
  int indexOfAccount(const QString &keyId) const;

  // An unused "Account N" label, N counting up from m_accounts.size() + 1.
  QString defaultAccountLabel() const;

  // This app's private data directory (not shared with other Logos modules),
  // scoped to the Basecamp data tree (LOGOS_USER_DIR) when there is one so the
  // accounts track the keystore_signer instance that holds their keys, and
  // suffixed by EXAMPLE_FORUM_INSTANCE when set so two standalone instances on
  // one machine don't share one identity.
  QString identityDir() const;

  // Encode `msg`, send it on kTopic, then locally echo it (the relay does not
  // loop our own messages back). Returns "" on success, or an error string.
  // Signs `msg` (setting its author/sig fields) before encoding with the
  // selected account; fails if no account is ready yet (see loadAccounts()).
  QString publish(ForumMessage msg);

  // Fan a decoded message out to the matching .rep signal (topicReceived for a
  // topic, replyReceived for a reply). `timestamp` is ns since the Unix epoch.
  void emitForumMessage(const ForumMessage &msg, qint64 timestamp);

  // The single LIP-23 content topic this forum lives on, so every instance of
  // the app shares one forum.
  static const QString kTopic;

  // One signing identity: a keystore_signer key id plus the app-side metadata
  // the keystore doesn't hold. `label` is a display name the user can change;
  // `createdAt` is ms since the Unix epoch, kept so adopted keys of unknown age
  // still sort stably.
  struct Account {
    QString keyId;
    QString label;
    qint64 createdAt = 0;
  };

  // Every account this install holds, oldest first. Empty until loadAccounts()
  // completes (and, if the keystore is unreachable, after it too).
  QVector<Account> m_accounts;

  // The selected account's key id (hex-encoded) — the key publish() signs with.
  // Empty until an account is ready; publish() gates on this the same way it
  // gates on nodeReady(). Always equal to m_accounts[i].keyId for some i, or
  // empty; publishAccountState() is what maintains that. Mirrored to the view
  // as the myAddress PROP.
  QString m_keyId;

  // True once subscribe(kTopic) has succeeded. Guards the several paths into
  // subscribeToForum() against each other, and gates refreshStatus().
  bool m_subscribed = false;

  // How many times subscribeToForum() has asked delivery_module to subscribe.
  // Bounds the retry so a genuinely broken node doesn't retry forever.
  int m_subscribeAttempts = 0;

  // True once nodeStarted has confirmed the node is up, or once we've found a
  // node already running. The store backfill needs a live node, and the
  // subscribe that precedes it now happens before start() — so this is what
  // keeps the query from going out against a node that can't serve it.
  bool m_nodeStarted = false;

  // True once the store backfill has run. It is a one-shot catch-up, not
  // something to repeat every time the node reports itself started.
  bool m_backfilled = false;

  // The last connectionStateChanged value (Connected / PartiallyConnected /
  // Disconnected), or empty before the first one arrives. The only honest
  // account of connectivity this app gets — a successful local subscribe says
  // nothing about whether any peer will ever relay to us.
  QString m_connectionState;

  // Our in-flight sends: delivery_module requestId -> ForumMessage id. Entries
  // live from the send until the network validates or rejects the message
  // (settleSend()); "propagated" is a waypoint and keeps the entry.
  QHash<QString, QString> m_pendingSends;

  // This install's keystore-signer-module credential (256-bit bearer secret),
  // kept in memory so publish() can pass it to the signing call directly.
  // Enables per-caller key isolation in keystore-signer-module; every account
  // is a key within this one credential's namespace.
  QByteArray m_credential;
};
