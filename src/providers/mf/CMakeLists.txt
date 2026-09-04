# morph.provider.mf — the Media Foundation backend (architecture section 7.1).
#
# No third-party dependency, by design: MF is the OS's own codecs, which is what
# makes a Store SKU possible (section 7.3) and what keeps the H.264 and HEVC
# patent licensing with the platform rather than with us.

add_library(morph_provider_mf STATIC
    mf_provider.cpp
    mf_formats.cpp
)
add_library(morph::provider::mf ALIAS morph_provider_mf)

target_include_directories(morph_provider_mf
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(morph_provider_mf
    PUBLIC  morph::core morph::settings morph::provider::comio
    PRIVATE mf mfplat mfreadwrite mfuuid ole32 propsys)

set_target_properties(morph_provider_mf PROPERTIES
    FOLDER "providers"
    OUTPUT_NAME "morph.provider.mf")
