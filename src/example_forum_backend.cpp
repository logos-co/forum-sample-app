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
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QUuid>
#include <QVariantList>

// Generated umbrella: LogosModules (behind modules()) built from
// metadata.json#dependencies — the typed `delivery_module` and `accounts_module`
// wrappers and their typed event accessors. logos_types.h provides LogosResult
// (delivery_module's return type); logos_call_error.h provides logos::CallError,
// the out-param error accounts_module's synchronous, direct-return calls use
// instead (it has no native LogosResult wrapping — see its own std-typed
// accounts_module_impl.h).
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
  // AppDataLocation is keyed off the *host* process's org/app name (the
  // ui-host, not this plugin), so namespace under it by module name to avoid
  // colliding with any other Logos module's local data.
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
         QStringLiteral("/example_forum/identity");
}

void ExampleForumBackend::ensureIdentity() {
  const QString dir = identityDir();
  QDir().mkpath(dir);
  const QString keystoreDir = dir + QStringLiteral("/keystore");
  const QString passphraseFile = dir + QStringLiteral("/passphrase");
  const QString addressFile = dir + QStringLiteral("/address");

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

  // accounts_module's calls are direct-return (bool / QString), not wrapped in
  // LogosResult like delivery_module's — it takes an optional logos::CallError
  // out-param instead (see logos_call_error.h). Its native accounts_module_impl.h
  // interface has no Result wrapping of its own, and the generated wrapper
  // mirrors that.
  logos::CallError err;

  const bool opened = modules().accounts_module.initKeystore(keystoreDir, 4096, 6, &err);
  if (!opened) {
    logEvent("initKeystore failed: " + err.message);
    return;
  }

  QString passphrase = readFile(passphraseFile);
  QString address = readFile(addressFile);

  if (passphrase.isEmpty() || address.isEmpty()) {
    // First run for this install — mint a fresh identity. The passphrase is
    // not a user secret (see ensureIdentity()'s header comment); it only
    // exists to satisfy accounts_module's keystore API.
    passphrase = QUuid::createUuid().toString(QUuid::WithoutBraces) +
                 QUuid::createUuid().toString(QUuid::WithoutBraces);
    address = modules().accounts_module.keystoreNewAccount(passphrase, &err);
    if (address.isEmpty()) {
      logEvent("keystoreNewAccount failed: " + err.message);
      return;
    }
    if (!writeFile(passphraseFile, passphrase) || !writeFile(addressFile, address)) {
      logEvent("failed to persist identity to " + dir.toStdString());
      return;
    }
    logEvent("minted new forum identity " + address.toStdString());
  }

  // No keystoreUnlock() here — see the doc comment on ensureIdentity() in the
  // header for why that state can't be relied on to survive until publish().
  m_myAddress = address;
  m_passphrase = passphrase;
  setMyAddress(address);
  logEvent("identity ready — signing as " + address.toStdString());
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
           " myAddress=" + (m_myAddress.isEmpty() ? "<empty>" : m_myAddress.toStdString()));

  if (!isContextReady() || !nodeReady())
    return QStringLiteral("Node not ready");
  if (m_myAddress.isEmpty())
    return QStringLiteral("Identity not ready");

  // Sign the canonical payload (Keccak-256, matching go-wallet-sdk's
  // go-ethereum-derived signing convention) before encoding — author/sig must
  // be set on msg itself so encodeForumMessage() carries them on the wire.
  msg.author = m_myAddress;
  const QByteArray hash = QCryptographicHash::hash(forumMessageSigningBytes(msg),
                                                     QCryptographicHash::Keccak_256);
  const QString hashHex = QStringLiteral("0x") + QString::fromLatin1(hash.toHex());

  // Reopen our directory immediately before signing, and sign with the
  // passphrase directly (keystoreSignHashWithPassphrase), rather than relying
  // on a keystoreUnlock() from bootstrap time to have survived — accounts_module
  // shares one keystore handle across every consumer, so anyone else's
  // initKeystore() call since then would have silently locked us out again.
  // See ensureIdentity()'s doc comment in the header.
  logos::CallError err;
  const QString keystoreDir = identityDir() + QStringLiteral("/keystore");
  if (!modules().accounts_module.initKeystore(keystoreDir, 4096, 6, &err)) {
    logEvent("re-initKeystore before sign failed: " + err.message);
    return QString::fromStdString(err.message);
  }
  msg.sig = modules().accounts_module.keystoreSignHashWithPassphrase(
      m_myAddress, m_passphrase, hashHex, &err);
  if (msg.sig.isEmpty()) {
    logEvent("sign failed: " + err.message);
    return QString::fromStdString(err.message);
  }

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
