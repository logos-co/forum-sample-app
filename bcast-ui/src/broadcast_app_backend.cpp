#include "broadcast_app_backend.h"

#include <iostream>

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
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
} // namespace

// A LIP-23 content topic (https://lip.logos.co/messaging/informational/23/topics.html).
// Hard-coded so every instance of this app talks on the same channel.
const QString BroadcastAppBackend::kTopic =
    QStringLiteral("/broadcast-app/1/messages/proto");

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

  // Inbound messages on any subscribed topic. data[2] is the raw payload bytes;
  // we publish UTF-8 text, so decode it back to a QString. data[3] is the
  // message timestamp (qint64, ns since epoch).
  modules().delivery_module.on(
      "messageReceived", [this](const QVariantList &data) {
        if (data.size() < 4)
          return;
        const QByteArray payload = data.at(2).toByteArray();
        const QString text = QString::fromUtf8(payload);
        logEvent("messageReceived on " + data.at(1).toString().toStdString() +
                 " -> \"" + text.toStdString() + "\"");
        emit messageReceived(text, data.at(3).toLongLong());
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
  setStatus(QStringLiteral("Listening on %1").arg(kTopic));
  logEvent("node ready — listening on " + kTopic.toStdString());
}

QString BroadcastAppBackend::sendMessage(QString text) {
  if (!isContextReady() || !nodeReady())
    return QStringLiteral("Node not ready");
  if (text.isEmpty())
    return QStringLiteral("Nothing to send");

  // delivery_module.send takes raw bytes; encode the plain text as UTF-8 so the
  // receiver can decode it back with QString::fromUtf8 (see the event handler).
  const QByteArray payload = text.toUtf8();
  LogosResult r = modules().delivery_module.send(kTopic, payload);
  if (!r.success) {
    logEvent("send failed: " + r.getError().toStdString());
    return r.getError();
  }
  logEvent("sent message, requestId=" + r.getString().toStdString());
  return QString(); // empty == success
}
