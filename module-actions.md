# Module support — context for AI agents

Reference notes for working on Logos modules in this repo. Useful when a task
involves authoring, building, or understanding the glue around a module.

## Generating a module's in-between (generated) files

A module can generate its intermediate files with:

```sh
nix build .#generate --out-link result-generate
```

The generated files then appear in the `result-generate` directory. Inspect them
there to see what the build wires up around your hand-written sources (e.g.
generated `*Plugin` / `*Interface` classes, QtRO replica/source headers from the
`.rep`, and `initLogos` wiring).

## Supporting repos (under the `repos/` directory)

These checkouts explain the glue around modules — read them to understand how the
pieces fit together:

- `repos/logos-module-builder` — the module builder; how modules are assembled,
  generated, and built.
- `repos/logos-cpp-sdk` — the C++ SDK; the typed wrappers, context, and runtime
  glue that modules build against.

## Running a module: Basecamp (portable lgx) vs `nix run` (standalone)

Both ways of running a ui_qml/backend module (e.g. `bcast-ui`) use the **same
view-module runtime mechanics**. `repos/logos-view-module-runtime` is a *shared*
static library + `ui-host` binary that **both** `logos-basecamp` and
`logos-standalone-app` link. So in either case the backend plugin
(`<module>_plugin.so`) is loaded by a `ui-host` child process, remoted over a
private `QLocalSocket` via QtRO, and reached from QML through `LogosQmlBridge`
(`callModule` / `callModuleAsync`). Process isolation, typed-vs-dynamic
remoting, and the bridge path are identical. The differences are about
**packaging, the host shell, and when dependencies are resolved** — not how the
module executes once loaded.

| Aspect | Basecamp + portable `.lgx` | `nix run` (standalone) |
|---|---|---|
| Host app | Full multi-module host (capability_module, package-manager, chat, storage, …); your module is one tile among many | `mkStandaloneApp` wrapping `logos-standalone-app` — minimal single-module dev harness (just the module + deps + built-in capability_module) |
| Module artifact | **Portable** `.lgx` (e.g. `bcast-ui/result-lgx-portable/logos-broadcast_app-module.lgx`): self-contained, **no `/nix/store` refs**, transitive libs vendored; same artifact as logos-modules releases / Package Manager downloads (`nix bundle --bundler …#portable`) | Plugin taken **directly from the local Nix build** (`packages.default` `combined` derivation), **dev-linked** (`moduleLib`, has `/nix/store` runpaths) — only runs on the build machine |
| How loaded | Runtime: Basecamp extracts the platform variant (`linux-x86_64`, `darwin-arm64`, …) from the lgx tarball, reads `manifest.json`, drops into its `modules/` dir | Plugin dir extracted from `/nix/store` at build time; no lgx on the module's critical path |
| Dependency resolution | **Runtime** — modules dir + package manager; add/remove without rebuilding host | **Build time** — `collectAllModuleDeps` walks `metadata.json` `dependencies`, pulls each from its lgx, bakes a merged `modules/` dir into the wrapped app; change deps → rebuild |
| Portability | Runs anywhere (AppImage/DMG, other machines, CI, end users) — distribution/integration path | Machine-local, requires Nix, throwaway — fast dev inner loop |

Relevant builder code: `repos/logos-module-builder/lib/mkLogosQmlModule.nix`
(wires `apps.default` via `mkStandaloneApp` for `nix run`, and `lgx` /
`lgx-portable` packages) and `repos/logos-module-builder/lib/mkStandaloneApp.nix`
(plugin-dir extraction + `collectAllModuleDeps` bundling).

**Practical takeaway:** use `nix run` for fast iteration (single-module harness),
but it proves nothing about portability or coexistence with other modules. Use
the **portable** lgx in Basecamp to validate the real artifact — that the bundle
is self-contained (no leaked `/nix/store` paths), variant/manifest selection
works, and the module behaves next to the rest of the suite. A module can pass
`nix run` and still fail in Basecamp if a runtime lib wasn't vendored into the
portable bundle. Note the **local** lgx (`#default` bundler) still has store
refs — only the **portable** lgx exercises the true Basecamp distribution path.

## Documentation (under the `repos/` directory)

Potentially useful documentation lives in:

- `repos/logos-tutorial` — worked tutorials/examples.
- `repos/logos-docs` — reference documentation.
