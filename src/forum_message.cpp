#include "forum_message.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLatin1String>

namespace {
constexpr char kTypeTopic[] = "topic";
constexpr char kTypeReply[] = "reply";
} // namespace

QString topicIdFor(const QString &title) {
  // Frozen canonical form: NFC-normalised, outer whitespace trimmed, behind a
  // version tag. Copy/paste of a shared title survives trailing spaces and
  // encoding differences, but the match is otherwise exact (a hash is not
  // fuzzy). Bump the "v1" tag if this canonicalisation ever changes — doing so
  // changes every id.
  const QString canonical =
      QStringLiteral("v1\n") +
      title.normalized(QString::NormalizationForm_C).trimmed();
  return QString::fromLatin1(
      QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha256)
          .toHex());
}

QByteArray forumMessageSigningBytes(const ForumMessage &msg) {
  // Same frozen-canonical-form technique as topicIdFor() above, so the bytes
  // that get hashed don't depend on JSON key order/whitespace and stay stable
  // even if the wire envelope's JSON shape changes later. "v1" tags the form
  // itself, distinct from the envelope's own `msg.version`.
  QString canonical = QStringLiteral("v1\n") + msg.type + QLatin1Char('\n') + msg.id;
  if (msg.type == QLatin1String(kTypeTopic))
    canonical += QLatin1Char('\n') + msg.title;
  else if (msg.type == QLatin1String(kTypeReply))
    canonical += QLatin1Char('\n') + msg.topicId;
  canonical += QLatin1Char('\n') + msg.body;
  return canonical.toUtf8();
}

QByteArray encodeForumMessage(const ForumMessage &msg) {
  QJsonObject obj{
      {"v", msg.version},
      {"type", msg.type},
      {"id", msg.id},
      {"body", msg.body},
      {"author", msg.author},
      {"sig", msg.sig},
  };
  // Only carry the field that's meaningful for the type, to keep envelopes lean.
  if (msg.type == QLatin1String(kTypeTopic))
    obj.insert("title", msg.title);
  else if (msg.type == QLatin1String(kTypeReply))
    obj.insert("topicId", msg.topicId);

  return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

bool decodeForumMessage(const QByteArray &bytes, ForumMessage &out) {
  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
    return false;

  const QJsonObject obj = doc.object();

  ForumMessage m;
  m.version = obj.value("v").toInt(1);
  m.type = obj.value("type").toString();
  m.id = obj.value("id").toString();
  m.body = obj.value("body").toString();
  // Absent on messages from a client that predates signing (or a malicious
  // one) — left empty rather than rejected, since this client has no way to
  // verify a signature yet either way. See forumMessageSigningBytes().
  m.author = obj.value("author").toString();
  m.sig = obj.value("sig").toString();
  if (m.id.isEmpty())
    return false;

  if (m.type == QLatin1String(kTypeTopic)) {
    m.title = obj.value("title").toString();
    if (m.title.isEmpty())
      return false; // a topic must have a title
  } else if (m.type == QLatin1String(kTypeReply)) {
    m.topicId = obj.value("topicId").toString();
    if (m.topicId.isEmpty())
      return false; // a reply must reference its topic
  } else {
    return false; // unknown / missing type
  }

  out = m;
  return true;
}
