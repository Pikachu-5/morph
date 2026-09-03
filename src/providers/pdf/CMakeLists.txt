# morph.provider.pdf — PDF output through the WebView2 print pipeline
# (architecture section 8.6). PDFium input (section 8.7) joins this target later.

find_package(unofficial-webview2 CONFIG REQUIRED)

add_library(morph_provider_pdf STATIC
    pdf_writer.cpp
    webview2_host.cpp
)
add_library(morph::provider::pdf ALIAS morph_provider_pdf)

target_include_directories(morph_provider_pdf
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(morph_provider_pdf
    PUBLIC  morph::core morph::settings
    PRIVATE unofficial::webview2::webview2)

set_target_properties(morph_provider_pdf PROPERTIES
    FOLDER "providers"
    OUTPUT_NAME "morph.provider.pdf")
