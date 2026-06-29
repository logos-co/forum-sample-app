# Logos Modules — lifecycle, calls, execution, and UI

A summary distilled from [`repos/logos-basecamp`](repos/logos-basecamp) (the
desktop host application) and [`repos/logos-cpp-sdk`](repos/logos-cpp-sdk) (the
Qt-free base SDK + code generator). Cross-references the
[`logos-view-module-runtime`](repos/logos-view-module-runtime) library that
Basecamp links for UI hosting.

Sources: [basecamp spec](repos/logos-basecamp/docs/spec.md),
[basecamp CLAUDE.md](repos/logos-basecamp/CLAUDE.md),
[cpp-sdk README](repos/logos-cpp-sdk/README.md),
[cpp-sdk docs](repos/logos-cpp-sdk/docs/docs.md),
[generator spec](repos/logos-cpp-sdk/cpp-generator/docs/spec.md),
[view-module-runtime README](repos/logos-view-module-runtime/README.md).

---

## 1. Two kinds of "module"

Basecamp manages two fundamentally different component types. The word "module"
is overloaded, so keep them apart:

| | **Logos Module** (backend) | **UI App / View Module** (frontend) |
|---|---|---|
| What it is | A plugin implementing `PluginInterface`, managed by the Logos runtime `liblogos_core` | A Qt plugin (QML package or C++ `IComponent`) providing a graphical tab, managed by Basecamp |
| Where it runs | Its **own isolated `logos_host` subprocess** | QML view in the **Basecamp process**; C++ backend in an isolated **`ui-host` subprocess** |
| Who loads it | `liblogos_core` C API (`logos_core_load_module*`) | Basecamp via `QPluginLoader` / `QQuickWidget` |
| Has UI? | No — headless background service | Yes — appears as an MDI tab |
| Auth | UUID capability tokens | Calls backends through tokens like anyone else |
| Examples | `package_manager`, `capability_module`, `waku_module` | `package_manager_ui`, `broadcast_app` |

```
┌─ Basecamp Process ───────────────────────────────────────────┐
│  App Shell + Main UI plugin                                   │
│   ├─ Sidebar / MDI / system views                            │
│   ├─ MainUIBackend (+ 3 managers)  ── liblogos_core C API ──┐ │
│   └─ UI App QML view  ── LogosQmlBridge ──┐                  │ │
│  liblogos_core (linked library)           │                  │ │
│   └─ Remote Object Registry + lifecycle ◄─┘──────────────────┘ │
└──────────┬──────────────────────────┬───────────────┬─────────┘
   IPC (local socket / QRO)    IPC               spawn │ (private socket)
     ┌──────▼──────┐  ┌────────▼────────┐        ┌──────▼──────┐
     │ logos_host  │  │ logos_host      │  ...   │ ui-host     │
     │ package_mgr │  │ capability_mod  │        │ <view bknd> │
     └─────────────┘  └─────────────────┘        └─────────────┘
       Logos Modules (separate processes)         View backend (isolated)
```

### Authoring flavors (how a module's code is written)

The SDK generator (`logos-cpp-generator`) and `mkLogosModule.nix` support
several authoring styles, all selected by `metadata.json`:

- **Universal / core module** — a plain C++ impl class inheriting
  `LogosModuleContext` (`logos_module_context.h`). No Qt at the call site;
  `interface: "universal"` makes codegen emit **std**-typed wrappers
  (`std::string`, `LogosMap`, …). All Qt glue is generated around the impl.
- **Provider module** — methods marked with `LOGOS_METHOD`; generated provider
  glue wraps them.
- **Legacy Qt module** — handcrafted `QObject` plugin with `Q_INVOKABLE`
  methods, introspected via Qt's meta-object system. `interface: "qt"` (default).
- **`ui_qml` view module** — a C++ backend (deriving a generated
  `<Foo>SimpleSource` from a `.rep` contract **and** `LogosUiPluginContext`) plus
  a QML `view`. See [§6](#6-ui-modules-qt-frontend--c-backend).

`metadata.json` is the source of truth for `name`, `version`, `type`
(`module` vs `ui_qml`), `interface`, `dependencies`, `view`, and `codegen.rep`.
Example: [bcast-ui/metadata.json](bcast-ui/metadata.json).

---

## 2. Lifecycle of modules

Both kinds share the same state machine — `Discovered → Loading → Running →
Unloading → Discovered` — but the mechanics differ.

### Backend Logos Module lifecycle (managed by liblogos_core)

1. **Discovery** — At startup `liblogos_core` scans the module directories
   (embedded read-only dir + user-writable dir), extracts each plugin's
   `metadata.json`, and populates the known-modules list. The module shows in
   the **Core Modules** tab as available but not loaded. Nothing of the module's
   own code runs yet.
2. **Loading** — `liblogos_core` spawns a dedicated `logos_host` subprocess,
   sends an **auth token** over a local socket, and waits for the module to
   register with the remote object registry. Dependencies load first via
   `logos_core_load_module_with_dependencies()`.
3. **Running** — The module is live in its subprocess, serving methods and
   emitting events over the Logos API. Resource usage (CPU%, memory) is polled
   every ~2s (`logos_core_get_module_stats`).
4. **Unloading** — The host subprocess is terminated, tokens are cleaned up, and
   the module returns to *Discovered*. Unload can cascade to dependents.

Some modules (e.g. `package_manager`, `capability_module`) **auto-load** at
startup.

### Code-level lifecycle hooks (SDK side)

When a provider registers an object (`LogosAPIProvider::registerObject`), the
SDK fires hooks in this order (all optional, SFINAE-wired so non-inheriting
impls compile unchanged):

- **`initLogos(LogosAPI*)`** — called by the provider/`ui-host` if present. The
  generated provider glue's `onInit(LogosAPI*)` override (a) copies the three
  host-injected properties — `modulePath`, `instanceId`,
  `instancePersistencePath` — into the impl, and (b) builds the per-module
  `LogosModules` aggregate and threads its pointer in. The raw `LogosAPI` never
  escapes the provider.
- **`onContextReady()`** — fires **exactly once**, after the context is wired
  and before any method dispatch. This is the module's "start": open files,
  prime caches, start timers. (See `BroadcastAppBackend::onContextReady` starting
  a per-second tick: [bcast-ui/src/broadcast_app_backend.h](bcast-ui/src/broadcast_app_backend.h).)
- **Destruction** — Qt child-destruction order tears things down; the provider
  stops emitting, then widgets/objects, then the C API handle.

### UI App / view module lifecycle (managed by Basecamp)

1. **Discovery** — Basecamp queries the `package_manager` module for installed
   UI Apps; they appear in the **UI Modules** tab.
2. **Loading** — Basecamp loads the plugin into its own process. Any declared
   backend dependencies load first (via liblogos). For a `ui_qml` view module,
   Basecamp also **spawns a `ui-host` subprocess** for the C++ backend (see
   [§6](#6-ui-modules-qt-frontend--c-backend)), creates a sandboxed
   `QQuickWidget`, and adds an MDI tab.
3. **Running** — The widget is displayed; the user interacts; the app calls
   backends via the QML bridge / `LogosAPI`.
4. **Unloading** — Tab removed, widget destroyed, plugin unloaded, `ui-host`
   subprocess stopped. **Backend dependencies stay loaded** (they may be shared
   with other apps).

---

## 3. Calls between modules

All inter-module calls cross a process boundary and are **token-authenticated**.
The SDK (`logos-cpp-sdk/cpp`) hides sockets, tokens, and marshalling.

### The call path

```
caller code
  └─ LogosModules: logos.waku.subscribeTopic()          ← generated typed wrapper
       └─ LogosAPI::getClient("waku")
            └─ LogosAPIClient::invokeRemoteMethod(...)   ← looks up + attaches token
                 └─ LogosAPIConsumer                     ← owns the transport connection
                      └─ [transport]  QRO replica / TCP RPC framing
                           └─ ModuleProxy::callRemoteMethod(token, method, args)
                                └─ validates token vs TokenManager
                                └─ QMetaObject::invokeMethod on the real object
```

Key pieces (all in [cpp-sdk docs](repos/logos-cpp-sdk/docs/docs.md)):

- **`LogosAPI`** — per-module entry point. Owns one `LogosAPIProvider` (to expose
  *this* module) and a cache of `LogosAPIClient`s (to call *others*).
- **`LogosAPIProvider`** — exposes the local object over one host per transport;
  wraps it in a `ModuleProxy`.
- **`ModuleProxy`** — the security gate. Validates the auth token on **every**
  inbound call (invalid/missing → empty `QVariant`), then dispatches via Qt
  meta-object (up to 5 args). Also introspects: `getPluginMethods()`,
  `getPluginEvents()`, `getPluginInterface()` — all filtered views over one
  `getMethods()` call (kept as a single vtable slot for ABI stability).
- **`LogosAPIClient` / `LogosAPIConsumer`** — async client; sync and
  `...Async(callback)` overloads for 0–5 args. Auto-negotiates a per-target
  token by dialing `capability_module` first.
- **`TokenManager`** — thread-safe singleton token store keyed by module name.

> **Security: `informModuleToken` is privileged.** Because `callRemoteMethod`
> authorizes against *any* token in the store, the write path
> (`informModuleToken`) must be gated — it validates the caller's `authToken`
> against the module's seed secret (planted by the host under the
> `core`/`capability_module` keys at init) and fails closed otherwise (finding
> F-002).

### Generated wrappers (the typed surface modules actually use)

`logos-cpp-generator` introspects each dependency plugin and emits
`<module>_api.h/.cpp` plus an umbrella `logos_sdk.h/.cpp` exposing a flat
`LogosModules` struct — one accessor per `metadata.json#dependencies` entry:

```cpp
LogosModules logos(api);
bool ok = logos.chat.initialize();          // typed, no string method names
logos.chat.sendMessage(channel, user, msg); // args marshalled to QVariantList
```

Wrapper signatures follow **this module's** `interface`: `universal` → std types;
`qt`/`provider`/`legacy` → Qt types. `core_manager` is always emitted so apps
can drive `liblogos_core` even though it can't be introspected at build time.

### Events

Events are the other half of a module's API. A universal module declares them in
a `logos_events:` section; codegen supplies the emit bodies and a `.lidl`
descriptor. Consumers get typed `on<EventName>(callback)` accessors (or the
generic `logos.chat.on("name", cb)`). Under the hood every event routes through
`emitEvent → ModuleProxy::eventResponse` (QRO) signal to subscribers.

### From QML (UI Apps)

QML never touches `LogosAPI` directly. It calls through the bridge exposed as
`Logos`:

```qml
logos.callModuleAsync("waku_module", "getStatus", [], function(payload) {
    const r = JSON.parse(payload);   // results are always JSON strings
});
```

`LogosQmlBridge` routes to a regular backend over `LogosAPI` IPC, **or** to a
view module over a private QRO replica if that name was registered via
`setViewModuleSocket`. Prefer `callModuleAsync` — the sync `callModule` blocks
the QML/JS event loop.

---

## 4. When module code is executing / active

| Component | Process | When its code runs |
|---|---|---|
| Backend Logos Module | own `logos_host` subprocess | From **Loading** until **Unloading**. `onContextReady()` runs once at load; thereafter the module is event-loop idle, waking on inbound method calls or its own timers/events. Continues running while Basecamp is minimized to tray. |
| UI App **QML view** | Basecamp process | While the app is **loaded** (tab exists). Driven by the Qt UI event loop — runs on user interaction, property updates, and incoming event callbacks. |
| UI App **C++ backend** (`ui_qml`) | own `ui-host` subprocess | From load until unload, independent of whether the tab is focused. Active on QML `callModule` dispatch, on its own timers (e.g. the broadcast tick), and pushing `PROP` updates to replicas. |
| `liblogos_core` | linked into Basecamp | Whole app lifetime — registry, lifecycle C API, ~2s stats polling. |
| Basecamp C++ backend (`MainUIBackend` + managers) | Basecamp process | Whole app lifetime; see [§5](#5-basecamps-own-c-backend). |

So a module is "active" the entire time it is **loaded**, not only while being
called: it holds a live process (or, for a QML view, lives in the host event
loop), retains state set up in `onContextReady()`, and can emit events or run
timers at any time. Method dispatch is the on-demand part layered on top.

---

## 5. Basecamp's own C++ backend

Distinct from the modules it manages, Basecamp's UI backend is split into four
classes with a unidirectional dependency graph
([CLAUDE.md](repos/logos-basecamp/CLAUDE.md),
[src/](repos/logos-basecamp/src)):

```
MainUIBackend (QML-facing facade; owns the other three as Qt children)
   ├─► CoreModuleManager   — sole owner of the logos_core_* C API + stats timer
   ├─► UIPluginManager     — UI-plugin widget lifecycle, app launcher, unload cascade
   └─► PackageCoordinator  — package_manager IPC: install/uninstall/upgrade + dialogs
```

- **MainUIBackend** — thin facade holding only navigation state; each QML slot
  is a one-line delegation. `coreModules()` composes data from several managers.
- **CoreModuleManager** — the only code that calls the `logos_core_*` C API
  (`knownModules`, `loadModule`, `unloadModuleWithDependents`, stats polling).
- **UIPluginManager** — in-process UI-plugin widget teardown, `QPluginLoader`
  wiring, app launcher, local unload cascade.
- **PackageCoordinator** — every `package_manager` interaction: LGX install flow,
  gated uninstall/upgrade (acks `beforeUninstall`/`beforeUpgrade` within 3s),
  cascade dialogs, package-state caches.

Construction order: CoreModuleManager → UIPluginManager → PackageCoordinator,
then `setPackageCoordinator` closes the cycle. Qt reverse-order destruction tears
down PackageCoordinator first (stops emitting), then UIPluginManager (widgets
while the C handle is still valid), then CoreModuleManager.

---

## 6. UI modules (Qt frontend + C++ backend)

A `ui_qml` view module is split across a **process boundary** for isolation: the
QML view runs in the host app, the C++ backend runs in a dedicated `ui-host`
child process. A crash/hang in one view backend cannot take down the host or
other view modules.

```
┌─ Host app (basecamp) ──────────┐         ┌─ ui-host (child process) ─┐
│  QML view ── logos.callModule ─▶│         │   ViewModuleProxy         │
│       │                         │  QRO    │     │                     │
│  LogosQmlBridge ────────────────┼────────▶│   QPluginLoader           │
│       │                         │  local  │     │                     │
│  ViewModuleHost ── spawn ──────▶│  socket │   <view backend>.so       │
└────────────────────────────────┘         └───────────────────────────┘
```

### The three authored pieces (see [bcast-ui/src](bcast-ui/src))

1. **`.rep` contract** ([broadcast_app.rep](bcast-ui/src/broadcast_app.rep)) —
   the QtRO interface: `SLOT`s (callable from QML) and `PROP`s (auto-synced to
   every replica). `logos_module(REP_FILE …)` generates a `<Foo>SimpleSource`
   base and a typed source/replica pair.
2. **C++ backend** ([broadcast_app_backend.h](bcast-ui/src/broadcast_app_backend.h))
   — derives the generated `<Foo>SimpleSource` (implement its slots, feed its
   PROPs via `setXxx(...)`) **and** `LogosUiPluginContext` (gives
   `onContextReady()` + `modules()` for any declared `dependencies`). The
   author writes *only* this class and the `.rep`; the `*Plugin` /
   `*Interface` classes, `Q_PLUGIN_METADATA`, `initLogos`, and QtRO registration
   are generated around it.
3. **QML view** ([qml/Main.qml](bcast-ui/src/qml/Main.qml)) — declared as
   `view` in metadata; loaded into a sandboxed `QQuickWidget`.

### Remoting & data flow

- **`ViewModuleHost`** (host side) spawns `ui-host`, generates a unique local
  socket name, watches stdout for `READY`, emits `ready()`. The parent then wires
  the bridge: `bridge->setViewModuleSocket(name, socket)`.
- **`ui-host`** (child) loads the plugin, calls `initLogos(LogosAPI*)` via
  reflection, then exposes the backend over a `QRemoteObjectHost`:
  - **Typed remoting (preferred)** — if the plugin implements `LogosViewPlugin`
    (it derives the generated `<Foo>ViewPluginBase`), `ui-host` calls
    `enableRemoting<FooSourceAPI>(backend)`, so the QML side gets a typed replica
    that reaches the `Valid` state. The remoted object is `viewObject()`.
  - **Dynamic remoting (fallback)** — for plugins without a `.rep`, all
    `Q_INVOKABLE`s/slots/signals/`Q_PROPERTY`s are remoted via a
    `QRemoteObjectDynamicReplica`. Any `QAbstractItemModel*` property is
    additionally remoted as a child source `<module>/<property>`.
- **`LogosQmlBridge`** routes `callModule`/`callModuleAsync` to the right place
  (backend IPC vs view-module QRO) and **serializes all results to JSON strings**
  so QML always sees a string.

### Sandboxing (untrusted QML)

A `ui_qml` app's QML/JS loads into the Basecamp process, so its engine is
confined by `QmlSandbox::configure` ([src/restricted](repos/logos-basecamp/src/restricted)):

- **Network** — deny-all NAM + URL interception; UI apps reach the network only
  indirectly through (un-sandboxed) backend modules.
- **Filesystem** — URL interceptor allows only `qrc:` and files under an
  allow-list (the app's own dir, vetted shared Logos QML, Qt's module dirs).
- **Native code** — the app's install dir is kept off the native-plugin search
  path and a `qmldir` there may not declare a `plugin` (closes escape F-008).

The `sandbox-test` Nix check guards these guarantees, including an adversarial
fixture that fires every escape vector on load.

### DEV iteration

`DEV_QML_PATH=$PWD/src` makes the three MainContainer view-entries read QML from
disk instead of the embedded qrc — relaunch (no rebuild) to pick up edits. Editing
sub-components reached via `import Basecamp.<Feature>` still needs `nix build`
(see [CLAUDE.md](repos/logos-basecamp/CLAUDE.md)).
</content>
</invoke>
