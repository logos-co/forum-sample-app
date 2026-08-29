# cloud_data_core: the framework-agnostic CRDT/sync/durability engine. An
# ordinary internal CMake static library — not the Logos external_libraries/
# vendor_path mechanism, since this is first-party source compiled in the
# same tree, not a foreign prebuilt lib. Deliberately links nothing from the
# Logos SDK: see include/cloud_data_core/transport.h and blob_store.h for the
# two interfaces the embedding module supplies adapters for.
add_library(cloud_data_core STATIC
    src/hlc.cpp
    src/crdt.cpp
    src/store.cpp
    src/sync_engine.cpp
    src/engine.cpp
)

target_include_directories(cloud_data_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(cloud_data_core PUBLIC cxx_std_20)

find_package(SQLite3 REQUIRED)

target_link_libraries(cloud_data_core PUBLIC
    SQLite::SQLite3
)

# nlohmann/json comes from the Logos plugin SDK's include path in this build
# (the module build has never called find_package for it). Pick up the
# package if the toolchain does expose it, but don't require it — the header
# is already reachable via the SDK includes the parent scope sets up.
find_package(nlohmann_json QUIET)
if(nlohmann_json_FOUND)
    target_link_libraries(cloud_data_core PUBLIC nlohmann_json::nlohmann_json)
endif()
