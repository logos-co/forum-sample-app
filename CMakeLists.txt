cmake_minimum_required(VERSION 3.14)
project(ExampleForumPlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

# Derive the module name + version from metadata.json — single source of truth.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json" METADATA_JSON)
string(JSON MODULE_NAME GET ${METADATA_JSON} name)
string(JSON MODULE_VERSION GET ${METADATA_JSON} version)

# Universal UI module: you write the .rep + the *Backend class. REP_FILE runs
# repc; the generated *Plugin glue in generated_code/ is compiled automatically.
logos_module(
    NAME ${MODULE_NAME}
    REP_FILE src/example_forum.rep
    SOURCES
        src/example_forum_backend.h
        src/example_forum_backend.cpp
        src/forum_message.h
        src/forum_message.cpp
    INCLUDE_DIRS
        src
)

# Surface the metadata.json version to the backend, which publishes it to the
# QML view via the appVersion PROP (so the title's version stays in sync here).
target_compile_definitions(${MODULE_NAME}_module_plugin PRIVATE
    EXAMPLE_FORUM_VERSION="${MODULE_VERSION}")
