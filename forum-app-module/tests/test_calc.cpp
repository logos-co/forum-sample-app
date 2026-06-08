#include "calc_module_impl.h"
#include <logos_test.h>

LOGOS_TEST(add) {
  auto t = LogosTestContext("calc_module");

  CalcModuleImpl calc;
  LOGOS_ASSERT_EQ(calc.add(3, 5), 8);
}

LOGOS_TEST(libVersion_converts_cstring_to_string) {
  auto t = LogosTestContext("calc_module");

  CalcModuleImpl calc;
  LOGOS_ASSERT_EQ(calc.libVersion(), std::string("1.0.0"));
}