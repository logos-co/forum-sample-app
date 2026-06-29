#pragma once

#include <QByteArray>
#include <QString>

/**
 * @brief A single forum message carried over the broadcast topic.
 *
 * All forum traffic — topic creations and replies — shares the one delivery
 * content topic (BroadcastAppBackend::kTopic). Each message is a small JSON
 * envelope whose `type` distinguishes a new topic from a reply, and whose
 * `id`/`topicId` thread replies under their topic.
 *
 * encodeForumMessage() / decodeForumMessage() own that wire format so the
 * backend, the delivery transport, and the QML view stay decoupled from it.
 */
struct ForumMessage {
  int version = 1;
  QString type;    ///< "topic" | "reply"
  QString id;      ///< unique message id; for a topic this is also the topic id
  QString topicId; ///< replies only: the topic.id this reply belongs to
  QString title;   ///< topics only
  QString body;    ///< both
};

// Serialise to a compact UTF-8 JSON envelope, ready for delivery_module.send().
QByteArray encodeForumMessage(const ForumMessage &msg);

// Parse a payload produced by encodeForumMessage(). Returns false (leaving `out`
// untouched) when the bytes are not a well-formed forum message — malformed
// JSON, an unknown `type`, or a missing required field (topics need a `title`,
// replies need a `topicId`) — so non-forum traffic on the channel is ignored.
bool decodeForumMessage(const QByteArray &bytes, ForumMessage &out);
