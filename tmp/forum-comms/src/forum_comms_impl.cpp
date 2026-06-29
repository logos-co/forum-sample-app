#include "forum_comms_impl.h"
#define MODULE_VERSION "1.0.2"

int64_t ForumCommsImpl::add(int64_t a, int64_t b) { return a + b; }

int64_t ForumCommsImpl::multiply(int64_t a, int64_t b) { return a * b; }

int64_t ForumCommsImpl::factorial(int64_t n) {
  if (n < 0)
    return -1; // Error case
  if (n == 0)
    return 1;
  int64_t result = 1;
  for (int64_t i = 1; i <= n; ++i) {
    result *= i;
  }
  return result;
}

int64_t ForumCommsImpl::fibonacci(int64_t n) {
  if (n < 0)
    return -1; // Error case
  if (n == 0)
    return 0;
  if (n == 1)
    return 1;
  int64_t a = 0, b = 1, fib = 0;
  for (int64_t i = 2; i <= n; ++i) {
    fib = a + b;
    a = b;
    b = fib;
  }
  return fib;
}

std::string ForumCommsImpl::libVersion() { return std::string(MODULE_VERSION); }

void ForumCommsImpl::libVersionNotify() {
  // Emit the event declared in `logos_events:`. When the module is
  // loaded by a host, this reaches every subscriber. When the class
  // is constructed outside a host (e.g. in unit tests), it is a
  // safe no-op.
  versionReady(std::string(MODULE_VERSION));
}