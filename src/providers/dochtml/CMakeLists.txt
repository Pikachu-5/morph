# morph.provider.dochtml — HTML reader and writer (architecture section 8.5).

find_package(lexbor CONFIG REQUIRED)

add_library(morph_provider_dochtml STATIC
    html_reader.cpp
    html_writer.cpp
)
add_library(morph::provider::dochtml ALIAS morph_provider_dochtml)

target_include_directories(morph_provider_dochtml
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# vcpkg's static triplet exports lexbor::lexbor_static; a shared build exports
# lexbor::lexbor. Resolve it here rather than with a generator expression, which
# would defer the choice past the point where usage requirements propagate.
if(TARGET lexbor::lexbor_static)
    set(MORPH_LEXBOR_TARGET lexbor::lexbor_static)
elseif(TARGET lexbor::lexbor)
    set(MORPH_LEXBOR_TARGET lexbor::lexbor)
else()
    message(FATAL_ERROR "lexbor was found but exports no known target.")
endif()

# lexbor's exported target carries no INTERFACE_INCLUDE_DIRECTORIES, so the
# include path has to be located explicitly rather than inherited.
find_path(MORPH_LEXBOR_INCLUDE_DIR
    NAMES lexbor/html/html.h
    REQUIRED)

target_include_directories(morph_provider_dochtml PRIVATE ${MORPH_LEXBOR_INCLUDE_DIR})

target_link_libraries(morph_provider_dochtml
    PUBLIC  morph::core morph::settings
    PRIVATE ${MORPH_LEXBOR_TARGET})

# Without this, LXB_API expands to __declspec(dllimport) and every lexbor symbol
# fails to link against the static library.
if(MORPH_LEXBOR_TARGET STREQUAL "lexbor::lexbor_static")
    target_compile_definitions(morph_provider_dochtml PRIVATE LEXBOR_STATIC)
endif()

set_target_properties(morph_provider_dochtml PROPERTIES
    FOLDER "providers"
    OUTPUT_NAME "morph.provider.dochtml")
