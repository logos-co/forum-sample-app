---
name: use-delivery-module
description: Use the Logos delivery_module (liblogosdelivery / Waku messaging) from another module — bootstrap a node, subscribe to a content topic, send a payload, and handle the message events. Covers the createNode config, the synchronous-call + async-event model, and the plain-text send/receive recipe. Use when a module needs to send/receive messages over the Logos messaging network (e.g. bcast-ui broadcasting on a topic).
---

# Use the delivery_module

`delivery_module` wraps liblogosdelivery (Waku-based messaging) as a Logos core
module. A consumer **bootstraps a node**, then **subscribes** to content topics
and **sends** payloads; delivery results and inbound messages arrive as **async
events**. This repo pins it to **v0.1.3** (`repos/logos-delivery-module`,
`metadata.json#version` `0.1.3`).

This is a concrete instance of [use-another-module](../use-another-module/SKILL.md)
— read that for the dependency-wiring mechanics. The canonical usage reference is
[repos/logos-delivery-demo](../../repos/logos-delivery-demo) (a ui_qml demo) and
the full API in [delivery_module_plugin.h](../../repos/logos-delivery-module/src/delivery_module_plugin.h).
A worked universal-ui_qml implementation lives in [bcast-ui](../../bcast-ui/src/broadcast_app_backend.cpp).

## Declare the dependency

```json
// metadata.json
"dependencies": ["delivery_module"]
```
```nix
// flake.nix
delivery_module.url = "github:logos-co/logos-delivery-module/v0.1.3";
```
The module name is `delivery_module` in both places. Then
`nix flake lock && git add . && nix build -L`.

## Lifecycle (all calls synchronous; return `LogosResult`)

```
createNode(cfgJson)  →  start()  →  subscribe(topic) / send(topic, payload) / unsubscribe(topic)  →  stop()
```

`LogosResult` (Qt consumer) / `StdLogosResult` (core consumer): check `.success`,
read `.getError()` on failure, `.getString()` for the value (e.g. `send`'s
request id). `createNode` is called **once per node**; subsequent calls fail.

### `createNode` config — flat JSON of WakuNodeConf keys

Only non-default keys are needed. A `preset` auto-populates cluster id, entry
nodes, sharding and RLN. Minimal, network-ready:

```json
{ "logLevel": "INFO", "mode": "Core", "preset": "logos.test" }
```

| Key | Values | Notes |
|---|---|---|
| `preset` | `logos.test` (default fleet), `logos.dev`, `twn` | picks cluster/bootstrap/shards |
| `mode` | `Core` (full relay), `Edge` (light), `noMode` | |
| `logLevel` | `TRACE`/`DEBUG`/`INFO`/`WARN` | |

**Omit ports.** Unspecified ports default to `0`, so the OS assigns free ones and
multiple instances on one machine coexist without collisions (the basis for
running two app instances and messaging between them).

## Content topics

Use a LIP-23 content topic, e.g. `"/myapp/1/messages/proto"`
([LIP-23](https://lip.logos.co/messaging/informational/23/topics.html)). For a
broadcast/shared channel, hard-code one topic that every instance subscribes and
sends to.

## Send / receive **plain text**

`send`'s payload is **raw bytes**, and `messageReceived` delivers raw bytes — not
text. For a plain-text app, encode/decode UTF-8 yourself (the demo instead uses
hex; that's a UI choice, not a requirement):

```cpp
// SEND plain text:
QByteArray payload = text.toUtf8();
LogosResult r = modules().delivery_module.send(topic, payload);   // .getString() == request id

// RECEIVE plain text (inside the messageReceived handler, see Events):
const QByteArray bytes = data.at(2).toByteArray();
const QString text = QString::fromUtf8(bytes);
```

A Qt consumer's `send` takes `(QString topic, QByteArray payload)`; a core/std
consumer's takes `(const std::string&, const std::vector<uint8_t>&)`.

## Events (subscribe via `.on("name", cb)`; args are positional in a `QVariantList`)

Wire these **before** `start()`. For v0.1.3, the Qt-marshalled positions are:

| Event | `data[…]` positions |
|---|---|
| `messageReceived` | `[0]` messageHash · `[1]` contentTopic · `[2]` payload **(QByteArray, raw bytes)** · `[3]` timestamp **(qint64, ns since epoch)** |
| `messageSent` | `[0]` requestId · `[1]` messageHash · `[2]` timestamp (qint64) — message confirmed by network |
| `messagePropagated` | `[0]` requestId · `[1]` messageHash · `[2]` timestamp — reached network, not yet validated |
| `messageError` | `[0]` requestId · `[1]` messageHash · `[2]` error · `[3]` timestamp |
| `connectionStateChanged` | `[0]` status (`Connected`/`PartiallyConnected`/`Disconnected`) · `[1]` timestamp |

```cpp
modules().delivery_module.on("messageReceived", [this](const QVariantList& data){
    if (data.size() < 4) return;
    const QString text = QString::fromUtf8(data.at(2).toByteArray());
    emit messageReceived(text, data.at(3).toLongLong());      // forward to QML, etc.
});
```

> **Version caveat.** The module's README "Events" section describes the
> `messageReceived` payload as a base64 *string* and timestamps as nanosecond
> *strings*. The **v0.1.3 demo code** (the tested reference) treats the payload as
> a `QByteArray` and timestamps as `qint64`. Trust the demo/`bcast-ui` code for
> v0.1.3; re-check positions/types if you bump the pin.

## Node info

The peer id and lib version are only exposed via `getNodeInfo`:
`getNodeInfo("MyPeerId")` (poll ~3s — it can change) and `getNodeInfo("Version")`
(fixed; read once). Both return the value via `LogosResult.getString()`.

## Shared-singleton bootstrap (important)

`delivery_module` is a **singleton shared across all Basecamp apps**
([module-considerations.md](../../module-considerations.md)). Another app may have
already created and started the node, so your `createNode`/`start` will fail with
"already created". Don't abort — fall through to `subscribe` so you still receive
on your topic:

```cpp
LogosResult created = modules().delivery_module.createNode(cfgJson);
if (created.success) {
    modules().delivery_module.start();        // check its result too
} else {
    // node likely already running (shared singleton) — proceed to subscribe
}
LogosResult sub = modules().delivery_module.subscribe(topic);
```

## Run two instances (manual end-to-end test)

Because ports default to `0`, two instances coexist. Subscribe both to the same
topic, send from one, and the other fires `messageReceived`:

```bash
# standalone harness (single-module), two terminals:
cd bcast-ui && nix run        # terminal A
cd bcast-ui && nix run        # terminal B
# or two Basecamp instances with isolated state:
./result/bin/LogosBasecamp --user-dir /tmp/bc-a &
./result/bin/LogosBasecamp --user-dir /tmp/bc-b &
```

Needs the live `logos.test` network. Bootstrapping is synchronous and can block
briefly — in a UI backend, defer it off `onContextReady()` (e.g.
`QTimer::singleShot(0, …)`) so the QML replica comes up promptly.

## Gotchas

- **Bytes, not text** on both `send` and `messageReceived` — encode/decode UTF-8
  for plain text, or you'll publish/render mojibake.
- **`createNode` once** — guard against the shared-singleton case above instead
  of assuming a fresh node.
- **Subscribe before `start()`**, and wire event `.on(...)` handlers before
  triggering sends, or you'll miss early events.
- **Send is async after return** — `send` returning a request id only means it was
  accepted locally; track real delivery via `messageSent` / `messagePropagated` /
  `messageError`.
