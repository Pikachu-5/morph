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
)
add_library(morph::core ALIAS morph_core)

target_include_directories(morph_core
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(morph_core PUBLIC morph::settings)

set_target_properties(morph_core PROPERTIES
    FOLDER "core"
    OUTPUT_NAME "morph.core")
