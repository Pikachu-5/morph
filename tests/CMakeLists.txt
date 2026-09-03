find_package(Catch2 3 REQUIRED)
include(Catch)

add_executable(morph_tests
    unit/test_media_profile.cpp
    unit/test_graph.cpp
    unit/test_planner.cpp
    unit/test_planner_invariants.cpp
)

target_include_directories(morph_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(morph_tests PRIVATE
    morph::core
    morph::settings
    Catch2::Catch2WithMain)

set_target_properties(morph_tests PROPERTIES FOLDER "tests")

# Registers each TEST_CASE with CTest individually, so a failure names the case
# rather than the binary.
catch_discover_tests(morph_tests)
