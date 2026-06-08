#include "calc_module_impl.h"

int64_t CalcModuleImpl::add(int64_t a, int64_t b) { return a + b; }

int64_t CalcModuleImpl::multiply(int64_t a, int64_t b) { return a * b; }

int64_t CalcModuleImpl::factorial(int64_t n) {
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

int64_t CalcModuleImpl::fibonacci(int64_t n) {
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

std::string CalcModuleImpl::libVersion() { return std::string("1.0.0"); }

void CalcModuleImpl::libVersionNotify() {
  // Emit the event declared in `logos_events:`. When the module is
  // loaded by a host, this reaches every subscriber. When the class
  // is constructed outside a host (e.g. in unit tests), it is a
  // safe no-op.
  versionReady(std::string("1.0.0"));
}