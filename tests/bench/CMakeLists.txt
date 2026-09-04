# The performance budgets (architecture section 17).
#
# A standalone executable rather than a ctest case, because it is a different
# question: the suite asks whether the code is correct and takes thirty seconds,
# and this asks whether it is fast enough and takes a minute of steady-state
# measurement. Section 16 runs it as its own gate ("fail the build on >10%
# regression"), not as part of the unit run.

add_executable(morph_bench bench.cpp)

target_link_libraries(morph_bench PRIVATE
    morph::core
    morph::settings
    morph::broker
    morph::shellext::core
    morph::provider::docmd
    morph::provider::dochtml
    morph::provider::docooxml
    morph::provider::pdf
    morph::provider::wic)

set_target_properties(morph_bench PROPERTIES
    FOLDER "tests"
    OUTPUT_NAME "morph.bench")

# It starts a broker, which starts a worker.
add_dependencies(morph_bench morph_broker morph_worker)
