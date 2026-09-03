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
    unit/test_docx_reader.cpp
    unit/test_docx_writer.cpp
    unit/test_pdf.cpp
    unit/test_pdf_reader.cpp
)

target_include_directories(morph_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(morph_tests PRIVATE
    morph::core
    morph::provider::docmd
    morph::provider::dochtml
    morph::provider::docooxml
    morph::provider::pdf
    morph::settings
    Catch2::Catch2WithMain
    miniz::miniz)

set_target_properties(morph_tests PROPERTIES FOLDER "tests")

# Registers each TEST_CASE with CTest individually, so a failure names the case
# rather than the binary.
catch_discover_tests(morph_tests)

# pdfium.dll is loaded at run time and must sit beside the test executable.
get_property(MORPH_PDFIUM_DLL GLOBAL PROPERTY MORPH_PDFIUM_DLL)
add_custom_command(TARGET morph_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MORPH_PDFIUM_DLL}" "$<TARGET_FILE_DIR:morph_tests>"
    COMMENT "Copying pdfium.dll next to the tests")
