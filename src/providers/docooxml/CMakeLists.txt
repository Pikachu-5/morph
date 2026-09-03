# morph.provider.docooxml — OPC container and the DOCX reader/writer
# (architecture section 8.4). This is the project's centrepiece: images are an
# afternoon with WIC and video is plumbing around FFmpeg, but nobody ships a
# free, permissively licensed DOCX engine.

find_package(pugixml CONFIG REQUIRED)
find_package(miniz CONFIG REQUIRED)

add_library(morph_provider_docooxml STATIC
    opc.cpp
    docx_styles.cpp
    docx_numbering.cpp
    docx_reader.cpp
)
add_library(morph::provider::docooxml ALIAS morph_provider_docooxml)

target_include_directories(morph_provider_docooxml
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

if(TARGET pugixml::static)
    set(MORPH_PUGIXML_TARGET pugixml::static)
else()
    set(MORPH_PUGIXML_TARGET pugixml::pugixml)
endif()

target_link_libraries(morph_provider_docooxml
    PUBLIC  morph::core morph::settings
    PRIVATE ${MORPH_PUGIXML_TARGET} miniz::miniz)

set_target_properties(morph_provider_docooxml PROPERTIES
    FOLDER "providers"
    OUTPUT_NAME "morph.provider.docooxml")
