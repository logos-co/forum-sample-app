---
name: rename-logos-module
description: Rename a Logos C++ module end to end — every file, identifier, and config field that must change so it still builds with `nix build .`. Covers both authoring flavors: universal "core" modules (impl class only) and "ui_qml" modules (C++ backend + .rep + QML view). Use when changing a module's name (e.g. "minimal" → "broadcast_module" or "ui_example" → "broadcast_app").
---

# Rename a Logos module

Renaming a Logos module means changing the name in **three casings** consistently
across source, config, and (for UI modules) the QML view. Miss one and
`nix build .` fails (missing generated header, unresolved class) or — worse for
UI — the view loads but silently never connects to the backend.

## Two flavors

Check `metadata.json#type` first; it decides which files exist.

| | Universal **core** module | **ui_qml** module |
|---|---|---|
| `metadata.type` | `core` (`interface: universal`) | `ui_qml` |
| You author | one impl class | a `.rep` view contract + a backend class + a QML view |
| Source files | `src/<snake>_impl.{h,cpp}` | `src/<snake>.rep`, `src/<snake>_backend.{h,cpp}`, `src/qml/Main.qml` |
| Main class | `<Pascal>Impl : public LogosModuleContext` | `<Pascal>Backend : public <Pascal>SimpleSource, public LogosUiPluginContext` |
| `codegen.rep` in metadata | absent | `src/<snake>.rep` |
| Generated (don't touch) | `<snake>_api.{h,cpp}` | `rep_<snake>_source.h`, `<Pascal>SimpleSource` |

The rename mechanics are the same; the steps below mark **(core)** / **(ui_qml)**
where they diverge. Steps with no tag apply to both.

## Naming conventions (the three forms)

Pick the new name once, then derive every form from it.

| Form | Rule | core e.g. | ui_qml e.g. |
|------|------|-----------|-------------|
| `snake_case` | metadata `name`, filenames, `logos.module("…")` ids | `broadcast_module` | `broadcast_app` |
| `PascalCase` | C++ / `.rep` class names | `BroadcastModule` | `BroadcastApp` |
| `Title Case` | human-facing `display_name`, QML label text | `Broadcast Module` | `Broadcast App` |

Derived identifiers follow from these:
- plugin lib (metadata `main`) → `<snake>_plugin` *(both)*
- CMake `project()` → cosmetic target name; conventionally `<Pascal>Plugin`. The
  build's module identity comes from metadata `name`, **not** this — so it only
  needs to be sensible and consistent, not exact. *(both)*
- **(core)** impl class → `<Pascal>Impl`
- **(ui_qml)** `.rep` class → `<Pascal>`; backend → `<Pascal>Backend`; generated
  base → `<Pascal>SimpleSource`; generated rep header → `rep_<snake>_source.h`

## What to change

Work from the module directory. `<snake>`/`<Pascal>`/`<Title>` mean the new name.

### 1. `metadata.json`
- `"name"`: `<snake>`
- `"display_name"`: `<Title>`
- `"main"`: `<snake>_plugin`
- **(ui_qml)** `"codegen": { "rep": "src/<snake>.rep" }`
- Leave `"description"` alone unless it embeds the old *name*. It is prose, so a
  word like "a minimal example module" can stay accurate after the rename — don't
  blind-replace it.

`CMakeLists.txt` reads `name` from here via `string(JSON …)`, so the module name
is single-sourced — but the literal *paths* in `CMakeLists.txt` (step 4) still
need updating.

### 2. Rename the source files (preserve history with `git mv`)
- **(core)** `src/<old_snake>_impl.h` → `src/<snake>_impl.h`; `_impl.cpp` likewise
- **(ui_qml)** `src/<old_snake>.rep`, `src/<old_snake>_backend.{h,cpp}` → `<snake>.*`

### 3. File contents
- **(core)** `src/<snake>_impl.h`: class `<Pascal>Impl : public LogosModuleContext`;
  update doc comments, including any event-subscription example
  `modules().<snake>.on<Event>(...)` (the id there is the *snake* module name).
- **(core)** `src/<snake>_impl.cpp`: `#include "<snake>_impl.h"`; method
  definitions `<Pascal>Impl::…`; and any human-facing strings that name the
  module (greetings, status text).
- **(ui_qml)** `src/<snake>.rep`: `class <Pascal>` (the only declaration).
- **(ui_qml)** `src/<snake>_backend.h`: `#include "rep_<snake>_source.h"`; class
  `<Pascal>Backend : public <Pascal>SimpleSource, public LogosUiPluginContext`;
  update comment references to the base class / `.rep` filename.
- **(ui_qml)** `src/<snake>_backend.cpp`: `#include "<snake>_backend.h"`; method
  definitions `<Pascal>Backend::…`.

### 4. `CMakeLists.txt`
- `project(<Pascal>Plugin LANGUAGES CXX)` (and any header comment naming the module)
- **(core)** `SOURCES src/<snake>_impl.h` and `src/<snake>_impl.cpp`
- **(ui_qml)** `REP_FILE src/<snake>.rep`; `SOURCES src/<snake>_backend.{h,cpp}`

### 5. `src/qml/Main.qml` — **(ui_qml only; core modules have no view)**
- `logos.module("<snake>")`
- the `onViewModuleReadyChanged` guard: `if (moduleName === "<snake>")`
- `logos.isViewModuleReady("<snake>")`
- any human-facing label text using the `<Title>` form

### 6. `flake.nix`
- Usually no name reference (it reads `metadata.json`; note the entry point
  differs by flavor — `mkLogosModule` for core, `mkLogosQmlModule` for ui_qml,
  but neither names the module). Update the `description` string if it mentions
  the old name. Input attributes only change if you also rename a declared
  dependency.

## Verify

1. No stale references remain (case-insensitive; exclude build output). Search
   every old form you used:
   ```bash
   grep -rniI -e '<old_snake>' -e '<old title>' -e '<oldpascal>' . | grep -v result
   ```
   Expect no hits — except a deliberate prose word left in `description`.
2. Stage so the Nix flake can see the new/renamed files (flakes ignore untracked
   files). Stage only this module's folder unless told otherwise:
   ```bash
   git add <module-folder>
   ```
3. Build:
   ```bash
   nix build . -L
   ```
   Success looks like the build log using the new name throughout and a `result/`
   symlink to `…-logos-<snake>-module`. Confirm the plugin lib is named for the
   new module:
   ```bash
   find -L result -name '<snake>_plugin.so'
   ```

## Gotchas
- Generated artifacts are **built by the toolchain**, never created or renamed by
  hand: **(core)** `<snake>_api.{h,cpp}` (from the impl header); **(ui_qml)**
  `rep_<snake>_source.h` and `<Pascal>SimpleSource` (from repc). IDE "file not
  found" / "expected class name" / "QStringLiteral undeclared" errors on these
  *before* a build are expected — the clean `nix build` is the real check.
- **(ui_qml)** The QML `logos.module("…")` id must equal metadata `name` exactly,
  or the view loads but never connects to the backend (no compile error — a
  silent runtime failure).
- The CMake `project()` name is cosmetic and need not match the old name's exact
  derivation (e.g. a `minimal` module may have used `MinimalModulePlugin`, not
  `MinimalPlugin`) — just make it consistent with the new name.
- Nix flakes only see git-tracked files; forgetting `git add` after a rename
  produces confusing "file not found" build errors for files that exist on disk.
