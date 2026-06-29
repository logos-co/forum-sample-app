#include "forum_message.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLatin1String>

namespace {
constexpr char kTypeTopic[] = "topic";
constexpr char kTypeReply[] = "reply";
} // namespace

QByteArray encodeForumMessage(const ForumMessage &msg) {
  QJsonObject obj{
      {"v", msg.version},
      {"type", msg.type},
      {"id", msg.id},
      {"body", msg.body},
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
