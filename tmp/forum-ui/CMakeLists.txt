cmake_minimum_required(VERSION 3.14)
project(ForumUiPlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

logos_module(
    NAME forum_ui
    REP_FILE src/forum_ui.rep
    SOURCES
        src/forum_ui_interface.h
        src/forum_ui_plugin.h
        src/forum_ui_plugin.cpp
)
