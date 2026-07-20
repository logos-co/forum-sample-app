#pragma once

#include <QByteArray>
#include <QString>

/**
 * @brief A single forum message carried over the broadcast topic.
 *
 * All forum traffic — topic creations and replies — shares the one delivery
 * content topic (ExampleForumBackend::kTopic). Each message is a small JSON
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
  QString author;  ///< signer's address (0x…), set by publish() before sending
  QString sig;     ///< signature over forumMessageSigningBytes(msg), set by publish() before sending
};

// The bytes that get hashed and signed — a frozen canonical form independent
// of JSON field order, mirroring topicIdFor()'s approach below. Deliberately
// excludes `author`/`sig` themselves (a signature can't cover its own field).
// Both signing (publish) and, once a verify path exists, checking a received
// message must hash exactly these bytes.
QByteArray forumMessageSigningBytes(const ForumMessage &msg);

// Derive a topic's id from its title, as a content hash (SHA-256 hex over a
// frozen canonical form of the title). The id is therefore deterministic and
// reproducible: anyone with the exact title can recompute it, which is what lets
// a user restore a topic they only heard about through its replies — paste the
// shared title and the hash proves it's the right one. Title-only by design, so
// two topics sharing a title share an id (the same thread); the body is not part
// of the id and cannot be recovered from it.
QString topicIdFor(const QString &title);

// Serialise to a compact UTF-8 JSON envelope, ready for delivery_module.send().
QByteArray encodeForumMessage(const ForumMessage &msg);

// Parse a payload produced by encodeForumMessage(). Returns false (leaving `out`
// untouched) when the bytes are not a well-formed forum message — malformed
// JSON, an unknown `type`, or a missing required field (topics need a `title`,
// replies need a `topicId`) — so non-forum traffic on the channel is ignored.
bool decodeForumMessage(const QByteArray &bytes, ForumMessage &out);
