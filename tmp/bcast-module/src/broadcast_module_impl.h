#pragma once

#include "logos_module_context.h"
#include <string>

/**
 * @brief A broadcast universal Logos module.
 *
 * In the universal authoring model you write only this implementation class.
 * Its public methods ARE the module's API — callable by other modules and from
 * the CLI (`logoscore -c`). The Qt plugin glue (the `*Plugin`/`*Interface`
 * classes, `Q_PLUGIN_METADATA`, `initLogos` wiring) is generated from this
 * header by `logos-module-builder`.
 *
 * Deriving `LogosModuleContext` gives you:
 *   - `modules()` — typed callers for anything in `metadata.json#dependencies`
 *   - typed event subscriptions (`modules().dep.on<Event>(...)`)
 *   - `onContextReady()` — override it to run once the module is wired
 *
 * Module code is Qt-free: use `std::string` and friends, not `QString`.
 *
 * Every lifecycle hook and callback below logs to `std::cerr` so the module's
 * activity is visible in the host's stderr stream (the constructor/destructor
 * are skipped by the codegen header parser, so they don't become API methods).
 */
class BroadcastModuleImpl : public LogosModuleContext {
public:
  BroadcastModuleImpl();
  ~BroadcastModuleImpl() override;

  /// Returns a greeting and announces it as a typed `greeted` event.
  std::string greet(const std::string &name);

  /// Returns a short status string.
  std::string getStatus();

  logos_events :
      /// Emitted by greet() with the greeting it produced. Other modules
      /// subscribe with `modules().broadcast_module.onGreeted(...)`.
      void greeted(const std::string &greeting);

protected:
  // Lifecycle hook from LogosModuleContext — fires exactly once, after the
  // host wires the context (modulePath/instanceId/instancePersistencePath),
  // before any method dispatch.
  void onContextReady() override;
};
