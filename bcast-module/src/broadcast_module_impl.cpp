#include "broadcast_module_impl.h"

#include <iostream>

namespace {
// One consistently-tagged line per lifecycle hook / callback so they're easy
// to spot (and grep) in the host's stderr stream.
void logEvent(const std::string &what) {
  std::cerr << "[broadcast_module] " << what << std::endl;
}
} // namespace

BroadcastModuleImpl::BroadcastModuleImpl() {
  // Runs before the framework hands over the context — getters are still empty.
  logEvent("ctor — impl constructed (context not yet wired)");
}

BroadcastModuleImpl::~BroadcastModuleImpl() {
  logEvent("dtor — impl destroyed");
}

void BroadcastModuleImpl::onContextReady() {
  // Fires once, after the host populates the context. The canonical place for
  // one-time per-instance setup that depends on the persistence path.
  logEvent("onContextReady — context wired");
  std::cerr << "[broadcast_module]   modulePath=" << modulePath()
            << " instanceId=" << instanceId()
            << " instancePersistencePath=" << instancePersistencePath()
            << " isContextReady=" << (isContextReady() ? "true" : "false")
            << std::endl;
}

std::string BroadcastModuleImpl::greet(const std::string &name) {
  logEvent("greet(name=\"" + name + "\") — called");

  std::string greeting =
      "Hello, " + name + "! Greetings from the broadcast module.";

  // The generated event body routes the typed payload to every subscriber.
  logEvent("greet — emitting greeted(\"" + greeting + "\")");
  greeted(greeting);

  logEvent("greet — returning \"" + greeting + "\"");
  return greeting;
}

std::string BroadcastModuleImpl::getStatus() {
  logEvent("getStatus() — called");
  return "Broadcast module is running.";
}
