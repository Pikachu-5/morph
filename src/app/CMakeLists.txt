# Morph.App.exe — the standalone application (architecture section 12).
#
# Split the same way the shell extension is, and for the same reason: what the
# app DECIDES is a static library that a console test process can drive, and the
# framework around it is left with layout. Section 12.3 spells out why that
# matters more here than anywhere else -- if the C++/WinRT XAML tooling becomes
# a schedule risk, the shell is rewritten in C# over the broker protocol and
# everything below this line is untouched.
#
# So morph.app.core links no XAML, no WinRT and no provider. It talks to the
# broker over the pipe and takes probing as a function it is handed.

add_library(morph_app_core STATIC
    client.cpp
    model.cpp
)
add_library(morph::app::core ALIAS morph_app_core)

target_include_directories(morph_app_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morph_app_core PUBLIC morph::core morph::settings)
set_target_properties(morph_app_core PROPERTIES FOLDER "processes")

# --- The window --------------------------------------------------------------
#
# OFF by default, and the reason is in winappsdk.cmake: the Windows App SDK is
# read out of the local NuGet cache rather than fetched by a pinned, checksummed
# script the way PDFium is. Until that script exists this builds on a machine
# that has opened a WinUI project before and nowhere else, and a default that
# breaks a fresh clone is not a default.
option(MORPH_BUILD_APP "Build Morph.App.exe (needs the Windows App SDK)" OFF)

if(NOT MORPH_BUILD_APP)
    return()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/winappsdk.cmake")

morph_find_winappsdk(MORPH_HAVE_WINAPPSDK)
if(NOT MORPH_HAVE_WINAPPSDK)
    message(WARNING "MORPH_BUILD_APP is ON but the Windows App SDK was not found.")
    return()
endif()

set(MORPH_CPPWINRT_DIR "${CMAKE_CURRENT_BINARY_DIR}/winrt")
morph_generate_cppwinrt(morph_app_projection "${MORPH_CPPWINRT_DIR}")

add_executable(morph_app WIN32 main.cpp)
add_dependencies(morph_app morph_app_projection)

target_include_directories(morph_app PRIVATE
    "${MORPH_CPPWINRT_DIR}"
    ${MORPH_WINAPPSDK_INCLUDE})

target_compile_definitions(morph_app PRIVATE
    MORPH_WINAPPSDK_MAJORMINOR=${MORPH_WINAPPSDK_MAJORMINOR})

target_link_libraries(morph_app PRIVATE
    morph::app::core
    # The provider table, shared with the worker so the two processes cannot
    # disagree about what a file is (section 12.2 probes here, section 5.1
    # converts there).
    morph::providers
    "${MORPH_WINAPPSDK_LIB}"
    windowsapp
    shell32)

set_target_properties(morph_app PROPERTIES
    FOLDER "processes"
    OUTPUT_NAME "Morph.App")

# Bootstrap.lib is an IMPORT library. Without the DLL beside the executable the
# process dies before main runs, with no message and no window -- which is a
# genuinely confusing hour to spend, so it is copied like pdfium.dll is.
get_property(MORPH_PDFIUM_DLL GLOBAL PROPERTY MORPH_PDFIUM_DLL)
add_custom_command(TARGET morph_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MORPH_WINAPPSDK_DLL}" "$<TARGET_FILE_DIR:morph_app>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${MORPH_WINAPPSDK_PRI} "$<TARGET_FILE_DIR:morph_app>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MORPH_PDFIUM_DLL}" "$<TARGET_FILE_DIR:morph_app>"
    COMMENT "Copying the Windows App SDK bootstrapper, its resource indexes and pdfium next to Morph.App")

# The development manifest and its assets, so the build output is a loose
# package layout that `Add-AppxPackage -Register` accepts as it stands.
#
# Staged on every build, registered by hand and only when wanted: the app runs
# unpackaged, and section 13's packaged path is exercised early rather than for
# the first time at the end of the packaging milestone. The manifest staged here
# deliberately does NOT carry the shell extension -- see
# packaging/msix/AppxManifest.dev.xml for why.
add_custom_command(TARGET morph_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/packaging/msix/AppxManifest.dev.xml"
            "$<TARGET_FILE_DIR:morph_app>/AppxManifest.xml"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/packaging/msix/Assets"
            "$<TARGET_FILE_DIR:morph_app>/Assets"
    COMMENT "Staging the development package layout for Morph.App")
