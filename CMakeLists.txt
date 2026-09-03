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
endif()

# --- Components --------------------------------------------------------------
add_subdirectory(src/core)

if(MORPH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
