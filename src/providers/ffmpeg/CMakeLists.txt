# morph.provider.ffmpeg — the GitHub SKU's codecs (architecture section 7.2).
#
# NOTHING FFMPEG IS LINKED HERE, and that is the point rather than an oversight:
# section 7.2 consumes FFmpeg as a subprocess so the LGPL relinking obligation
# never arises, and section 14's licensing matrix depends on it staying that way.
# An #include of an FFmpeg header in this directory would change what this
# package is.

add_library(morph_provider_ffmpeg STATIC
    ffmpeg_provider.cpp
    ffmpeg_command.cpp
    ffmpeg_process.cpp
)
add_library(morph::provider::ffmpeg ALIAS morph_provider_ffmpeg)

target_include_directories(morph_provider_ffmpeg
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(morph_provider_ffmpeg PUBLIC morph::core morph::settings)

set_target_properties(morph_provider_ffmpeg PROPERTIES
    FOLDER "providers"
    OUTPUT_NAME "morph.provider.ffmpeg")
