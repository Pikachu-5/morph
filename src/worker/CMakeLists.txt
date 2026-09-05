# morph.worker.exe — all parsing, decoding and encoding (architecture section 3.1).
#
# Also the unadvertised CLI (section 2). The same Providers table and Executor
# serve both, so the broker-driven mode does not grow a second code path.

# The provider TABLE is its own library, because two processes need it and a
# second copy of it is a build where the app and the worker disagree about what
# formats exist -- which shows up as a target offered in the picker that fails
# the moment it is chosen. Section 12.2 probes files in the app; section 5.1
# converts them in the worker; both answers come from this one table.
add_library(morph_providers STATIC providers.cpp)
add_library(morph::providers ALIAS morph_providers)

target_include_directories(morph_providers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(morph_providers PUBLIC
    morph::core
    morph::settings
    morph::provider::docmd
    morph::provider::dochtml
    morph::provider::docooxml
    morph::provider::pdf
    morph::provider::wic
    morph::provider::mf
    morph::provider::ffmpeg)
set_target_properties(morph_providers PROPERTIES FOLDER "processes")

add_executable(morph_worker
    main.cpp
    serve.cpp
)

morph_apply_manifest(morph_worker)

target_link_libraries(morph_worker PRIVATE morph::providers)

set_target_properties(morph_worker PROPERTIES
    FOLDER "processes"
    OUTPUT_NAME "morph.worker")

# pdfium.dll is loaded at run time and must sit beside the executable.
get_property(MORPH_PDFIUM_DLL GLOBAL PROPERTY MORPH_PDFIUM_DLL)
add_custom_command(TARGET morph_worker POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MORPH_PDFIUM_DLL}" "$<TARGET_FILE_DIR:morph_worker>"
    COMMENT "Copying pdfium.dll next to morph.worker")
