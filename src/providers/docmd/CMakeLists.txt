# morph.provider.docmd — Markdown reader and writer (architecture section 8.3).

find_package(md4c CONFIG REQUIRED)

add_library(morph_provider_docmd STATIC
    markdown_reader.cpp
    markdown_writer.cpp
)
add_library(morph::provider::docmd ALIAS morph_provider_docmd)

target_include_directories(morph_provider_docmd
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(morph_provider_docmd
    PUBLIC  morph::core morph::settings
    # md4c is an implementation detail: the header exposes only IR types, so
    # nothing downstream needs its include path.
    PRIVATE md4c::md4c)

set_target_properties(morph_provider_docmd PROPERTIES
    FOLDER "providers"
    OUTPUT_NAME "morph.provider.docmd")
