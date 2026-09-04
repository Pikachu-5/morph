# morph.worker.exe — all parsing, decoding and encoding (architecture section 3.1).
#
# Also the unadvertised CLI (section 2). The same Providers table and Executor
# serve both, so the broker-driven mode does not grow a second code path.

add_executable(morph_worker
    main.cpp
    providers.cpp
    serve.cpp
)

target_link_libraries(morph_worker PRIVATE
    morph::core
    morph::settings
    morph::provider::docmd
    morph::provider::dochtml
    morph::provider::docooxml
    morph::provider::pdf
    morph::provider::wic
    morph::provider::mf)

set_target_properties(morph_worker PROPERTIES
    FOLDER "processes"
    OUTPUT_NAME "morph.worker")

# pdfium.dll is loaded at run time and must sit beside the executable.
get_property(MORPH_PDFIUM_DLL GLOBAL PROPERTY MORPH_PDFIUM_DLL)
add_custom_command(TARGET morph_worker POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MORPH_PDFIUM_DLL}" "$<TARGET_FILE_DIR:morph_worker>"
    COMMENT "Copying pdfium.dll next to morph.worker")
