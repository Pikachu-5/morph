# morph.provider.comio — IStream over ISource/ISink.
#
# Not a provider itself: the piece two providers need to speak to COM. It lives
# here rather than in morph.core because the core is deliberately free of COM.

add_library(morph_provider_comio STATIC com_stream.cpp)
add_library(morph::provider::comio ALIAS morph_provider_comio)

target_include_directories(morph_provider_comio
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(morph_provider_comio PUBLIC morph::core morph::settings)

set_target_properties(morph_provider_comio PROPERTIES
    FOLDER "providers"
    OUTPUT_NAME "morph.provider.comio")
