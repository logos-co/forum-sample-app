#pragma once

#include <QByteArray>
#include <QString>

/**
 * @brief One forum post, in memory.
 *
 * A plain carrier between the backend's signing/publish path, the local store,
 * and the view's signals: `type` distinguishes a new topic from a reply, and
 * `id`/`topicId` thread replies under their topic.
 *
 * It is no longer a wire format. Posts travel as CRDT ops over
 * cloud_data_core (see ExampleForumBackend), which owns their encoding; what
 * survives here is the part this app owns — the canonical bytes a post's
 * signature covers, and the content-addressed derivation of a topic's id.
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

