# morph.core — the intermediate representation, format graph, planner, job model
# and options (architecture section 18).
#
# This library is deliberately free of Windows UI and COM dependencies so that it
# is unit-testable without a message pump, and so that the same code links into
# the broker, the worker and the CLI unchanged.

add_library(morph_core STATIC
    src/graph/media_profile.cpp
    src/graph/graph.cpp
    src/graph/planner.cpp
    src/graph/extensions.cpp
    src/io/stream.cpp
    src/io/options.cpp
    src/io/provider.cpp
    src/io/temp_store.cpp
    src/io/probe_cache.cpp
    src/io/executor.cpp
    src/doc/style.cpp
    src/doc/resource.cpp
    src/doc/document.cpp
    src/doc/pipeline.cpp
    src/ipc/frame.cpp
    src/ipc/codec.cpp
    src/ipc/worker_codec.cpp
    src/ipc/caps.cpp
    src/ipc/pipe.cpp
)
add_library(morph::core ALIAS morph_core)

target_include_directories(morph_core
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

find_package(blake3 CONFIG REQUIRED)
find_package(msgpack-c CONFIG REQUIRED)

target_link_libraries(morph_core
    PUBLIC  morph::settings
    # Content addressing for the resource store (section 8.2). Private: BLAKE3 is
    # an implementation detail of ResourceStore and must not leak into the
    # broker-facing headers, which section 12.3 requires stay portable.
    PRIVATE BLAKE3::blake3 msgpack-c
            # advapi32: the pipe's security descriptor (section 10.1).
            # kernel32 carries GetCurrentPackageFamilyName on Windows 8+.
            advapi32)

set_target_properties(morph_core PROPERTIES
    FOLDER "core"
    OUTPUT_NAME "morph.core")
