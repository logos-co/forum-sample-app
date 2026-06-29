#include "forum_app_impl.h"
#include <logos_test.h>

LOGOS_TEST(add) {
  auto t = LogosTestContext("forum_app");

  ForumAppImpl forumApp;
  LOGOS_ASSERT_EQ(forumApp.add(3, 5), 8);
}

LOGOS_TEST(libVersion_converts_cstring_to_string) {
  auto t = LogosTestContext("forum_app");

  ForumAppImpl forumApp;
  LOGOS_ASSERT_EQ(forumApp.libVersion(), std::string("1.0.0"));
}

LOGOS_TEST(calls_other_module) {
  auto t = LogosTestContext("forum_app");

  ForumAppImpl forumApp;
  LOGOS_ASSERT_EQ(forumApp.getModuleInfo(), std::string("1.0.0"));
}