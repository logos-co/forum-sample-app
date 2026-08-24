#include "example_forum_backend.h"

#include <iostream>

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QIODevice>
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

  // Node health: surface connectionStateChanged (Connected / PartiallyConnected
  // / Disconnected) as our status string once the node is up.
  modules().delivery_module.on(
      "connectionStateChanged", [this](const QVariantList &data) {
        if (data.isEmpty())
          return;
        if (nodeReady())
          setStatus(data.at(0).toString());
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

  // --- Open/create this install's signing identity ---------------------------
  // Independent of the delivery node below; publish() gates on both being
  // ready (nodeReady() and a non-empty m_myAddress).
  ensureIdentity();

  // --- Create + start the node against the logos.test fleet -----------------
  // No ports specified: delivery_module defaults them to 0, so the OS assigns
  // free ports and two instances on one machine don't collide.
  const QJsonObject cfg{
      {"logLevel", "INFO"},
      {"mode", "Core"},
      {"preset", "logos.test"},
  };
  const QString cfgJson =
      QString::fromUtf8(QJsonDocument(cfg).toJson(QJsonDocument::Compact));

  LogosResult created = modules().delivery_module.createNode(cfgJson);
  if (created.success) {
    logEvent("createNode succeeded, starting node");
    LogosResult started = modules().delivery_module.start();
    if (!started.success)
      logEvent("start failed: " + started.getError().toStdString());
  } else {
    // delivery_module is a singleton shared across Basecamp apps, so another
    // app may have already created and started the node. createNode then fails;
    // proceed to subscribe so we still receive on our topic.
    logEvent("createNode failed (node may already be running): " +
             created.getError().toStdString());
  }

  LogosResult subscribed = modules().delivery_module.subscribe(kTopic);
  if (!subscribed.success) {
    setStatus(QStringLiteral("subscribe failed: %1").arg(subscribed.getError()));
    logEvent("subscribe failed: " + subscribed.getError().toStdString());
    return;
  }

  setNodeReady(true);
  setStatus(QStringLiteral("Connected to forum on %1").arg(kTopic));
  logEvent("node ready — forum on " + kTopic.toStdString());
}

QString ExampleForumBackend::identityDir() const {
  // Basecamp's --user-dir gives an instance its own data tree (plugins,
  // modules, module_data, logs) and exports it to every child process — this
  // backend's ui-host included — as LOGOS_USER_DIR. keystore_signer keeps its
  // keys inside that tree (<user dir>/module_data/keystore_signer/<instance>),
  // so our credential + key id have to live there too: an identity stored
  // outside it is reloaded against a keystore that has never seen its key, and
  // every sign() then fails. Sit next to the keystore we're bound to.
  const QString userDir = qEnvironmentVariable("LOGOS_USER_DIR");
  if (!userDir.isEmpty())
    return userDir + QStringLiteral("/module_data/example_forum/identity");

  // Default launch (no --user-dir): AppDataLocation is keyed off the *host*
  // process's org/app name (the ui-host, not this plugin), so namespace under
  // it by module name to avoid colliding with any other Logos module's local
  // data. This path doesn't track the keystore's data tree either, but
  // ensureIdentity() re-mints when the keystore doesn't know the key, so a
  // mismatch costs an identity rather than every publish().
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
         QStringLiteral("/example_forum/identity");
}

void ExampleForumBackend::ensureIdentity() {
  const QString dir = identityDir();
  QDir().mkpath(dir);
  const QString credentialFile = dir + QStringLiteral("/keystore_signer_credential");
  const QString keyIdFile = dir + QStringLiteral("/key_id");

  auto readFile = [](const QString &path) -> QString {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
      return QString();
    return QString::fromUtf8(f.readAll()).trimmed();
  };
  auto writeFile = [](const QString &path, const QString &contents) -> bool {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
      return false;
    QTextStream out(&f);
    out << contents;
    return true;
  };

  // keystore-signer-module's calls use logos::CallError out-params (same pattern
  // as the previous accounts_module, but with different API: sign() returns raw
  // bytes instead of a hex string, and isolation is per-caller secret instead of
  // per-keystore-handle).
  logos::CallError err;

  QByteArray credentialBytes = QByteArray::fromHex(readFile(credentialFile).toLatin1());
  QString keyId = readFile(keyIdFile);

  // A persisted identity is only usable while keystore_signer still holds the
  // key it names, so check before trusting it — the keystore's storage lives
  // under the Basecamp user dir (see identityDir()), so it can be a fresh,
  // empty store while these files still name a key minted against an older
  // one. publicKey() returns empty bytes for a key id the credential's
  // namespace doesn't contain; that's the stale case, and re-minting beats
  // reloading an identity whose every publish() would fail to sign.
  bool identityUsable = false;
  if (credentialBytes.size() != kCredentialBytes || keyId.isEmpty()) {
    // Nothing (or nothing complete) persisted here yet — the first-run path.
    if (!credentialBytes.isEmpty() || !keyId.isEmpty())
      logEvent("identity files in " + dir.toStdString() +
               " are incomplete or corrupt — minting a new identity");
  } else if (modules()
                 .keystore_signer.publicKey(credentialBytes, keyId, &err)
                 .toByteArray()
                 .isEmpty()) {
    logEvent("keystore_signer holds no key " + keyId.toStdString() +
             " for this credential — identity in " + dir.toStdString() +
             " is stale, minting a new one");
  } else {
    identityUsable = true;
  }

  if (!identityUsable) {
    // Mint a fresh identity: a random 256-bit credential (32 bytes) plus the
    // key created under it. The credential isn't a user secret, but isolation
    // plumbing (see ensureIdentity()'s header doc comment).
    credentialBytes = QByteArray(kCredentialBytes, 0);
    for (int i = 0; i < credentialBytes.size(); ++i)
      credentialBytes[i] = static_cast<char>(QRandomGenerator::global()->generate() & 0xFF);

    keyId = modules().keystore_signer.createKey(credentialBytes,
                                                QStringLiteral("secp256k1"), &err);
    if (keyId.isEmpty()) {
      logEvent("createKey failed: " + err.message);
      return;
    }
    if (!writeFile(credentialFile, QString::fromLatin1(credentialBytes.toHex())) ||
        !writeFile(keyIdFile, keyId)) {
      logEvent("failed to persist identity to " + dir.toStdString());
      return;
    }
    logEvent("minted new forum identity " + keyId.toStdString());
  }

  m_keyId = keyId;
  m_credential = credentialBytes;
  setMyAddress(keyId);
  logEvent("identity ready — signing as " + keyId.toStdString());
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
  // workaround is needed. The sign() call returns raw signature bytes (wrapped in QVariant).
  logos::CallError err;
  const QVariant sigResult = modules().keystore_signer.sign(m_credential, m_keyId, hash, &err);
  const QByteArray sigBytes = sigResult.toByteArray();
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
  logEvent("published " + msg.type.toStdString() + " id=" + msg.id.toStdString() +
           ", requestId=" + r.getString().toStdString());

  // Local echo — the relay won't loop our own message back, so surface it now.
  // The id lets the QML view de-dupe if the network ever does echo it.
  emitForumMessage(msg, nowNs());
  return QString(); // empty == success
}

void ExampleForumBackend::emitForumMessage(const ForumMessage &msg,
                                           qint64 timestamp) {
  if (msg.type == QLatin1String("topic"))
    emit topicReceived(msg.id, msg.title, msg.body, msg.author, timestamp);
  else if (msg.type == QLatin1String("reply"))
    emit replyReceived(msg.id, msg.topicId, msg.body, msg.author, timestamp);
}
