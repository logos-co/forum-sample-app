#include "broadcast_app_backend.h"

#include <iostream>

#include <QByteArray>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLatin1String>
#include <QTimer>
#include <QUuid>
#include <QVariantList>

// Generated umbrella: LogosModules (behind modules()) built from
// metadata.json#dependencies — here the typed `delivery_module` wrapper and its
// typed event accessors. logos_types.h provides LogosResult.
#include "logos_sdk.h"
#include "logos_types.h"

namespace {
// One consistently-tagged line per lifecycle hook / delivery event so the
// backend's activity is easy to spot (and grep) in the host's stderr stream.
void logEvent(const std::string &what) {
  std::cerr << "[broadcast_app backend] " << what << std::endl;
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
const QString BroadcastAppBackend::kTopic =
    QStringLiteral("/broadcast-app/1/forum/proto");

BroadcastAppBackend::BroadcastAppBackend() {
  // Runs in the ui-host process before the context is wired.
  logEvent("ctor — backend constructed (context not yet wired)");
}

BroadcastAppBackend::~BroadcastAppBackend() {
  logEvent("dtor — backend destroyed");
}

void BroadcastAppBackend::onContextReady() {
  logEvent("onContextReady — context wired, scheduling node bootstrap");
  setTopic(kTopic);

  // createNode()/start() are synchronous and can block for a moment. Defer the
  // bootstrap to the next event-loop turn so onContextReady() returns and the
  // QML view's replica can reach its Valid state promptly. modules() stays live.
  QTimer::singleShot(0, [this]() { bootstrap(); });
}

void BroadcastAppBackend::bootstrap() {
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

QString BroadcastAppBackend::createTopic(QString title, QString body) {
  if (title.isEmpty())
    return QStringLiteral("A topic needs a title");

  ForumMessage msg;
  msg.type = QStringLiteral("topic");
  msg.id = newId();
  msg.title = title;
  msg.body = body;
  return publish(msg);
}

QString BroadcastAppBackend::replyToTopic(QString topicId, QString body) {
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

QString BroadcastAppBackend::publish(const ForumMessage &msg) {
  if (!isContextReady() || !nodeReady())
    return QStringLiteral("Node not ready");

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

void BroadcastAppBackend::emitForumMessage(const ForumMessage &msg,
                                           qint64 timestamp) {
  if (msg.type == QLatin1String("topic"))
    emit topicReceived(msg.id, msg.title, msg.body, timestamp);
  else if (msg.type == QLatin1String("reply"))
    emit replyReceived(msg.id, msg.topicId, msg.body, timestamp);
}
