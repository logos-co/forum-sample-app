#include "forum_message.h"

#include <QCryptographicHash>
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
