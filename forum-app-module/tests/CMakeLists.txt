cmake_minimum_required(VERSION 3.14)
project(ForumAppTests LANGUAGES CXX)

include(LogosTest)

logos_test(
    NAME forum_app_tests
    MODULE_SOURCES
        ../src/forum_app_impl.cpp
        mocks/forum_app_events_stub.cpp
    TEST_SOURCES
        main.cpp
        test_forum.cpp
)