# morph.provider.pdf — PDF output through the WebView2 print pipeline
# (architecture section 8.6). PDFium input (section 8.7) joins this target later.

find_package(unofficial-webview2 CONFIG REQUIRED)

# PDFium has no vcpkg port, so it is fetched as a pinned, checksummed prebuilt
# by tools/fetch-pdfium.ps1 (architecture section 13.2).
set(MORPH_PDFIUM_DIR "${CMAKE_SOURCE_DIR}/third_party/pdfium")
if(NOT EXISTS "${MORPH_PDFIUM_DIR}/PDFiumConfig.cmake")
    message(FATAL_ERROR
        "PDFium is missing. Run tools/fetch-pdfium.ps1 to download the pinned release.")
endif()
find_package(PDFium REQUIRED PATHS "${MORPH_PDFIUM_DIR}" NO_DEFAULT_PATH)

add_library(morph_provider_pdf STATIC
    pdf_reader.cpp
    pdf_writer.cpp
    webview2_host.cpp
)
add_library(morph::provider::pdf ALIAS morph_provider_pdf)

target_include_directories(morph_provider_pdf
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(morph_provider_pdf
    PUBLIC  morph::core morph::settings
    PRIVATE unofficial::webview2::webview2 pdfium)

set_target_properties(morph_provider_pdf PROPERTIES
    FOLDER "providers"
    OUTPUT_NAME "morph.provider.pdf")

# pdfium.dll is loaded at run time, so it has to sit beside every executable that
# links this provider. Exported as a property so the test and worker targets can
# copy it without hard-coding the path.
set_property(GLOBAL PROPERTY MORPH_PDFIUM_DLL "${MORPH_PDFIUM_DIR}/bin/pdfium.dll")
