// Stub bodies for the impl's `logos_events:` methods.
// In the real build the codegen generates calc_module_events.cpp with
// bodies that route through LogosModuleContext. The test build skips
// that codegen, so we provide no-op stubs to satisfy the linker.
#include "forum_app_impl.h"

void ForumAppImpl::versionReady(const std::string &) {}