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
    unit/test_wic.cpp
    unit/test_mf.cpp
    unit/test_ffmpeg.cpp
    unit/test_hostile_input.cpp
    unit/test_path_safety.cpp
    unit/test_probe_cache.cpp
    unit/test_pdf.cpp
    unit/test_pdf_reader.cpp
    unit/test_ipc.cpp
    unit/test_executor.cpp
    unit/test_worker_protocol.cpp
    unit/test_broker_output.cpp
    unit/test_capability_table.cpp
    unit/test_shell_menu.cpp
    unit/test_broker_scheduling.cpp
    integration/test_broker_end_to_end.cpp
)

add_subdirectory(fuzz)
add_subdirectory(bench)

target_include_directories(morph_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(morph_tests PRIVATE
    morph::core
    morph::broker
    morph::shellext::core
    morph::provider::docmd
    morph::provider::dochtml
    morph::provider::docooxml
    morph::provider::pdf
    morph::provider::wic
    morph::provider::mf
    morph::provider::ffmpeg
    morph::fuzz::targets
    morph::settings
    Catch2::Catch2WithMain
    miniz::miniz)

set_target_properties(morph_tests PROPERTIES FOLDER "tests")

# Registers each TEST_CASE with CTest individually, so a failure names the case
# rather than the binary.
catch_discover_tests(morph_tests)

# The stand-in for ffmpeg.exe and ffprobe.exe (tests/tools/fake_ffmpeg.cpp).
#
# It lives in its own directory rather than beside the binaries, for two reasons:
# a build tree that contains something called ffmpeg.exe would have the REAL
# provider find it, and a packaging step that copied bin/ wholesale would ship a
# test double as a codec.
add_executable(morph_fake_ffmpeg tools/fake_ffmpeg.cpp)
target_link_libraries(morph_fake_ffmpeg PRIVATE morph::settings)
set_target_properties(morph_fake_ffmpeg PROPERTIES
    FOLDER "tests"
    OUTPUT_NAME "ffmpeg"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/fake-ffmpeg")

# The provider looks for two binaries by name and tells them apart by the
# arguments it gives them, so the same executable answers to both.
add_custom_command(TARGET morph_fake_ffmpeg POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:morph_fake_ffmpeg>"
            "${CMAKE_BINARY_DIR}/bin/fake-ffmpeg/ffprobe.exe"
    COMMENT "Copying the FFmpeg stand-in to ffprobe.exe")

add_dependencies(morph_tests morph_fake_ffmpeg)

# The end-to-end test starts morph.broker.exe, which starts morph.worker.exe.
# Both have to exist next to the test binary before it runs -- they all share one
# output directory for exactly this reason.
add_dependencies(morph_tests morph_broker morph_worker)

# pdfium.dll is loaded at run time and must sit beside the test executable.
get_property(MORPH_PDFIUM_DLL GLOBAL PROPERTY MORPH_PDFIUM_DLL)
add_custom_command(TARGET morph_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MORPH_PDFIUM_DLL}" "$<TARGET_FILE_DIR:morph_tests>"
    COMMENT "Copying pdfium.dll next to the tests")
