# morph.broker.exe — the graph, the queue and worker lifetime (section 3.1).
#
# It links morph.core and nothing else of ours. No provider is linked here on
# purpose: the broker is the process that survives, so it is the one process that
# must never contain a parser. It learns the graph from a worker instead
# (see morph/core/ipc/worker_protocol.h).

# The broker's parts are a library so the tests can reach them. main.cpp is the
# only thing left in the executable, which keeps the split honest: anything worth
# testing has to be on this side of the line.
add_library(morph_broker_core STATIC
    broker.cpp
    capabilities.cpp
    job.cpp
    output.cpp
    pool.cpp
    worker_channel.cpp
)
add_library(morph::broker ALIAS morph_broker_core)

target_include_directories(morph_broker_core PUBLIC ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(morph_broker_core PUBLIC morph::core morph::settings)

set_target_properties(morph_broker_core PROPERTIES FOLDER "processes")

add_executable(morph_broker main.cpp)
morph_apply_manifest(morph_broker)

target_link_libraries(morph_broker PRIVATE morph::broker)

set_target_properties(morph_broker PROPERTIES
    FOLDER "processes"
    OUTPUT_NAME "morph.broker")
