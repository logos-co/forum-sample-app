---
name: surface-metadata-in-ui
description: Surface a build-time value from metadata.json (e.g. version) into the QML UI, since the `logos` QML bridge exposes no module metadata. Threads the value through CMake (string(JSON …) + a compile definition) → a READONLY .rep PROP set in the backend constructor → a QML `backend.<prop>` binding. Use when the UI needs to display anything held in metadata.json — version, display_name, category — kept in sync from that single source.
---

# Surface a metadata.json value in the QML UI

`metadata.json` is the module's single source of truth, but the `logos` QML
bridge (`LogosQmlBridge`) exposes **no** metadata to QML — there is no
`logos.version` or `logos.metadata`. QML only sees the typed replica returned by
`logos.module(...)`. So to show a metadata value (version, display name, …) in
the UI you thread it through the backend as a normal READONLY PROP, filled at
**build time** from `metadata.json`.

The chain — same one CMake already uses to read `name`:

```
metadata.json → CMake string(JSON) → compile definition → backend ctor setPROP() → .rep PROP → QML binding
```

Worked instance (version in the header): [CMakeLists.txt](../../CMakeLists.txt),
[src/example_forum.rep](../../src/example_forum.rep),
[src/example_forum_backend.cpp](../../src/example_forum_backend.cpp),
[src/qml/Main.qml](../../src/qml/Main.qml).

## The four edits

### 1. CMake — read the value, inject it as a compile definition

`CMakeLists.txt` already `file(READ ...)`s `metadata.json` and pulls `name`. Add
the field you want and hand it to the plugin target. **The target created by
`logos_module()` is `${MODULE_NAME}_module_plugin`** — that's what you define on:

```cmake
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json" METADATA_JSON)
string(JSON MODULE_NAME    GET ${METADATA_JSON} name)
string(JSON MODULE_VERSION GET ${METADATA_JSON} version)

logos_module(NAME ${MODULE_NAME} ...)   # creates target ${MODULE_NAME}_module_plugin

# Add AFTER logos_module() — the target must exist first.
target_compile_definitions(${MODULE_NAME}_module_plugin PRIVATE
    EXAMPLE_FORUM_VERSION="${MODULE_VERSION}")
```

The `"..."` quoting makes the macro expand to a C string literal.

### 2. `.rep` — declare a READONLY PROP

```cpp
// App version, sourced from metadata.json at build time (for display).
PROP(QString appVersion="" READONLY)
```

repc generates a `setAppVersion(const QString&)` setter on the `*SimpleSource`
base (setter = `set` + Capitalised prop name) and auto-syncs the value to every
QML replica, exactly like `status` / `topic`.

### 3. Backend constructor — set it from the macro (with a fallback)

```cpp
// Guard so the file still compiles (as "unknown") if the definition is missing.
#ifndef EXAMPLE_FORUM_VERSION
#define EXAMPLE_FORUM_VERSION "unknown"
#endif

ExampleForumBackend::ExampleForumBackend() {
  setAppVersion(QStringLiteral(EXAMPLE_FORUM_VERSION));
}
```

Setting it in the constructor is fine (unlike `modules()`, PROP setters don't
need the context wired). A build-time constant doesn't belong in
`onContextReady()`.

### 4. QML — bind the replica PROP

```qml
readonly property string appVersion: backend ? backend.appVersion : ""

LogosText {
    text: "Example Forum" + (root.appVersion.length > 0 ? " v" + root.appVersion : "")
}
```

Guarding on `.length > 0` avoids showing a stray `v` before the replica syncs
(see Gotchas).

## Verify

Building is the real check: if the PROP didn't generate the setter, the ctor's
`setAppVersion(...)` fails to compile.

```bash
nix build .#lib --out-link result-lib          # runs repc + compiles the backend
```

Confirm the actual metadata value (not the `"unknown"` fallback) is baked in.
`QStringLiteral` stores the text as **UTF-16**, so plain `strings` won't find it —
use `-e l` for 16-bit little-endian:

```bash
strings -e l result-lib/lib/example_forum_plugin.so | grep -F '1.0.0'
```

## Gotchas

- **Value is baked in at build time — rebuild after editing `metadata.json`.**
  Bumping the field alone changes nothing until you rebuild; no code edit needed.
- **Target name is `<module>_module_plugin`**, not `<module>` or the CMake
  `project()` name. `target_compile_definitions` on the wrong name fails ("no
  target").
- **Add the compile definition *after* `logos_module()`** — the target it refers
  to doesn't exist before that call.
- **`strings` won't show a `QStringLiteral`.** It's UTF-16 in the binary; use
  `strings -e l` (or grep the UTF-8 `metadata.json`, not the `.so`).
- **The PROP arrives when the replica reaches `Valid`, not at t=0.** Like every
  `.rep` PROP the title briefly shows without the value, then updates — matches
  `status`/`topic`. Guard the QML binding rather than assuming it's set on first
  paint.
- **clangd flags the macro as "undeclared".** The compile definition comes from
  CMake, which the language server hasn't picked up — a false positive, the same
  class as other clangd-vs-`nix build` errors noted in
  [AGENTS.md](../../AGENTS.md). The `#ifndef` fallback also clears it.
- **Keep the `#ifndef … #define … #endif` fallback.** It lets the translation
  unit compile even if some build path omits the definition, degrading to
  `"unknown"` instead of a hard error.
