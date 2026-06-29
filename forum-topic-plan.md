# Plan — forum-topic functionality in `bcast-ui`

Scope: **`bcast-ui` only.** No changes to `delivery_module`, the dependency
wiring, or the hard-coded delivery topic `kTopic` (the single forum channel).

## Idea

Messages currently sent as raw UTF-8 text over `kTopic` become a small **JSON
envelope** carrying metadata (message type + threading ids), so the one channel
can carry both **topic creations** and **replies**. A "single forum" = everything
published on `kTopic`, disambiguated by the envelope.

> **On "metadata":** the message *type* alone can't thread a reply to its topic,
> so the necessary metadata is `type` **plus `id` / `topicId`**. Those are
> included below even though only `type` was named in the request.

## 1. Message envelope + encode/decode (the metadata layer)

New file `src/forum_message.h` (+ optional `src/forum_message.cpp`) — a struct and
two functions that own the wire format, isolated from both transport and UI.

```cpp
struct ForumMessage {
  int     version = 1;
  QString type;      // "topic" | "reply"
  QString id;        // unique msg id (for a topic, this IS the topic id)
  QString topicId;   // replies only: the topic.id being replied to
  QString title;     // topics only
  QString body;      // both
};

QByteArray encodeForumMessage(const ForumMessage&);              // -> compact JSON, UTF-8
bool       decodeForumMessage(const QByteArray&, ForumMessage&); // false if not valid forum JSON
```

Wire format (UTF-8 JSON, replaces the current raw-text payload):

```json
{ "v":1, "type":"topic", "id":"<uuid>", "title":"…", "body":"…" }
{ "v":1, "type":"reply", "id":"<uuid>", "topicId":"<uuid>", "body":"…" }
```

`decode` returns `false` for malformed JSON, unknown `type`, or missing required
fields (a topic needs `title`; a reply needs `topicId`) — so any non-forum traffic
on the channel is safely ignored.

## 2. `.rep` contract — `src/broadcast_app.rep`

Replace the single `sendMessage` slot and `messageReceived` signal with
forum-aware ones; keep the `status` / `nodeReady` / `topic` PROPs.

```
SLOT(QString createTopic(QString title, QString body))
SLOT(QString replyToTopic(QString topicId, QString body))

SIGNAL(topicReceived(QString id, QString title, QString body, qint64 timestamp))
SIGNAL(replyReceived(QString id, QString topicId, QString body, qint64 timestamp))
```

Two typed signals (vs one signal with a `type` arg) keep the QML mapping clean.
Slots return `""` on success / an error string on failure — same convention as the
current `sendMessage`.

## 3. Backend — `src/broadcast_app_backend.{h,cpp}`

- **Receive:** in the existing `delivery_module.on("messageReceived", …)` handler,
  `decodeForumMessage(data[2].toByteArray(), …)`; on success
  `emit topicReceived(...)` or `emit replyReceived(...)`; on failure log + ignore.
- **`createTopic(title, body)`:** guard `nodeReady`; build
  `ForumMessage{type:"topic", id: QUuid::createUuid()…, title, body}`;
  `send(kTopic, encodeForumMessage(m))`.
- **`replyToTopic(topicId, body)`:** same, with `type:"reply"` and `topicId` set.
- **Local echo:** on send success, immediately `emit topicReceived/replyReceived`
  with the **same `id`** so the author sees their own post (Waku relay won't loop
  it back). The id makes QML dedupe idempotent if the network ever does echo.
- Remove `sendMessage`; add `#include <QUuid>` and `#include "forum_message.h"`.

## 4. QML — `src/qml/Main.qml`

Replace the flat message list with a **master-detail forum** (recommended layout):

- **New Topic composer:** title + body fields + "Create" button (enabled on
  `nodeReady`).
- **Topic list (master):** rows showing title, body snippet, reply count, time;
  click selects a topic.
- **Thread view (detail):** selected topic's title/body + its replies + a reply
  composer.
- **Models:** flat `topicsModel` `{ id, title, body, ts }` and `repliesModel`
  `{ id, topicId, body, ts }`; the thread view filters replies by
  `selectedTopicId`. `Connections` on `backend` route
  `onTopicReceived` / `onReplyReceived` into the models, **deduping by `id`**.

*Alternative:* a single stacked column with inline replies under each topic —
simpler but less forum-like. Default is master-detail.

## 5. Build wiring — `CMakeLists.txt`

Add `src/forum_message.h` (and `.cpp` if split) to `SOURCES`. No `metadata.json`
or `flake.nix` changes — `delivery_module` is already a dependency.

## Decisions & assumptions

- Forum **replaces** plain-text broadcast on this app; all traffic on `kTopic` is
  the JSON envelope.
- **Orphan replies** (a reply whose topic wasn't received) are retained and
  surface if/when the topic arrives.
- Self/echo de-duplication is by message `id`.

## Out of scope

- Persistence / history — no Waku store query, so late joiners see only messages
  received while running.
- Author identity / auth / signatures.
- Multiple forums, topic edit/delete, pagination, rich text/attachments.

## Verification

- `nix build` in `bcast-ui` (the real check; IDE/clangd errors here are the known
  false positives — Qt headers aren't on clangd's path outside the nix build).
- Manual end-to-end: two `nix run` instances → create a topic in A, see it in B;
  reply in B, see it in A.
- Optional: a round-trip unit check for `encode`/`decode` (no test harness exists
  in `bcast-ui` today, so this would be net-new).

## Files touched (all under `bcast-ui/`)

| File | Change |
|---|---|
| `src/forum_message.h` (+ `.cpp`?) | **new** — `ForumMessage` + encode/decode |
| `src/broadcast_app.rep` | new slots/signals; drop `sendMessage`/`messageReceived` |
| `src/broadcast_app_backend.h` | declare `createTopic`/`replyToTopic`; includes |
| `src/broadcast_app_backend.cpp` | encode on send, decode on receive, local echo, QUuid |
| `src/qml/Main.qml` | master-detail forum UI + topics/replies models |
| `CMakeLists.txt` | add `forum_message.*` to `SOURCES` |
