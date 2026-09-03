find_package(Catch2 3 REQUIRED)
# The OPC tests build deliberately hostile packages that PackageWriter refuses to
# produce, so they reach for miniz directly.
find_package(miniz CONFIG REQUIRED)
include(Catch)

add_executable(morph_tests
    unit/test_media_profile.cpp
    unit/test_graph.cpp
    unit/test_planner.cpp
    unit/test_planner_invariants.cpp
    unit/test_document_ir.cpp
    unit/test_markdown.cpp
    unit/test_html.cpp
    unit/test_opc.cpp
)

target_include_directories(morph_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(morph_tests PRIVATE
    morph::core
    morph::provider::docmd
    morph::provider::dochtml
    morph::provider::docooxml
    morph::settings
    Catch2::Catch2WithMain
    miniz::miniz)

set_target_properties(morph_tests PROPERTIES FOLDER "tests")

# Registers each TEST_CASE with CTest individually, so a failure names the case
# rather than the binary.
catch_discover_tests(morph_tests)
