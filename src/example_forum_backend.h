#pragma once

#include <memory>

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include "cloud_data_core/engine.h"
#include "delivery_module_transport.h"
#include "forum_message.h"
#include "logos_ui_plugin_context.h"
#include "rep_example_forum_source.h"
#include "storage_module_blob_store.h"

/**
 * @brief UI backend for Example Forum (universal authoring model).
 *
 * Posts live in a local-first CRDT store: `cloud_data_core::CloudDataEngine`
 * (vendored under lib/, from cloud-data-module) owns an embedded SQLite
 * database, merges concurrent writes, and syncs over `delivery_module`.
 * This backend supplies the two adapters that engine needs
 * (`DeliveryModuleTransport`, `StorageModuleBlobStore`) and translates between
 * its documents and the view's signals:
 *
 *   - `createTopic` / `replyToTopic` sign the post, then `put()` it into the
 *     `kTopicsCollection` / `kRepliesCollection` collection. The write lands
 *     locally first and broadcasts second, so a post survives a restart even
 *     if it never reaches the network.
 *   - every materialized change — a local `put`, a merged remote op, or a row
 *     replayed from disk at startup — arrives on one path
 *     (`handleDocumentChanged`) and fans out to the `topicReceived` /
 *     `replyReceived` signals the QML view threads into a forum.
 *   - `loadBacklog()` hands the view everything the store already holds, which
 *     is what makes the forum survive open/close. The view pulls it once its
 *     replica is up, because a signal emitted during bootstrap would reach no
 *     replica at all (see the .rep).
 *
 * The engine is what persists; `storage_module` only backs its optional
 * snapshot bridge, which stays inert unless the user has a storage node up.
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

  // Every post the local store already holds, as the JSON array documented in
  // the .rep. Pulled by the view once its replica is up — see the .rep's
  // doc comment for why this is a slot and not a burst of startup signals.
  QString loadBacklog() override;

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

  // Subscribe to kTopic and flip nodeReady. Retries itself on failure (up to
  // kMaxSubscribeAttempts) — a subscribe that fails once used to leave the app
  // permanently unable to receive, with composing disabled and no way back
  // short of a restart.
  //
  // Called before start(), and again from nodeStarted if that first attempt
  // didn't take. Idempotent: several paths into it can legitimately land.
  void subscribeToForum();

  // Re-derive the status PROP from the last connectionStateChanged value. A
  // no-op until subscribeToForum() succeeds, since bootstrap's own progress
  // messages own the status until then.
  void refreshStatus();

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
  // accounts track the keystore_signer instance that holds their keys.
  QString identityDir() const;

  // Sign `msg` with the selected account (setting its author/sig fields) and
  // put() it into its collection. Returns "" on success, or an error string;
  // fails if no account is ready yet (see loadAccounts()). No local echo is
  // needed any more — put() reports the write back through
  // handleDocumentChanged() before it returns.
  QString publish(ForumMessage msg);

  // Build the adapters and the engine, open the local store, and replay what
  // it already holds. Returns false if the store could not be opened, in which
  // case composing stays disabled. Called once from bootstrap().
  bool openEngine();

  // The engine's single report of a materialized document change, whatever
  // caused it (local put, merged remote op, or startup replay). Decodes the
  // document and fans it out to topicReceived / replyReceived.
  void handleDocumentChanged(const std::string &collectionId,
                             const std::string &docId, const std::string &json);

  // Record which of our posts a delivery request id belongs to, by decoding
  // the op the transport just published. Feeds settleSend().
  void notePublished(const std::string &requestId,
                     const std::vector<uint8_t> &payload);

  // Fan a decoded message out to the matching .rep signal (topicReceived for a
  // topic, replyReceived for a reply). `timestamp` is ns since the Unix epoch.
  void emitForumMessage(const ForumMessage &msg, qint64 timestamp);

  // LIP-23 content-topic app segment, and the two collections every post
  // belongs to. The engine derives the actual topics from these
  // (`/example-forum/1/<collection>/proto`, unbucketed — see kBucketBytes),
  // so every instance of the app shares one forum.
  static const char kAppName[];
  static const char kTopicsCollection[];
  static const char kRepliesCollection[];

  // One content topic per collection rather than the engine's default 256
  // hash buckets: a forum wants every post in a collection, including ones
  // from peers it has never heard of, which a per-doc bucket cannot deliver.
  // See cloud_data_core::sync_engine::contentTopicForDoc.
  static constexpr int kBucketBytes = 0;

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

  // The last connectionStateChanged value (Connected / PartiallyConnected /
  // Disconnected), or empty before the first one arrives. The only honest
  // account of connectivity this app gets — a successful local subscribe says
  // nothing about whether any peer will ever relay to us.
  QString m_connectionState;

  // Our in-flight sends: delivery_module requestId -> ForumMessage id. Entries
  // live from the send until the network validates or rejects the message
  // (settleSend()); "propagated" is a waypoint and keeps the entry.
  QHash<QString, QString> m_pendingSends;

  // The local-first store and its two module adapters. Declared in this order
  // so the engine (which holds references to both) is destroyed first.
  std::unique_ptr<DeliveryModuleTransport> m_transport;
  std::unique_ptr<StorageModuleBlobStore> m_blobStore;
  std::unique_ptr<cloud_data_core::CloudDataEngine> m_engine;

  // This install's keystore-signer-module credential (256-bit bearer secret),
  // kept in memory so publish() can pass it to the signing call directly.
  // Enables per-caller key isolation in keystore-signer-module; every account
  // is a key within this one credential's namespace.
  QByteArray m_credential;
};
