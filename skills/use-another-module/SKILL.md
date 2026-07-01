---
name: use-another-module
description: Make one Logos module call (and subscribe to events from) another — declaring the dependency in metadata.json + flake.nix and reaching it through the generated typed `modules().<dep>` wrapper. Covers all three authoring flavors (universal core, universal ui_qml, explicit ui_qml plugin). Use when a module needs to invoke another module's methods/events (e.g. bcast-ui using delivery_module, forum_ui using forum_app).
---

# Use another module as a dependency

A Logos module calls another by declaring it as a **dependency** and reaching it
through a generated, typed wrapper — never the raw `LogosAPI`, never string
method names. There is no manual socket/token code: the builder introspects the
dependency's published LIDL contract and emits a `modules().<dep>` wrapper with
typed methods and event accessors.

For the concrete delivery-messaging case (a worked instance of this skill), use
[use-delivery-module](../use-delivery-module/SKILL.md).

## The two declarations (both required, names must match)

The dependency's **snake_case module name** is the single source of truth and
must appear identically in both places, or the wrapper isn't generated:

1. **`metadata.json#dependencies`** — add the module name:
   ```json
   "dependencies": ["other_module"]
   ```
2. **`flake.nix` inputs** — add an input whose **attribute name equals the
   module name**:
   ```nix
   inputs = {
     logos-module-builder.url = "github:logos-co/logos-module-builder";
     other_module.url = "github:logos-co/logos-other-module/v1.2.3";  # pinned
     # or a sibling checkout:  other_module.url = "path:../other-module";
     # or a monorepo subdir:   other_module.url = "github:org/repo?dir=other-module";
   };
   ```
   `outputs = inputs@{ logos-module-builder, ... }` already forwards everything via
   `flakeInputs = inputs`, so you do **not** need to name the new input in the
   destructuring (forum-ui names `forum_app` there, but that's optional).

The builder matches each `dependencies` entry to the like-named `flakeInputs`
attribute. Building your module generates the wrapper from the dependency's
LIDL — it does **not** build the dependency's plugin (see the developer guide's
"dependency interfaces" section). Examples: [forum-ui/flake.nix](../../forum-ui/flake.nix)
(`path:` input), [bcast-ui/flake.nix](../../bcast-ui/flake.nix) (pinned GitHub).

## Reaching the dependency in code

Each `metadata.json#dependencies` entry becomes one accessor on the generated
`struct LogosModules` (emitted in `logos_sdk.h`). The consumer's `.cpp` must
`#include "logos_sdk.h"` to make `LogosModules` complete — the SDK context
headers only forward-declare it.

**API style follows the *consumer's* `interface`, not the dependency's:**

| Consumer `interface` | Wrapper arg/return types |
|---|---|
| `universal` **core** module | **std** (`std::string`, `int64_t`, `LogosMap`, `StdLogosResult`) |
| `ui_qml` / `qt` / `provider` | **Qt** (`QString`, `qint64`, `QByteArray`, `LogosResult`) |

So the *same* dependency exposes std-typed wrappers to a core consumer and
Qt-typed wrappers to a UI consumer.

### Where you get `modules()` and where to wire it — by flavor

| Consumer flavor | Base class giving `modules()` | Construct wrapper | Wire calls/subscriptions in |
|---|---|---|---|
| Universal **core** | `LogosModuleContext` (`logos_module_context.h`) | framework-provided | `onContextReady()` / any method body |
| Universal **ui_qml** | `LogosUiPluginContext` (`logos_ui_plugin_context.h`) | framework-provided | `onContextReady()` |
| Explicit **ui_qml plugin** (forum-ui/delivery-demo style) | none — hold `m_logos` yourself | `m_logos = new LogosModules(api)` in `initLogos(LogosAPI*)` | `initLogos`, then `m_logos->...` |

In the two **universal** flavors `modules()` is live from `onContextReady()`
onward (guard early helpers with `isContextReady()`); do **not** call it from the
constructor. The explicit-plugin flavor instead news up `LogosModules` from the
`LogosAPI*` handed to `initLogos`.

### Calling methods

```cpp
#include "logos_sdk.h"                       // generated — makes LogosModules complete

// universal core (std types):
std::string reply = modules().greeter_module.greet(name);

// universal ui_qml (Qt types):
LogosResult r = modules().other_module.doThing(QStringLiteral("x"));
if (!r.success) { /* r.getError() */ }

// explicit ui_qml plugin:
int sum = m_logos->forum_app.add(a, b);      // see forum-ui/src/forum_ui_plugin.cpp
```

### Subscribing to a dependency's events

Events are the other half of a module's API. Two equivalent accessor forms are
generated:

```cpp
// typed accessor (universal/std consumers):
m_sub = modules().greeter_module.onGreeted([this](const std::string& g){ /* ... */ });

// generic form (works for Qt consumers; args arrive as a QVariantList):
modules().other_module.on("someEvent", [this](const QVariantList& data){
    if (data.isEmpty()) return;
    auto first = data.at(0).toString();      // positional, per the dep's event spec
});
```

Wire subscriptions **before** triggering the activity that emits them. Callbacks
are delivered on the consumer's event-loop thread, so emitting your own QtRO
source signal / setting a PROP from inside the callback is safe.

## Runtime: dependency loads first; modules are shared singletons

- **Load order** — Basecamp/liblogos loads declared dependencies *before* the
  consumer (`logos_core_load_module_with_dependencies`). For `nix run`
  standalone, deps are baked into the app at build time
  (`collectAllModuleDeps`); change deps → rebuild. See
  [module-actions.md](../../helper-mds/module-actions.md).
- **Shared singleton state** — a dependency is loaded **once** and shared by every
  app that depends on it ([module-considerations.md](../../helper-mds/module-considerations.md)).
  Don't assume you are the first/only user: an init-once call (e.g. a
  `createNode`-style bootstrap) may fail because another app already ran it —
  handle that path gracefully rather than aborting.

## Verify

1. Lock the new input (flakes only see **git-tracked** files, and need the lock
   updated):
   ```bash
   nix flake lock          # adds the dependency node to flake.lock
   git add .               # so the flake sees new/edited files
   ```
2. Build — this is the real check that the wrapper generated and your call sites
   resolve against it:
   ```bash
   nix build -L
   ```
   IDE/clangd errors on `logos_sdk.h` / Qt types before a build are expected
   false positives; the clean `nix build` is authoritative.
3. Inspect the generated wrapper if a call won't resolve:
   ```bash
   nix build .#generate --out-link result-generate   # then read logos_sdk.h
   ```

## Gotchas

- **Name mismatch is silent at lock time, loud at build.** The `dependencies`
  entry and the flake input attribute must be byte-identical; otherwise no
  wrapper accessor is generated and the build fails to resolve `modules().<dep>`.
- **`#include "logos_sdk.h"` in the `.cpp`, not the header.** The context headers
  forward-declare `LogosModules`; the inline `modules()` body only compiles where
  the full definition is visible.
- **Don't touch `modules()` in the constructor** (universal flavors) — the
  framework wires the pointer afterwards; use `onContextReady()`.
- **`core_manager` is always emitted** in `LogosModules` even with no
  dependencies, so apps can drive `liblogos_core` directly.
- **Advanced — runtime-chosen provider:** instead of a fixed dependency you can
  declare a *dependency interface* (a method/event contract) and bind it to a
  concrete module at runtime with `modules().bind_<interface>("concrete_module")`.
  See the "dependency interfaces" section of
  [ref-repos/logos-tutorial/logos-developer-guide.md](../../ref-repos/logos-tutorial/logos-developer-guide.md).
