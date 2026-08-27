#include "example_forum_backend.h"

#include <iostream>

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

// Store backfill bounds: how far back to ask, how many messages to take, and
// how long to wait for the peer.
constexpr qint64 kStoreBackfillWindowNs = 24LL * 60 * 60 * 1000 * 1000000LL;
constexpr int kStoreBackfillLimit = 100;
constexpr int kStoreQueryTimeoutMs = 10000;

// The delivery node config: the defaults, with EXAMPLE_FORUM_DELIVERY_CFG (raw
// JSON object) merged over them, top-level key by top-level key.
//
// Overridable because the defaults can't serve two instances on one machine.
// Both share a public IP, and the relay network scores that down hard — the
// gossipsub ipColocationFactor* weights, plus the peer manager's own colocation
// limit — so the second node is pruned rather than grafted, ends up logging
// "No mesh peer for the given pubsub topic", and receives nothing. It still
// publishes fine (an empty mesh falls back to fanout), so its posts reach
// everyone else and only its own inbox stays empty. Colocation scoring is
// applied by the remote peers, so nothing set locally overrides it; what an
// override buys is a node that doesn't need mesh membership in the first place:
//
//   EXAMPLE_FORUM_DELIVERY_CFG='{"mode":"Edge"}'
//       light node — takes messages from filter/lightpush service peers.
//   EXAMPLE_FORUM_DELIVERY_CFG='{"tcpPort":60001,"discv5UdpPort":9001}'
//       explicit ports, so each node is dialable instead of fighting over the
//       OS-assigned ones behind NAT (the `dialMe timed out` case).
//
// Two IPs (a second machine, VM, or network namespace) remains the clean test.
QString deliveryConfigJson() {
  QJsonObject cfg{
      {"logLevel", "INFO"},
      {"mode", "Core"},
      {"preset", "logos.test"},
  };

  const QString overrides = qEnvironmentVariable("EXAMPLE_FORUM_DELIVERY_CFG");
  if (!overrides.isEmpty()) {
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(overrides.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
      // Fall back to the defaults rather than refusing to start: a typo in a
      // debugging env var shouldn't cost you the app.
      logEvent("ignoring malformed EXAMPLE_FORUM_DELIVERY_CFG (" +
               err.errorString().toStdString() + ")");
    } else {
      const QJsonObject obj = doc.object();
      for (auto it = obj.begin(); it != obj.end(); ++it)
        cfg.insert(it.key(), it.value());
    }
  }

  return QString::fromUtf8(QJsonDocument(cfg).toJson(QJsonDocument::Compact));
}

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

// A LIP-23 content topic (https://lip.logos.co/messaging/informational/23/topics.html).
// Hard-coded so every instance of this app shares one forum.
const QString ExampleForumBackend::kTopic =
    QStringLiteral("/example-forum/1/forum/proto");

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
  setTopic(kTopic);

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

  // Inbound forum messages on the subscribed topic. data[2] is the raw payload
  // bytes (a JSON ForumMessage envelope); data[3] is the timestamp (qint64, ns
  // since epoch). Non-forum payloads are ignored.
  modules().delivery_module.on(
      "messageReceived", [this](const QVariantList &data) {
        if (data.size() < 4)
          return;
        ForumMessage msg;
        if (!decodeForumMessage(data.at(2).toByteArray(), msg)) {
          logEvent("ignored non-forum message on " +
                   data.at(1).toString().toStdString());
          return;
        }
        logEvent("received " + msg.type.toStdString() +
                 " id=" + msg.id.toStdString());
        emitForumMessage(msg, data.at(3).toLongLong());
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
    m_nodeStarted = true;
    // Both of these call synchronously back into delivery_module, so they run
    // off the event callback: calling in from inside the module's own event
    // dispatch is exactly the shape that times out.
    if (m_subscribed) {
      refreshStatus();
      QTimer::singleShot(0, [this]() { backfillFromStore(); });
      return;
    }
    // Fresh retry budget: attempts spent while the node was still booting were
    // fighting a different problem than the one from here on.
    m_subscribeAttempts = 0;
    QTimer::singleShot(0, [this]() { subscribeToForum(); });
  });

  // Delivery outcomes for our own posts, keyed by the request id send() hands
  // back. publish()'s local echo is a claim that we tried, not evidence that
  // anything left the machine; these events are what settle it.
  modules().delivery_module.on(
      "messagePropagated", [this](const QVariantList &data) {
        settleSend(data.value(0).toString(), QStringLiteral("propagated"),
                   QString());
      });
  modules().delivery_module.on("messageSent", [this](const QVariantList &data) {
    settleSend(data.value(0).toString(), QStringLiteral("sent"), QString());
  });
  modules().delivery_module.on(
      "messageError", [this](const QVariantList &data) {
        settleSend(data.value(0).toString(), QStringLiteral("failed"),
                   data.value(2).toString());
      });

  // --- Open/create this install's signing accounts ---------------------------
  // Independent of the delivery node below; publish() gates on both being
  // ready (nodeReady() and a selected account).
  loadAccounts();

  // --- Create + start the node ----------------------------------------------
  const QString cfgJson = deliveryConfigJson();
  logEvent("createNode config: " + cfgJson.toStdString());

  LogosResult created = modules().delivery_module.createNode(cfgJson);
  if (!created.success) {
    // delivery_module is a singleton shared across Basecamp apps, so another
    // app may have already created and started the node. createNode then fails
    // and no nodeStarted will ever fire for us — subscribe directly.
    logEvent("createNode failed (node may already be running): " +
             created.getError().toStdString());
    // Whoever created it started it too, and no nodeStarted will fire for us.
    m_nodeStarted = true;
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

  ++m_subscribeAttempts;
  LogosResult subscribed = modules().delivery_module.subscribe(kTopic);
  if (!subscribed.success) {
    setStatus(QStringLiteral("subscribe failed: %1").arg(subscribed.getError()));
    logEvent("subscribe attempt " + std::to_string(m_subscribeAttempts) +
             " failed: " + subscribed.getError().toStdString());
    // Retry rather than leaving the app receiving nothing with composing
    // disabled. The common failure here is a timeout because the node is busy
    // bootstrapping, which passes on its own.
    if (m_subscribeAttempts < kMaxSubscribeAttempts)
      QTimer::singleShot(kSubscribeRetryMs, [this]() { subscribeToForum(); });
    return;
  }

  m_subscribed = true;
  // Composing is gated on nodeReady, and publishing genuinely does work without
  // mesh peers (that is exactly the failure mode this app used to hide), so a
  // successful subscribe is the right gate. Connectivity is reported separately,
  // through the status PROP.
  setNodeReady(true);
  refreshStatus();
  logEvent("subscribed — forum on " + kTopic.toStdString());

  backfillFromStore();
}

void ExampleForumBackend::refreshStatus() {
  if (!m_subscribed)
    return; // bootstrap's own progress messages own the status until then

  if (m_connectionState.isEmpty()) {
    // Subscribed locally, but nothing has yet said we have peers. This is the
    // state a second instance on one machine gets stuck in when the network
    // prunes it for IP colocation, so name it instead of claiming "Connected".
    setStatus(QStringLiteral("Subscribed — waiting for peers"));
    return;
  }
  setStatus(m_connectionState);
}

void ExampleForumBackend::backfillFromStore() {
  // The subscribe now runs before start(), so this can be reached with a node
  // that isn't up yet; nodeStarted calls back here once it is. One-shot: a
  // catch-up, not something to repeat on every start report.
  if (m_backfilled || !m_subscribed || !m_nodeStarted)
    return;

  const QString peer = qEnvironmentVariable("EXAMPLE_FORUM_STORE_PEER");
  if (peer.isEmpty())
    return; // live traffic only, as before

  m_backfilled = true;

  // Live relay traffic is otherwise all this app ever sees, so everything
  // posted before we subscribed — or while we had no mesh peers to relay to us
  // — is simply lost. A store service peer still holds it. Replay it through
  // the same decode/emit path as live messages so the view's de-dupe treats it
  // as a late arrival and nothing shows up twice.
  QJsonObject query{
      {"requestId", newId()},
      {"includeData", true},
      {"paginationForward", false},
      {"paginationLimit", kStoreBackfillLimit},
      {"contentTopics", QJsonArray{kTopic}},
      // A string, not a number: ns since the epoch exceeds what a JSON double
      // holds exactly, and the store API accepts either form.
      {"timeStart", QString::number(nowNs() - kStoreBackfillWindowNs)},
  };

  LogosResult r = modules().delivery_module.storeQuery(
      QString::fromUtf8(QJsonDocument(query).toJson(QJsonDocument::Compact)),
      peer, kStoreQueryTimeoutMs);
  if (!r.success) {
    logEvent("store backfill failed: " + r.getError().toStdString());
    return;
  }

  const QJsonDocument doc = QJsonDocument::fromJson(r.getString().toUtf8());
  const QJsonArray messages =
      doc.object().value(QStringLiteral("messages")).toArray();

  int replayed = 0;
  for (const QJsonValue &value : messages) {
    const QJsonObject message =
        value.toObject().value(QStringLiteral("message")).toObject();
    // The kernel's StoreQueryResponseHex wraps a WakuMessage, whose payload is
    // base64 (what the send path encodes) and whose timestamp is ns since the
    // epoch, as a number or a string depending on the build.
    const QByteArray payload = QByteArray::fromBase64(
        message.value(QStringLiteral("payload")).toString().toUtf8());
    const QJsonValue ts = message.value(QStringLiteral("timestamp"));

    ForumMessage msg;
    if (payload.isEmpty() || !decodeForumMessage(payload, msg))
      continue; // not one of ours, or a shape this build can't read
    emitForumMessage(msg, ts.isString() ? ts.toString().toLongLong()
                                        : static_cast<qint64>(ts.toDouble()));
    ++replayed;
  }

  // Log both counts: a non-zero fetch that replays nothing means the response
  // shape has moved (storeQuery is explicitly "use at your own risk", backed by
  // a kernel API that can change without a deprecation cycle), which is worth
  // telling apart from an empty store.
  logEvent("store backfill: replayed " + std::to_string(replayed) + " of " +
           std::to_string(messages.size()) + " message(s) from " +
           peer.toStdString());
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
  //
  // EXAMPLE_FORUM_INSTANCE suffixes whichever path we land on. The standalone
  // `nix run` launcher sets no LOGOS_USER_DIR, so two instances started that
  // way otherwise share one identity dir — one credential, one accounts.json,
  // both processes writing it — and post as the same account. Since every
  // account is a key under the credential, a distinct dir means a distinct
  // credential and so a distinct keystore_signer namespace: separate accounts,
  // no coordination needed.
  const QString instance = qEnvironmentVariable("EXAMPLE_FORUM_INSTANCE");
  const QString suffix =
      instance.isEmpty() ? QString() : QStringLiteral("-") + instance;

  const QString userDir = qEnvironmentVariable("LOGOS_USER_DIR");
  if (!userDir.isEmpty())
    return userDir + QStringLiteral("/module_data/example_forum/identity") +
           suffix;

  // Default launch (no --user-dir): AppDataLocation is keyed off the *host*
  // process's org/app name (the ui-host, not this plugin), so namespace under
  // it by module name to avoid colliding with any other Logos module's local
  // data. This path doesn't track the keystore's data tree either, but
  // loadAccounts() reconciles against the keystore, so a mismatch costs the
  // account labels rather than every publish().
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
         QStringLiteral("/example_forum/identity") + suffix;
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

  if (!isContextReady() || !nodeReady())
    return QStringLiteral("Node not ready");
  if (m_keyId.isEmpty())
    return QStringLiteral("Identity not ready");

  // Sign the canonical payload (Keccak-256, matching go-wallet-sdk's
  // go-ethereum-derived signing convention) before encoding — author/sig must
  // be set on msg itself so encodeForumMessage() carries them on the wire.
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

  LogosResult r = modules().delivery_module.send(kTopic, encodeForumMessage(msg));
  if (!r.success) {
    logEvent("send failed: " + r.getError().toStdString());
    return r.getError();
  }
  const QString requestId = r.getString();
  m_pendingSends.insert(requestId, msg.id);
  logEvent("published " + msg.type.toStdString() + " id=" + msg.id.toStdString() +
           ", requestId=" + requestId.toStdString());

  // Local echo — the relay won't loop our own message back, so surface it now.
  // The id lets the QML view de-dupe if the network ever does echo it.
  emitForumMessage(msg, nowNs());
  // ...and immediately mark it unconfirmed. A successful send() means the
  // module accepted the message locally and nothing more, so on its own the
  // echo above would render a post that never left the machine exactly like a
  // delivered one. settleSend() resolves this from the delivery events.
  emit messageStateChanged(msg.id, QStringLiteral("pending"), QString());
  return QString(); // empty == success
}

void ExampleForumBackend::emitForumMessage(const ForumMessage &msg,
                                           qint64 timestamp) {
  if (msg.type == QLatin1String("topic"))
    emit topicReceived(msg.id, msg.title, msg.body, msg.author, timestamp);
  else if (msg.type == QLatin1String("reply"))
    emit replyReceived(msg.id, msg.topicId, msg.body, msg.author, timestamp);
}
