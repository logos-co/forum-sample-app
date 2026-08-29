#include "example_forum_backend.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLatin1String>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QUuid>
#include <QVariantList>

// Generated umbrella: LogosModules (behind modules()) built from
// metadata.json#dependencies — the typed `delivery_module` and `keystore_signer`
// wrappers and their typed event accessors. logos_types.h provides LogosResult
// (delivery_module's return type); logos_call_error.h provides logos::CallError,
// the out-param error keystore_signer's synchronous, direct-return calls use
// instead (it has no native LogosResult wrapping).
#include "logos_call_error.h"
#include "logos_sdk.h"
#include "logos_types.h"

// The vendored local-first engine (lib/, from cloud-data-module) and the two
// Qt-typed adapters that bind it to this app's delivery_module/storage_module
// dependencies.
#include "cloud_data_core/sync_engine.h"
#include "delivery_module_transport.h"
#include "storage_module_blob_store.h"

// Injected by CMake from metadata.json#version. Guard so the file still compiles
// (as an "unknown" version) if the definition is ever missing.
#ifndef EXAMPLE_FORUM_VERSION
#define EXAMPLE_FORUM_VERSION "unknown"
#endif

namespace {
// One consistently-tagged line per lifecycle hook / delivery event so the
// backend's activity is easy to spot (and grep) in the host's stderr stream.
void logEvent(const std::string &what) {
  std::cerr << "[example_forum backend] " << what << std::endl;
}

// A fresh, collision-free message id (also the topic id, for topics).
QString newId() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// Length of the keystore_signer bearer credential, in bytes: 256 bits, the
// minimum secret length keystore_signer accepts.
constexpr int kCredentialBytes = 32;

// Local-echo timestamp in the same units delivery_module reports for received
// messages: nanoseconds since the Unix epoch.
qint64 nowNs() {
  return QDateTime::currentMSecsSinceEpoch() * 1000000LL;
}

// Subscribe retry: how long to wait before asking again, and how many attempts
// before giving up and leaving the failure on screen.
constexpr int kSubscribeRetryMs = 5000;
constexpr int kMaxSubscribeAttempts = 5;

// Account bookkeeping timestamp: milliseconds since the Unix epoch. Human
// scale, and deliberately not the ns units delivery_module stamps messages
// with — these two never mix.
qint64 nowMs() {
  return QDateTime::currentMSecsSinceEpoch();
}

// Schema version stamped into accounts.json, so a future layout change can be
// recognised rather than silently misparsed.
constexpr int kAccountsVersion = 1;

// The signing algorithm every account's key is minted with.
const QLatin1String kKeyAlgorithm("secp256k1");

// Small text-file helpers: the identity files are all short, single-value
// documents, and both the credential and accounts.json need reading/writing.
QString readTextFile(const QString &path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return QString();
  return QString::fromUtf8(f.readAll()).trimmed();
}

bool writeTextFile(const QString &path, const QString &contents) {
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
    return false;
  QTextStream out(&f);
  out << contents;
  return true;
}
} // namespace

// The LIP-23 content-topic app segment
// (https://lip.logos.co/messaging/informational/23/topics.html) and the two
// collections posts live in. The engine derives one topic per collection from
// these (kBucketBytes == 0), so every instance of this app shares one forum.
const char ExampleForumBackend::kAppName[] = "example-forum";
const char ExampleForumBackend::kTopicsCollection[] = "topics";
const char ExampleForumBackend::kRepliesCollection[] = "replies";

ExampleForumBackend::ExampleForumBackend() {
  // Runs in the ui-host process before the context is wired.
  logEvent("ctor — backend constructed (context not yet wired)");
  // EXAMPLE_FORUM_VERSION is injected by CMake from metadata.json#version.
  setAppVersion(QStringLiteral(EXAMPLE_FORUM_VERSION));
}

ExampleForumBackend::~ExampleForumBackend() {
  logEvent("dtor — backend destroyed");
}

void ExampleForumBackend::onContextReady() {
  logEvent("onContextReady — context wired, scheduling node bootstrap");
  // Both collection topics, for display. Derived rather than written out, so
  // the line on screen can't drift from what the engine actually joins.
  setTopic(QStringLiteral("%1 + %2")
               .arg(QString::fromStdString(cloud_data_core::sync_engine::contentTopicForDoc(
                        kAppName, 1, kTopicsCollection, QString().toStdString(), kBucketBytes)),
                    QString::fromStdString(cloud_data_core::sync_engine::contentTopicForDoc(
                        kAppName, 1, kRepliesCollection, QString().toStdString(), kBucketBytes))));

  // createNode()/start() are synchronous and can block for a moment. Defer the
  // bootstrap to the next event-loop turn so onContextReady() returns and the
  // QML view's replica can reach its Valid state promptly. modules() stays live.
  QTimer::singleShot(0, [this]() { bootstrap(); });
}

void ExampleForumBackend::bootstrap() {
  // --- Subscribe to delivery_module events before starting the node ---------

  // Node health. connectionStateChanged (Connected / PartiallyConnected /
  // Disconnected) is the only honest account of connectivity we get, so it
  // drives the status PROP outright. Deliberately ungated: the early events
  // land before the subscribe does, and dropping them is what let a node with
  // no peers sit there reporting "Connected".
  modules().delivery_module.on(
      "connectionStateChanged", [this](const QVariantList &data) {
        if (data.isEmpty())
          return;
        m_connectionState = data.at(0).toString();
        logEvent("connection state -> " + m_connectionState.toStdString());
        refreshStatus();
      });

  // Inbound CRDT ops. data[1] is the content topic, data[2] the raw payload.
  // The engine routes on topic shape (regular ops vs snapshot pointers),
  // merges by op id, and reports anything that actually changed through
  // handleDocumentChanged — so payloads that aren't ours are dropped there
  // rather than here.
  modules().delivery_module.on(
      "messageReceived", [this](const QVariantList &data) {
        if (data.size() < 3 || !m_engine)
          return;
        const QByteArray payload = data.at(2).toByteArray();
        m_engine->handleIncomingMessage(
            data.at(1).toString().toStdString(),
            std::vector<uint8_t>(payload.begin(), payload.end()));
      });

  // Registered alongside — not instead of — the handler above: the engine
  // prefers a reliable channel and latches back to plain send/subscribe only
  // once channelCreate() fails, and a host-owned node can be either. Both
  // paths are idempotent by op id, so an op arriving on both merges once.
  // data[0] is the channel id, which the engine sets to the content topic, so
  // the same routing applies.
  modules().delivery_module.on(
      "channelMessageReceived", [this](const QVariantList &data) {
        if (data.size() < 3 || !m_engine)
          return;
        const QByteArray payload = data.at(2).toByteArray();
        m_engine->handleIncomingMessage(
            data.at(0).toString().toStdString(),
            std::vector<uint8_t>(payload.begin(), payload.end()));
      });

  // Node startup. start() is dispatch-only in delivery_module v0.2.1 — its
  // contract is "`true` once dispatched; completion is reported via
  // `nodeStarted`" — so this event, not start()'s return, says whether the node
  // actually came up. The subscribe does not hang off it (see bootstrap's
  // ordering below); this only reports, and re-drives a subscribe that failed
  // earlier now that the node is definitely up.
  modules().delivery_module.on("nodeStarted", [this](const QVariantList &data) {
    const bool ok = !data.isEmpty() && data.at(0).toBool();
    const QString message = data.value(1).toString();
    logEvent("nodeStarted success=" + std::to_string(ok) + " " +
             message.toStdString());
    if (!ok) {
      setStatus(QStringLiteral("Node failed to start: %1").arg(message));
      return;
    }
    if (m_subscribed) {
      refreshStatus();
      return;
    }
    // Fresh retry budget: attempts spent while the node was still booting were
    // fighting a different problem than the one from here on. Deferred off the
    // event callback — calling synchronously back into the module from inside
    // its own event dispatch is exactly the shape that times out.
    m_subscribeAttempts = 0;
    QTimer::singleShot(0, [this]() { subscribeToForum(); });
  });

  // Delivery outcomes for our own posts, keyed by the request id the transport
  // hands back (see notePublished, which maps it to the post it carried). A
  // successful put() means the post is safe on disk and queued, not that it
  // left the machine; these events are what settle that second question.
  //
  // They do double duty now: as well as driving the view's per-post state,
  // they resolve the engine's outbox row for that request id. Until a row is
  // resolved the engine keeps re-issuing it on the next put/subscribe, so
  // skipping these would mean every op re-sent forever.
  modules().delivery_module.on(
      "messagePropagated", [this](const QVariantList &data) {
        // A waypoint, not an outcome — reported to the view, but the outbox
        // row stays open until the send is settled either way.
        settleSend(data.value(0).toString(), QStringLiteral("propagated"),
                   QString());
      });
  modules().delivery_module.on("messageSent", [this](const QVariantList &data) {
    const QString requestId = data.value(0).toString();
    settleSend(requestId, QStringLiteral("sent"), QString());
    if (m_engine)
      m_engine->onOutboxResolved(requestId.toStdString(), true);
  });
  modules().delivery_module.on(
      "messageError", [this](const QVariantList &data) {
        const QString requestId = data.value(0).toString();
        settleSend(requestId, QStringLiteral("failed"), data.value(2).toString());
        if (m_engine)
          m_engine->onOutboxResolved(requestId.toStdString(), false);
      });

  // The same bookkeeping for the reliable-channel path. Here data[0] is the
  // channel id and data[1] the request id (the plain-send events lead with the
  // request id instead), and the error string moves to data[2].
  modules().delivery_module.on(
      "channelMessageSent", [this](const QVariantList &data) {
        const QString requestId = data.value(1).toString();
        settleSend(requestId, QStringLiteral("sent"), QString());
        if (m_engine)
          m_engine->onOutboxResolved(requestId.toStdString(), true);
      });
  modules().delivery_module.on(
      "channelMessageError", [this](const QVariantList &data) {
        const QString requestId = data.value(1).toString();
        settleSend(requestId, QStringLiteral("failed"), data.value(2).toString());
        if (m_engine)
          m_engine->onOutboxResolved(requestId.toStdString(), false);
      });

  // --- storage_module events -> the engine's snapshot bridge ----------------
  // The blob-store adapter owns storage_module's wire format, so what reaches
  // the engine is already a parsed (success, sessionId, bytes). Inert unless
  // the user has a storage node up: this app never starts one (see openEngine).
  modules().storage_module.onStorageUploadProgress([this](const QString &payload) {
    if (!m_engine)
      return;
    const auto ev = StorageModuleBlobStore::parseUploadProgress(payload.toStdString());
    m_engine->onBlobUploadProgress(ev.success, ev.sessionId);
  });
  modules().storage_module.onStorageDownloadProgress([this](const QString &payload) {
    if (!m_engine)
      return;
    const auto ev = StorageModuleBlobStore::parseDownloadProgress(payload.toStdString());
    m_engine->onBlobDownloadProgress(ev.success, ev.sessionId, ev.chunk);
  });
  modules().storage_module.onStorageDownloadDone([this](const QString &payload) {
    if (!m_engine)
      return;
    const auto ev = StorageModuleBlobStore::parseDownloadDone(payload.toStdString());
    m_engine->onBlobDownloadDone(ev.success, ev.sessionId);
  });

  // --- Open/create this install's signing accounts ---------------------------
  // Independent of the delivery node below; publish() gates on both being
  // ready (nodeReady() and a selected account).
  loadAccounts();

  // --- Open the local store and replay what it holds -------------------------
  // Before the node, deliberately: local reads and writes never touch the
  // network, so the forum can be populated and composable while the node is
  // still bootstrapping — or when it never comes up at all.
  if (!openEngine()) {
    setStatus(QStringLiteral("Local store unavailable — posts can't be saved"));
    return;
  }

  // --- Create + start the node against the logos.test fleet -----------------
  // The *layered* config shape: `mode` and `preset` are both keys the layered
  // parser consumes, which is what earns the structured defaults — ephemeral
  // p2p ports, plus the host's per-instance localStoragePath. Adding any bare
  // WakuNodeConf key at the top level (logLevel, tcpPort, relay, …) flips
  // delivery's isFlatShape() check and reclassifies the whole config as the
  // legacy flat shape, which since delivery v0.2.0 no longer zeroes the
  // listening ports — it binds upstream's fixed defaults (tcp 60000), so two
  // instances on one machine collide. `logLevel` used to sit here and did
  // exactly that. Tuning keys belong inside messagingOverrides /
  // channelsOverrides / kernelConf instead; getAvailableConfigs() reports
  // what the running module accepts.
  const QJsonObject cfg{
      {"mode", "Core"},
      {"preset", "logos.test"},
  };
  const QString cfgJson =
      QString::fromUtf8(QJsonDocument(cfg).toJson(QJsonDocument::Compact));

  LogosResult created = modules().delivery_module.createNode(cfgJson);
  if (!created.success) {
    // delivery_module is a singleton shared across Basecamp apps, so another
    // app may have already created and started the node. createNode then fails
    // and no nodeStarted will ever fire for us — subscribe directly.
    logEvent("createNode failed (node may already be running): " +
             created.getError().toStdString());
    subscribeToForum();
    return;
  }

  logEvent("createNode succeeded");

  // Subscribe *before* start(), which is what use-delivery-module documents
  // ("Subscribe before `start()`, and wire event `.on(...)` handlers before
  // triggering sends, or you'll miss early events"). This is the one window
  // where both hazards are absent: the node is built but not yet bootstrapping,
  // so the call doesn't queue behind discovery work and time out, and the
  // subscription is registered before any message can arrive — so there is no
  // race with start() left to lose either.
  subscribeToForum();

  setStatus(QStringLiteral("Starting node…"));
  LogosResult started = modules().delivery_module.start();
  if (!started.success) {
    setStatus(QStringLiteral("Node failed to start: %1").arg(started.getError()));
    logEvent("start failed: " + started.getError().toStdString());
    return;
  }
  logEvent("start dispatched — waiting for nodeStarted");
}

void ExampleForumBackend::subscribeToForum() {
  if (m_subscribed)
    return; // the pre-start attempt and nodeStarted can both land

  if (!m_engine)
    return; // nothing to subscribe with yet; openEngine() drives the first try

  ++m_subscribeAttempts;
  // One call per collection. The doc id is irrelevant to the topic at
  // kBucketBytes == 0 (see the header), so a sentinel stands in for it — what
  // this actually joins is the whole collection, which is what a forum needs:
  // posts arrive from peers whose doc ids we have never seen.
  const cloud_data_core::EngineResult topics =
      m_engine->subscribe(kTopicsCollection, "*");
  const cloud_data_core::EngineResult replies =
      m_engine->subscribe(kRepliesCollection, "*");
  if (!topics.success || !replies.success) {
    const std::string detail = topics.success ? replies.error : topics.error;
    setStatus(QStringLiteral("subscribe failed: %1")
                  .arg(QString::fromStdString(detail)));
    logEvent("subscribe attempt " + std::to_string(m_subscribeAttempts) +
             " failed: " + detail);
    // Retry rather than leaving the app receiving nothing. The common failure
    // here is a timeout because the node is busy bootstrapping, which passes
    // on its own. Composing is unaffected — that runs off the local store.
    if (m_subscribeAttempts < kMaxSubscribeAttempts)
      QTimer::singleShot(kSubscribeRetryMs, [this]() { subscribeToForum(); });
    return;
  }

  m_subscribed = true;
  refreshStatus();
  logEvent("subscribed — forum on the topics + replies collections");
}

void ExampleForumBackend::refreshStatus() {
  if (!m_subscribed)
    return; // bootstrap's own progress messages own the status until then

  if (m_connectionState.isEmpty()) {
    // Subscribed locally, but nothing has yet said we have peers — and a node
    // with none receives nothing while still publishing happily. Name that
    // state instead of claiming "Connected", which is what the old status line
    // did and what made a node talking to nobody indistinguishable from a
    // healthy one.
    setStatus(QStringLiteral("Subscribed — waiting for peers"));
    return;
  }
  setStatus(m_connectionState);
}

void ExampleForumBackend::settleSend(const QString &requestId,
                                     const QString &state,
                                     const QString &detail) {
  if (requestId.isEmpty())
    return;

  const auto it = m_pendingSends.constFind(requestId);
  if (it == m_pendingSends.constEnd())
    return; // another app's send — delivery_module is shared

  const QString messageId = it.value();
  // "propagated" is a waypoint, not an outcome: the message has reached the
  // network but isn't validated yet, so keep the mapping for the event that
  // settles it.
  if (state != QLatin1String("propagated"))
    m_pendingSends.remove(requestId);

  logEvent("send " + requestId.toStdString() + " -> " + state.toStdString() +
           (detail.isEmpty() ? "" : " (" + detail.toStdString() + ")"));
  emit messageStateChanged(messageId, state, detail);
}

QString ExampleForumBackend::identityDir() const {
  // Basecamp's --user-dir gives an instance its own data tree (plugins,
  // modules, module_data, logs) and exports it to every child process — this
  // backend's ui-host included — as LOGOS_USER_DIR. keystore_signer keeps its
  // keys inside that tree (<user dir>/module_data/keystore_signer/<instance>),
  // so our credential + accounts have to live there too: an account stored
  // outside it is reloaded against a keystore that has never seen its key, and
  // every sign() then fails. Sit next to the keystore we're bound to.
  const QString userDir = qEnvironmentVariable("LOGOS_USER_DIR");
  if (!userDir.isEmpty())
    return userDir + QStringLiteral("/module_data/example_forum/identity");

  // Default launch (no --user-dir): AppDataLocation is keyed off the *host*
  // process's org/app name (the ui-host, not this plugin), so namespace under
  // it by module name to avoid colliding with any other Logos module's local
  // data. This path doesn't track the keystore's data tree either, but
  // loadAccounts() reconciles against the keystore, so a mismatch costs the
  // account labels rather than every publish().
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
         QStringLiteral("/example_forum/identity");
}

bool ExampleForumBackend::ensureCredential() {
  const QString dir = identityDir();
  QDir().mkpath(dir);
  const QString credentialFile =
      dir + QStringLiteral("/keystore_signer_credential");

  QByteArray credential =
      QByteArray::fromHex(readTextFile(credentialFile).toLatin1());
  if (credential.size() == kCredentialBytes) {
    m_credential = credential;
    return true;
  }
  if (!credential.isEmpty())
    logEvent("credential in " + dir.toStdString() +
             " is malformed — generating a new one");

  // A random 256-bit credential. Not a user secret, but isolation plumbing
  // (see loadAccounts()'s header doc comment). Replacing it means a brand-new
  // keystore namespace, so any accounts.json beside it reconciles to empty and
  // a fresh account is minted below — the same outcome as a first run.
  credential = QByteArray(kCredentialBytes, 0);
  for (int i = 0; i < credential.size(); ++i)
    credential[i] =
        static_cast<char>(QRandomGenerator::global()->generate() & 0xFF);

  if (!writeTextFile(credentialFile,
                     QString::fromLatin1(credential.toHex()))) {
    logEvent("failed to persist credential to " + dir.toStdString());
    return false;
  }
  m_credential = credential;
  return true;
}

void ExampleForumBackend::loadAccounts() {
  if (!ensureCredential())
    return; // no credential, no accounts — publish() reports "Identity not ready"

  const QString dir = identityDir();
  const QString accountsFile = dir + QStringLiteral("/accounts.json");

  // --- What we last persisted: the labels and the selection -----------------
  // The keystore holds the keys but knows nothing about names or which one the
  // user was posting under, so that half comes from disk.
  QVector<Account> persisted;
  QString selected;
  const QJsonDocument doc =
      QJsonDocument::fromJson(readTextFile(accountsFile).toUtf8());
  if (doc.isObject()) {
    const QJsonObject obj = doc.object();
    selected = obj.value(QStringLiteral("selected")).toString();
    const QJsonArray stored = obj.value(QStringLiteral("accounts")).toArray();
    for (const QJsonValue &value : stored) {
      const QJsonObject entry = value.toObject();
      Account acct;
      acct.keyId = entry.value(QStringLiteral("keyId")).toString();
      acct.label = entry.value(QStringLiteral("label")).toString();
      acct.createdAt = static_cast<qint64>(
          entry.value(QStringLiteral("createdAt")).toDouble());
      if (!acct.keyId.isEmpty())
        persisted.append(acct);
    }
  } else {
    // No accounts.json. A pre-accounts install still has the single key_id
    // file, so adopt that key as the first account — minting a second identity
    // beside one the user has already been posting under would silently change
    // their author id. The legacy file is left alone from here on.
    const QString legacyKeyId = readTextFile(dir + QStringLiteral("/key_id"));
    if (!legacyKeyId.isEmpty()) {
      Account acct;
      acct.keyId = legacyKeyId;
      acct.label = QStringLiteral("Account 1");
      acct.createdAt = nowMs();
      persisted.append(acct);
      selected = legacyKeyId;
      logEvent("migrating legacy identity " + legacyKeyId.toStdString() +
               " into accounts.json");
    }
  }

  // --- Reconcile against the keystore, which owns the real key set ----------
  logos::CallError err;
  const QStringList live =
      modules().keystore_signer.listKeys(m_credential, &err);

  m_accounts.clear();
  if (!err.ok()) {
    // The *call* failed, as opposed to reporting an empty namespace, so `live`
    // says nothing about what exists. Trusting it would drop every account on
    // a transient keystore hiccup, so validate each persisted account
    // individually instead — the same publicKey() probe the single-identity
    // path used, just applied per account.
    logEvent("listKeys failed (" + err.code + ": " + err.message +
             ") — validating persisted accounts individually");
    for (const Account &acct : persisted) {
      logos::CallError probe;
      if (!modules()
               .keystore_signer.publicKey(m_credential, acct.keyId, &probe)
               .isEmpty())
        m_accounts.append(acct);
      else
        logEvent("dropping account " + acct.keyId.toStdString() +
                 " — keystore holds no such key");
    }
  } else {
    // Keys that still exist keep their persisted label and order...
    for (const Account &acct : persisted) {
      if (live.contains(acct.keyId))
        m_accounts.append(acct);
      else
        logEvent("dropping account " + acct.keyId.toStdString() +
                 " — keystore no longer holds it");
    }
    // ...and keys the keystore holds that we have no record of get adopted, so
    // a lost or corrupt accounts.json heals into labelled accounts instead of
    // stranding usable keys and minting duplicates next to them.
    for (const QString &keyId : live) {
      if (indexOfAccount(keyId) >= 0)
        continue;
      Account acct;
      acct.keyId = keyId;
      acct.label = defaultAccountLabel();
      acct.createdAt = nowMs();
      m_accounts.append(acct);
      logEvent("adopted unlabelled keystore key " + keyId.toStdString() +
               " as \"" + acct.label.toStdString() + "\"");
    }
  }

  // --- First run, or nothing survived: mint the starting account ------------
  if (m_accounts.isEmpty()) {
    QString keyId;
    const QString error = mintAccount(defaultAccountLabel(), &keyId);
    if (!error.isEmpty()) {
      logEvent("could not mint a first account: " + error.toStdString());
      return; // m_keyId stays empty; publish() fails closed on it
    }
    selected = keyId;
  }

  // publishAccountState() re-points the selection if `selected` names an
  // account that didn't survive reconciliation.
  m_keyId = selected;
  publishAccountState();
  saveAccounts();
  logEvent("accounts ready — " + std::to_string(m_accounts.size()) +
           " account(s), signing as " + m_keyId.toStdString());
}

QString ExampleForumBackend::mintAccount(const QString &label,
                                         QString *outKeyId) {
  logos::CallError err;
  const QString keyId =
      modules().keystore_signer.createKey(m_credential, kKeyAlgorithm, &err);
  if (keyId.isEmpty()) {
    // Same empty-return convention as sign() (see publish()): keystore_signer
    // has no result envelope, so err is normally blank even on failure.
    // Substitute a description rather than letting "" reach the view, which
    // reads "" as success.
    const std::string detail =
        err.message.empty() ? std::string("keystore_signer returned no key id")
                            : err.message;
    logEvent("createKey failed: " + detail);
    return QString::fromStdString("Couldn't create account: " + detail);
  }

  Account acct;
  acct.keyId = keyId;
  acct.label = label;
  acct.createdAt = nowMs();
  m_accounts.append(acct);
  if (outKeyId)
    *outKeyId = keyId;
  logEvent("minted account " + keyId.toStdString() + " (\"" +
           label.toStdString() + "\")");
  return QString();
}

bool ExampleForumBackend::saveAccounts() {
  QJsonArray stored;
  for (const Account &acct : m_accounts) {
    QJsonObject entry;
    entry.insert(QStringLiteral("keyId"), acct.keyId);
    entry.insert(QStringLiteral("label"), acct.label);
    entry.insert(QStringLiteral("createdAt"), acct.createdAt);
    stored.append(entry);
  }
  QJsonObject root;
  root.insert(QStringLiteral("version"), kAccountsVersion);
  root.insert(QStringLiteral("selected"), m_keyId);
  root.insert(QStringLiteral("accounts"), stored);

  const QString path = identityDir() + QStringLiteral("/accounts.json");
  if (!writeTextFile(path, QString::fromUtf8(QJsonDocument(root).toJson(
                               QJsonDocument::Indented)))) {
    // In-memory state stands, so the session keeps working — only the
    // selection and labels are lost on the next start.
    logEvent("failed to persist accounts to " + path.toStdString());
    return false;
  }
  return true;
}

void ExampleForumBackend::publishAccountState() {
  // m_keyId must always name a held account, or be empty when there are none.
  // Every mutation lands here, so this is the one place that re-establishes
  // that invariant — including after a delete removed the selected account.
  if (indexOfAccount(m_keyId) < 0)
    m_keyId = m_accounts.isEmpty() ? QString() : m_accounts.first().keyId;

  QJsonArray view;
  for (const Account &acct : m_accounts) {
    QJsonObject entry;
    entry.insert(QStringLiteral("keyId"), acct.keyId);
    entry.insert(QStringLiteral("label"), acct.label);
    view.append(entry);
  }
  setAccountsJson(QString::fromUtf8(
      QJsonDocument(view).toJson(QJsonDocument::Compact)));

  const int selectedIndex = indexOfAccount(m_keyId);
  setMyLabel(selectedIndex >= 0 ? m_accounts.at(selectedIndex).label
                                : QString());
  // myAddress last: the view gates composing on it, so it should only go
  // non-empty once the list and label it refers to are already published.
  setMyAddress(m_keyId);
}

int ExampleForumBackend::indexOfAccount(const QString &keyId) const {
  if (keyId.isEmpty())
    return -1;
  for (int i = 0; i < m_accounts.size(); ++i)
    if (m_accounts.at(i).keyId == keyId)
      return i;
  return -1;
}

QString ExampleForumBackend::defaultAccountLabel() const {
  // Count up from the current size, skipping names already in use so deleting
  // "Account 2" of three doesn't hand the next account a name that's still on
  // screen. At most m_accounts.size() names are taken, so this terminates.
  for (int n = m_accounts.size() + 1;; ++n) {
    const QString candidate = QStringLiteral("Account %1").arg(n);
    bool taken = false;
    for (const Account &acct : m_accounts) {
      if (acct.label == candidate) {
        taken = true;
        break;
      }
    }
    if (!taken)
      return candidate;
  }
}

QString ExampleForumBackend::createAccount(QString label) {
  if (m_credential.isEmpty())
    return QStringLiteral("Accounts aren't ready yet");

  label = label.trimmed();
  if (label.isEmpty())
    label = defaultAccountLabel();

  QString keyId;
  const QString error = mintAccount(label, &keyId);
  if (!error.isEmpty())
    return error;

  // Select what was just created — creating an account and then continuing to
  // post as the old one would be surprising.
  m_keyId = keyId;
  publishAccountState();
  saveAccounts();
  return QString(); // empty == success
}

QString ExampleForumBackend::selectAccount(QString keyId) {
  if (indexOfAccount(keyId) < 0)
    return QStringLiteral("No such account");
  if (keyId == m_keyId)
    return QString(); // already signing as this one

  m_keyId = keyId;
  publishAccountState();
  saveAccounts();
  logEvent("selected account " + keyId.toStdString());
  return QString();
}

QString ExampleForumBackend::renameAccount(QString keyId, QString label) {
  const int index = indexOfAccount(keyId);
  if (index < 0)
    return QStringLiteral("No such account");
  label = label.trimmed();
  if (label.isEmpty())
    return QStringLiteral("An account needs a name");

  // Display metadata only — the key is untouched, so this changes neither what
  // was signed before nor what can be signed after.
  m_accounts[index].label = label;
  publishAccountState();
  saveAccounts();
  return QString();
}

QString ExampleForumBackend::deleteAccount(QString keyId) {
  const int index = indexOfAccount(keyId);
  if (index < 0)
    return QStringLiteral("No such account");
  if (m_accounts.size() == 1)
    return QStringLiteral("Can't delete your only account");

  // Destroy the key first: dropping the account while the keystore still held
  // its key would strand a usable key that the next reconcile would silently
  // re-adopt as an unlabelled account.
  logos::CallError err;
  const bool deleted =
      modules().keystore_signer.deleteKey(m_credential, keyId, &err);
  if (!deleted || !err.ok()) {
    const std::string detail =
        err.message.empty()
            ? "keystore_signer did not delete key " + keyId.toStdString()
            : err.message;
    logEvent("deleteKey failed: " + detail);
    return QString::fromStdString("Couldn't delete account: " + detail);
  }

  const std::string label = m_accounts.at(index).label.toStdString();
  m_accounts.removeAt(index);
  // publishAccountState() re-selects when this was the selected account.
  publishAccountState();
  saveAccounts();
  logEvent("deleted account " + keyId.toStdString() + " (\"" + label + "\")");
  return QString();
}

QString ExampleForumBackend::createTopic(QString title, QString body) {
  if (title.isEmpty())
    return QStringLiteral("A topic needs a title");

  ForumMessage msg;
  msg.type = QStringLiteral("topic");
  msg.id = topicIdFor(title); // content-addressed: derived from the title
  msg.title = title;
  msg.body = body;
  return publish(msg);
}

QString ExampleForumBackend::reconstructTopic(QString topicId, QString title) {
  // Local recovery for a topic we only know through its replies (a backfilled
  // placeholder in the view). The topic id is a hash of the title, so a title
  // shared out-of-band and pasted here is provably the right one iff its hash
  // matches. No network send — we just surface the verified topic locally, with
  // an empty body (the body isn't part of the id and can't be recovered from
  // it; it fills in later only if the original topic message reaches us).
  if (title.isEmpty())
    return QStringLiteral("Enter the topic title");
  if (topicIdFor(title) != topicId)
    return QStringLiteral("That title doesn't match this topic");

  // No author: this is a local-only reconstruction from a title, not a
  // signed message that arrived over the network.
  emit topicReceived(topicId, title, QString(), QString(), nowNs());
  return QString(); // empty == success
}

QString ExampleForumBackend::replyToTopic(QString topicId, QString body) {
  if (topicId.isEmpty())
    return QStringLiteral("No topic selected");
  if (body.isEmpty())
    return QStringLiteral("A reply needs a body");

  ForumMessage msg;
  msg.type = QStringLiteral("reply");
  msg.id = newId();
  msg.topicId = topicId;
  msg.body = body;
  return publish(msg);
}

QString ExampleForumBackend::publish(ForumMessage msg) {
  // Unconditional entry log — the two early-return guards below fail closed
  // and silently (no send, no local echo, so nothing reaches the topics/reply
  // list), so this is what distinguishes "never got here" from "got here and
  // one of the guards tripped" when diagnosing a post that doesn't appear.
  logEvent("publish(" + msg.type.toStdString() + " id=" + msg.id.toStdString() +
           "): contextReady=" + std::to_string(isContextReady()) +
           " nodeReady=" + std::to_string(nodeReady()) +
           " myKeyId=" + (m_keyId.isEmpty() ? "<empty>" : m_keyId.toStdString()));

  if (!isContextReady() || !m_engine)
    return QStringLiteral("Store not ready");
  if (m_keyId.isEmpty())
    return QStringLiteral("Identity not ready");

  // Sign the canonical payload (Keccak-256, matching go-wallet-sdk's
  // go-ethereum-derived signing convention) before storing — author/sig must
  // be set on msg itself so they land in the document the engine syncs.
  msg.author = m_keyId;
  const QByteArray signingBytes = forumMessageSigningBytes(msg);
  const QByteArray hash = QCryptographicHash::hash(signingBytes,
                                                    QCryptographicHash::Keccak_256);

  // Sign with keystore-signer-module. Unlike accounts_module, keystore-signer
  // provides per-caller isolation via the secret credential, so no re-init
  // workaround is needed. The sign() call returns raw signature bytes.
  logos::CallError err;
  const QByteArray sigBytes =
      modules().keystore_signer.sign(m_credential, m_keyId, hash, &err);
  if (sigBytes.isEmpty()) {
    // keystore_signer reports failure by returning empty bytes, not an error
    // (its .lidl has no result envelope), so err.message is normally empty
    // here. Substitute a real description: an empty return would otherwise
    // reach the view as "" — which the view reads as success — silently
    // dropping the post instead of showing why it failed.
    const std::string detail =
        err.message.empty()
            ? "keystore_signer returned no signature for key " + m_keyId.toStdString() +
                  " (is this identity still in its keystore?)"
            : err.message;
    logEvent("sign failed: " + detail);
    return QString::fromStdString("sign failed: " + detail);
  }
  // Convert signature bytes to hex string for JSON wire format.
  msg.sig = QStringLiteral("0x") + QString::fromLatin1(sigBytes.toHex());

  // The document the engine stores and syncs. `ts` is a *string* deliberately:
  // it is nanoseconds since the epoch (~1.7e18), which a JSON number would
  // round, since that exceeds the 2^53 a double represents exactly.
  const bool isTopic = msg.type == QLatin1String("topic");
  QJsonObject doc{
      {"type", msg.type},
      {"body", msg.body},
      {"author", msg.author},
      {"sig", msg.sig},
      {"ts", QString::number(nowNs())},
  };
  if (isTopic)
    doc.insert("title", msg.title);
  else
    doc.insert("topicId", msg.topicId);

  const std::string collection = isTopic ? kTopicsCollection : kRepliesCollection;
  const cloud_data_core::EngineResult r = m_engine->put(
      collection, msg.id.toStdString(),
      QString::fromUtf8(QJsonDocument(doc).toJson(QJsonDocument::Compact))
          .toStdString());
  if (!r.success) {
    logEvent("put failed: " + r.error);
    return QString::fromStdString(r.error);
  }
  logEvent("stored " + msg.type.toStdString() + " id=" + msg.id.toStdString());

  // No local echo: put() already reported the write through
  // handleDocumentChanged() before returning, so the post is on screen.
  //
  // Mark it unconfirmed all the same. The write is durable locally the moment
  // put() succeeds, but that says nothing about whether it left the machine —
  // the engine parks it in an outbox and retries. settleSend() resolves this
  // once delivery_module reports what became of the broadcast.
  emit messageStateChanged(msg.id, QStringLiteral("pending"), QString());
  return QString(); // empty == success
}

bool ExampleForumBackend::openEngine() {
  // Sit beside the identity in the same per-instance data tree (see
  // identityDir()'s doc comment), so the store follows the keystore whose keys
  // signed its posts rather than drifting into another Basecamp instance's.
  QString dir = identityDir();
  dir.chop(QStringLiteral("/identity").size());
  dir += QStringLiteral("/store");
  QDir().mkpath(dir);

  // A stable per-install peer id. It tie-breaks concurrent CRDT writes and
  // namespaces op ids, so it has to survive a restart — and deliberately is
  // *not* the selected account, which the user can switch or delete without
  // meaning to fork this install's op history.
  const QString peerIdFile = dir + QStringLiteral("/peer_id");
  QString peerId = readTextFile(peerIdFile);
  if (peerId.isEmpty()) {
    peerId = newId();
    if (!writeTextFile(peerIdFile, peerId)) {
      logEvent("failed to persist peer id under " + dir.toStdString());
      return false;
    }
  }

  m_transport = std::make_unique<DeliveryModuleTransport>(modules());
  m_blobStore = std::make_unique<StorageModuleBlobStore>(modules());

  cloud_data_core::EngineConfig cfg;
  // The app segment of every topic the engine derives, which is what keeps
  // this forum's traffic off any other embedder's topics.
  cfg.appName = kAppName;
  cfg.bucketBytes = kBucketBytes;

  m_engine = std::make_unique<cloud_data_core::CloudDataEngine>(
      dir.toStdString(), peerId.toStdString(), cfg, *m_transport, *m_blobStore);
  if (!m_engine->open()) {
    logEvent("could not open the local store under " + dir.toStdString());
    m_engine.reset();
    return false;
  }

  // One path for every materialized change, whatever caused it: a local put,
  // a merged remote op, or a row replayed from disk below. `origin` is
  // deliberately ignored — the view renders its own posts and everyone else's
  // identically, and its de-dupe by id makes a repeat harmless.
  m_engine->setOnDocumentChanged([this](const std::string &collectionId,
                                        const std::string &docId,
                                        const std::string &json,
                                        const std::string &origin) {
    (void)origin;
    handleDocumentChanged(collectionId, docId, json);
  });
  m_transport->setPublishObserver(
      [this](const std::string &requestId, const std::vector<uint8_t> &payload) {
        notePublished(requestId, payload);
      });

  logEvent("local store open at " + dir.toStdString() +
           ", peer " + peerId.toStdString());
  replayPersisted();

  // Composing runs off the local store, not the network: put() is durable and
  // queued for broadcast whether or not a node ever comes up. Gating this on a
  // successful subscribe — which is what it did before there was a store —
  // would now refuse posts the app can keep perfectly well. Connectivity stays
  // a separate question, reported through the status PROP.
  setNodeReady(true);
  return true;
}

void ExampleForumBackend::replayPersisted() {
  // Topics before replies, so a reply never has to stand up a placeholder for
  // a topic that is about to be replayed two rows later.
  for (const char *collection : {kTopicsCollection, kRepliesCollection}) {
    const cloud_data_core::EngineResult r = m_engine->query(collection, "{}");
    if (!r.success) {
      logEvent(std::string("replay of ") + collection + " failed: " + r.error);
      continue;
    }
    if (!r.value.is_array())
      continue;

    // SQLite hands rows back in no particular order, so sort by the post's own
    // timestamp before replaying — otherwise a restart would shuffle every
    // thread into storage order.
    std::vector<std::pair<qint64, const nlohmann::json *>> rows;
    for (const auto &row : r.value) {
      if (!row.is_object() || !row.contains("docId") || !row["docId"].is_string())
        continue;
      qint64 ts = 0;
      if (row.contains("ts") && row["ts"].is_string())
        ts = QString::fromStdString(row["ts"].get<std::string>()).toLongLong();
      rows.emplace_back(ts, &row);
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });

    for (const auto &[ts, row] : rows) {
      (void)ts;
      handleDocumentChanged(collection, (*row)["docId"].get<std::string>(), row->dump());
    }
    logEvent("replayed " + std::to_string(rows.size()) + " persisted " +
             collection);
  }
}

void ExampleForumBackend::handleDocumentChanged(const std::string &collectionId,
                                                 const std::string &docId,
                                                 const std::string &json) {
  const QJsonDocument parsed =
      QJsonDocument::fromJson(QByteArray::fromStdString(json));
  if (!parsed.isObject())
    return;
  const QJsonObject obj = parsed.object();
  // A removed document still materializes, carrying the engine's reserved
  // tombstone marker. Nothing in this app deletes posts, but a merged remote
  // tombstone would otherwise put one back on screen.
  if (obj.value(QStringLiteral("$deleted")).toBool())
    return;

  ForumMessage msg;
  msg.id = QString::fromStdString(docId);
  msg.body = obj.value(QStringLiteral("body")).toString();
  // Claimed, not verified — see the .rep's topicReceived doc comment. Carried
  // as ordinary document fields, so the signature survives the trip through
  // the store and out to other peers unchanged.
  msg.author = obj.value(QStringLiteral("author")).toString();
  msg.sig = obj.value(QStringLiteral("sig")).toString();
  const qint64 ts = obj.value(QStringLiteral("ts")).toString().toLongLong();

  if (collectionId == kTopicsCollection) {
    msg.type = QStringLiteral("topic");
    msg.title = obj.value(QStringLiteral("title")).toString();
    if (msg.title.isEmpty())
      return; // a topic must have a title
  } else if (collectionId == kRepliesCollection) {
    msg.type = QStringLiteral("reply");
    msg.topicId = obj.value(QStringLiteral("topicId")).toString();
    if (msg.topicId.isEmpty())
      return; // a reply must reference its topic
  } else {
    return; // not a collection this app knows
  }

  emitForumMessage(msg, ts != 0 ? ts : nowNs());
}

void ExampleForumBackend::notePublished(const std::string &requestId,
                                         const std::vector<uint8_t> &payload) {
  if (requestId.empty())
    return;

  // The op carries the doc id, which is the message id the view knows a post
  // by — so decoding what was just published is what links a delivery request
  // id back to a row on screen. Anything that isn't an op (a snapshot pointer,
  // say) is not something the view tracks per-post.
  std::string collectionId;
  cloud_data_core::CrdtOp op;
  if (!cloud_data_core::sync_engine::decodeOp(payload, collectionId, op))
    return;

  m_pendingSends.insert(QString::fromStdString(requestId),
                        QString::fromStdString(op.docId));
}

void ExampleForumBackend::emitForumMessage(const ForumMessage &msg,
                                           qint64 timestamp) {
  if (msg.type == QLatin1String("topic"))
    emit topicReceived(msg.id, msg.title, msg.body, msg.author, timestamp);
  else if (msg.type == QLatin1String("reply"))
    emit replyReceived(msg.id, msg.topicId, msg.body, msg.author, timestamp);
}
