cmake_minimum_required(VERSION 3.14)
project(CalcModulePlugin LANGUAGES CXX)

# Include the Logos Module CMake helper (provided by logos-module-builder)
if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found")
endif()

# Define the module (with any external library dependencies) here.
# Because metadata.json sets `interface: universal`, the builder runs
# logos-cpp-generator over src/forum_app_impl.h before configuring,
# and LogosModule.cmake compiles the generated glue automatically.
logos_module(
    NAME forum_app
    SOURCES
        src/forum_app_impl.h
        src/forum_app_impl.cpp
)
