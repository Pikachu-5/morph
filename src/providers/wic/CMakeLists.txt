# morph.provider.wic — the image pipeline (architecture section 6).
#
# No third-party dependency: WIC ships in the OS, which is section 6's whole
# argument for it. windowscodecs.lib is the import library for it.

add_library(morph_provider_wic STATIC
    wic_provider.cpp
    wic_stream.cpp
)
add_library(morph::provider::wic ALIAS morph_provider_wic)

target_include_directories(morph_provider_wic
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(morph_provider_wic
    PUBLIC  morph::core morph::settings
    PRIVATE windowscodecs ole32 oleaut32 propsys)

set_target_properties(morph_provider_wic PROPERTIES
    FOLDER "providers"
    OUTPUT_NAME "morph.provider.wic")
