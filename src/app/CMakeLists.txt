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
# ON by default now that tools/fetch-winappsdk.ps1 exists: the Windows App SDK
# is pinned and checksummed the way PDFium is, so asking for it is no longer
# asking a fresh clone for something only a machine that has opened a WinUI
# project can produce.
#
# ON is not the same as required. A tree without the packages fetched still
# builds everything else and says what is missing -- see morph_find_winappsdk --
# because the engine, the worker, the broker and the whole test suite have no
# dependency on the SDK at all, and a first configure that fails on the UI would
# hide that.
option(MORPH_BUILD_APP "Build Morph.App.exe (needs the Windows App SDK)" ON)

if(NOT MORPH_BUILD_APP)
    return()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/winappsdk.cmake")

morph_find_winappsdk(MORPH_HAVE_WINAPPSDK)
if(NOT MORPH_HAVE_WINAPPSDK)
    return()
endif()

set(MORPH_CPPWINRT_DIR "${CMAKE_CURRENT_BINARY_DIR}/winrt")
morph_generate_cppwinrt(morph_app_projection "${MORPH_CPPWINRT_DIR}")

add_executable(morph_app WIN32 main.cpp)
morph_apply_manifest(morph_app)
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

# --- The package manifests ---------------------------------------------------
#
# ONE TEMPLATE, generated twice. There used to be two hand-written manifests
# with a note in each saying they had to be kept in step, and they were not: the
# shipping one never grew the PackageDependency on the Windows App Runtime that
# the development one has. A packaged app does not bootstrap the runtime, it
# declares it, and getting that wrong produces a process that dies before main
# with nothing written anywhere. It would have been found by installing the
# first real package, which is a bad place to find it.
#
# The two differ in three strings: the identity, the display name, and nothing
# else that a person chooses. Everything structural -- the surrogate, the
# context menu verb, the capability set, the runtime dependency -- is shared,
# which is the property that was missing.
set(MORPH_PACKAGE_VERSION "${PROJECT_VERSION}.0")
set(MORPH_PACKAGE_PUBLISHER "CN=Morph (placeholder)")

# The development registration, staged beside the executable so the build output
# is a loose package layout `Add-AppxPackage -Register` accepts as it stands.
# Staged on every build; registered by hand and only when wanted, because
# registering it puts a verb in the right-click menu of every file on the
# machine and that is not a build step's decision to make.
set(MORPH_PACKAGE_NAME "Morph.Converter.Dev")
set(MORPH_PACKAGE_DISPLAY_NAME "Morph (development)")
configure_file(
    "${PROJECT_SOURCE_DIR}/packaging/msix/AppxManifest.xml.in"
    "${CMAKE_CURRENT_BINARY_DIR}/AppxManifest.dev.xml"
    @ONLY)

# The shipping manifest. Generated into the build tree rather than staged next
# to the executable, because the two identities must never be registered at
# once: they declare the same CLSID, and two packages offering one class is not
# a configuration anybody has thought about. Section 13's bundle picks it up
# from here.
set(MORPH_PACKAGE_NAME "Morph.Converter")
set(MORPH_PACKAGE_DISPLAY_NAME "Morph")
configure_file(
    "${PROJECT_SOURCE_DIR}/packaging/msix/AppxManifest.xml.in"
    "${CMAKE_BINARY_DIR}/packaging/msix/AppxManifest.xml"
    @ONLY)

# The manifest is staged by a command that DEPENDS on it rather than by a
# POST_BUILD step on morph_app, and the difference is not cosmetic: POST_BUILD
# runs only when the executable relinks, so editing the template regenerated the
# file in the build tree and left the registered layout holding the old one.
# What that looks like is a deployment error on a line number that does not
# match anything you can see in the file you just fixed.
#
# CMAKE_RUNTIME_OUTPUT_DIRECTORY rather than $<TARGET_FILE_DIR:morph_app>
# because this is a custom target's DEPENDS and the two are the same directory
# here (one configuration, set once in the root list).
set(MORPH_STAGED_MANIFEST "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/AppxManifest.xml")
add_custom_command(
    OUTPUT "${MORPH_STAGED_MANIFEST}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_BINARY_DIR}/AppxManifest.dev.xml"
            "${MORPH_STAGED_MANIFEST}"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/AppxManifest.dev.xml"
    COMMENT "Staging the development manifest for Morph.App"
    VERBATIM)
add_custom_target(morph_app_manifest DEPENDS "${MORPH_STAGED_MANIFEST}")
add_dependencies(morph_app morph_app_manifest)

# The assets are a directory copy, which is cheap and unconditional, so a
# POST_BUILD step is the right shape for them.
add_custom_command(TARGET morph_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/packaging/msix/Assets"
            "$<TARGET_FILE_DIR:morph_app>/Assets"
    COMMENT "Staging the package assets for Morph.App")

# The manifest names morph.shellext.dll, and a registered package whose class
# points at a DLL that is not there is a verb that fails to activate. Every
# binary already lands in one directory (see CMAKE_RUNTIME_OUTPUT_DIRECTORY in
# the root list), so this is an ordering dependency and not a copy: what it
# guarantees is that the DLL exists by the time the layout is staged.
add_dependencies(morph_app morph_shellext)
