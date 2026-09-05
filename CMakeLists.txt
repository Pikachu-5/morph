cmake_minimum_required(VERSION 3.25)

project(morph
    VERSION 0.1.0
    DESCRIPTION "A native Windows file conversion engine"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Morph targets Windows only (see architecture section 1).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Everything is /MD (dynamic CRT). The Windows App SDK, WebView2 and the shell
# extension surrogate all require it, and mixing CRTs across the four processes
# in section 3 is not worth the static-link savings.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Every executable lands in one directory. The broker starts the worker by
# looking beside itself (section 10.3 starts it on demand, with no install
# database to consult), and a layout that mirrors the source tree would put them
# in sibling directories where neither can find the other.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# The context menu's class id, in the one spelling the package manifest needs.
#
# It is the single joint between src/shellext and packaging/msix: the manifest
# declares the class, the DLL implements it, and if the two strings differ the
# result is a verb that registers a class nobody implements -- which Explorer
# reports as nothing at all, on a machine where everything looks installed.
#
# The C++ side is still the GUID literal in src/shellext/shellext.h, because a
# generated header for one constant is worse than a test. The test is
# test_shell_launch.cpp, which parses this string and compares it with what the
# DLL was compiled with, so the two cannot drift without the suite going red.
set(MORPH_SHELLEXT_CLSID "6F3E1D0A-9C4B-4E58-9A2F-0B5E7C1D8A34"
    CACHE STRING "CLSID of the Explorer context menu handler")

option(MORPH_BUILD_TESTS "Build the test suite" ON)
option(MORPH_ASAN        "Build with AddressSanitizer" OFF)
option(MORPH_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)

# --- Shared compile settings -------------------------------------------------
# Applied through an interface target rather than global flags so that vcpkg
# dependencies are not built with our warning level.
add_library(morph_settings INTERFACE)
add_library(morph::settings ALIAS morph_settings)

target_compile_features(morph_settings INTERFACE cxx_std_20)

target_compile_definitions(morph_settings INTERFACE
    # The one place the product version is written down. It reaches the wire in
    # Hello and WorkerHello, where a version mismatch is diagnosed by a human
    # reading a log rather than by the protocol (section 10.2 versions the
    # PROTOCOL separately, and that constant lives in protocol.h).
    MORPH_VERSION="${PROJECT_VERSION}"
    UNICODE _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    # Windows 11 21H2 is the floor. Section 20.3: Windows 10 is out of scope for
    # v1, so we do not carry the legacy IContextMenu path or the acrylic
    # fallback. Raising this constant is how that decision is enforced.
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    NTDDI_VERSION=0x0A00000C)

if(MSVC)
    target_compile_options(morph_settings INTERFACE
        /W4
        /permissive-        # conformance; C++/WinRT needs it anyway
        /Zc:__cplusplus     # or __cplusplus reports 199711L
        /Zc:preprocessor
        /utf-8
        /EHsc
        /MP
        # Section 15.1: control-flow guard and CET shadow stack. Set at compile
        # time here and mirrored at link time below.
        /guard:cf)
    target_link_options(morph_settings INTERFACE
        /guard:cf
        /CETCOMPAT
        /DYNAMICBASE
        /HIGHENTROPYVA)

    if(MORPH_WARNINGS_AS_ERRORS)
        target_compile_options(morph_settings INTERFACE /WX)
    endif()
endif()

if(MORPH_ASAN AND MSVC)
    target_compile_options(morph_settings INTERFACE /fsanitize=address)
    # ASan is incompatible with incremental linking and with /RTC.
    target_link_options(morph_settings INTERFACE /INCREMENTAL:NO)

    # Our code is instrumented and vcpkg's dependencies are not, and the MSVC
    # standard library's container annotations have to agree across every
    # translation unit that touches a std::vector or std::string. They do not
    # here -- pugixml's objects were compiled without them -- and the link fails
    # with LNK2038 "mismatch detected for 'annotate_vector'" on a target that has
    # nothing obviously to do with either.
    #
    # Turning the annotations off is the documented answer and costs one class of
    # finding: a container overflow that stays inside the allocation. Everything
    # else ASan catches -- heap overflows, use-after-free, stack overflows, the
    # bugs a fuzzer is actually looking for -- is unaffected. The alternative is
    # an ASan build that does not link, which finds nothing at all.
    target_compile_definitions(morph_settings INTERFACE
        _DISABLE_STRING_ANNOTATION=1
        _DISABLE_VECTOR_ANNOTATION=1)
endif()

# --- The application manifest ------------------------------------------------
#
# Embedded in all three executables. See packaging/morph.manifest for what is in
# it and why; the short version is that a process without one is DPI unaware and
# Windows renders it at 96 DPI and stretches the result.
#
# MSVC's linker takes a manifest as an ordinary source file and embeds it, so
# there is no mt.exe step and nothing to keep in step by hand.
set(MORPH_APP_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/morph.manifest")

function(morph_apply_manifest target)
    if(MSVC)
        target_sources(${target} PRIVATE "${MORPH_APP_MANIFEST}")
    endif()
endfunction()

# --- Components --------------------------------------------------------------
add_subdirectory(src/core)
add_subdirectory(src/providers)
add_subdirectory(src/worker)
add_subdirectory(src/broker)
add_subdirectory(src/shellext)
add_subdirectory(src/app)

if(MORPH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
