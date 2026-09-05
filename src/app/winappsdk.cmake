# Finding the Windows App SDK, and generating a C++/WinRT projection from it.
#
# WHY THIS FILE EXISTS. Section 12 asks for WinUI 3, and WinUI 3's tooling is
# MSBuild: a .vcxproj, a NuGet restore, and a XAML compiler that runs as an
# MSBuild task. This repository is CMake and Ninja everywhere else, and section
# 18's whole argument for tools/build.ps1 is that CI drives the same script a
# developer does. A second build system for one executable would break that on
# the day it mattered.
#
# It turns out not to be necessary, and the reason is worth writing down because
# it is not obvious: the XAML COMPILER is only needed for XAML MARKUP. An app
# whose UI is built in code needs no .xaml file, no .xbf, no generated IDL and no
# MSBuild task -- only the C++/WinRT projection, which cppwinrt.exe generates
# from .winmd files and which CMake can run like any other code generator.
#
# So this file does three things, all of them ordinary CMake:
#
#   1. locates the SDK's metadata, headers and import libraries;
#   2. runs cppwinrt.exe over the .winmd files to produce winrt/*.h;
#   3. hands back the include directories, libraries and runtime DLLs.
#
# WHAT IT COSTS, stated plainly so nobody rediscovers it the hard way. The UI is
# written in C++ rather than in markup, which means no XAML hot reload and no
# designer -- neither of which section 12.3 was counting on ("less reliable hot
# reload" is on its list already). In exchange the app builds with `ninja`, in
# the same tree, from the same script, on the same machine as everything else.
#
# WHAT THIS DOES NOT DO is make WinUI's theme resources resolve. That needs a
# real IXamlMetadataProvider on the application object, and it is main.cpp's
# problem rather than the build's; see the note above `struct App` there, which
# is the single hardest-won paragraph in this directory.
#
# THE PACKAGES ARE PINNED, in third_party/winappsdk-packages.txt: a version and
# a SHA-256 per package, the shape tools/fetch-pdfium.ps1 already had.
# tools/fetch-winappsdk.ps1 downloads and verifies them into
# third_party/winappsdk, tools/scan-licenses.ps1 checks their licences, and this
# file reads the same manifest to find them. Three copies of five version
# numbers is how a build ends up linking one release of the SDK against the
# metadata of another, which does not fail at the link -- it fails at run time,
# with a type that does not activate.
#
# The local NuGet cache is still searched, second. A developer who has opened a
# WinUI project already has these packages, and making them download 90 MB they
# have on disk would be a worse first build, not a better one. The pin decides
# WHICH version is used either way; the roots only decide where it is found.

set(MORPH_WINAPPSDK_MANIFEST "${PROJECT_SOURCE_DIR}/third_party/winappsdk-packages.txt"
    CACHE FILEPATH "The pinned Windows App SDK package list")

set(MORPH_WINAPPSDK_ROOTS
    "${PROJECT_SOURCE_DIR}/third_party/winappsdk"
    "$ENV{USERPROFILE}/.nuget/packages"
    CACHE STRING "Directories searched for the pinned packages, in order")

# The runtime the bootstrapper asks for, as the packed major/minor the API takes:
# 0x00010008 is 1.8, 0x00020000 is 2.0. It must match the release the manifest's
# versions come from, because the headers, the .winmd files and the framework
# package the process resolves at run time all have to be one release of one
# product. It is here rather than in the manifest because it is a property of the
# release as a whole and not of any one package in it.
set(MORPH_WINAPPSDK_MAJORMINOR "0x00010008" CACHE STRING "Windows App Runtime major.minor, packed")

# The same release again, in the two spellings the PACKAGE manifest needs. An
# unpackaged app bootstraps the runtime with the packed number above; a packaged
# one declares a framework dependency by name and minimum version, and the
# deployment stack puts it in the package graph before the process starts. Get
# these wrong and there is nothing to debug: the process is created and dies
# before main, with no window and nothing written anywhere.
#
# MINVERSION is the runtime build that goes with the SDK packages pinned in the
# manifest, not "whatever is installed". Raising the pin raises this.
set(MORPH_WINAPPRUNTIME_NAME "Microsoft.WindowsAppRuntime.1.8"
    CACHE STRING "The framework package a packaged Morph declares")
set(MORPH_WINAPPRUNTIME_MINVERSION "8000.946.1701.0"
    CACHE STRING "Minimum Windows App Runtime build, matching the pinned SDK")

# Reads the manifest and sets, per package, MORPH_PKG_<ID> to the directory it
# was found in and MORPH_VER_<ID> to its pinned version -- with the id
# upper-cased and its dots turned into underscores. Not a function: the callers
# below want these at directory scope, and a function would need a PARENT_SCOPE
# for each of ten variables whose names it computes.
if(NOT EXISTS "${MORPH_WINAPPSDK_MANIFEST}")
    message(FATAL_ERROR "The Windows App SDK pin manifest is missing: ${MORPH_WINAPPSDK_MANIFEST}")
endif()

set(MORPH_WINAPPSDK_PINNED "")
file(STRINGS "${MORPH_WINAPPSDK_MANIFEST}" _morph_manifest_lines)
foreach(_line IN LISTS _morph_manifest_lines)
    string(STRIP "${_line}" _line)
    if(_line STREQUAL "" OR _line MATCHES "^#")
        continue()
    endif()
    string(REPLACE "|" ";" _fields "${_line}")
    list(LENGTH _fields _count)
    if(NOT _count EQUAL 5)
        message(FATAL_ERROR "Malformed line in ${MORPH_WINAPPSDK_MANIFEST}:\n  ${_line}")
    endif()
    list(GET _fields 0 _id)
    list(GET _fields 1 _version)

    string(TOUPPER "${_id}" _key)
    string(REPLACE "." "_" _key "${_key}")
    string(TOLOWER "${_id}" _lower)

    set(MORPH_VER_${_key} "${_version}")
    set(MORPH_PKG_${_key} "")
    foreach(_root IN LISTS MORPH_WINAPPSDK_ROOTS)
        if(EXISTS "${_root}/${_lower}/${_version}")
            set(MORPH_PKG_${_key} "${_root}/${_lower}/${_version}")
            break()
        endif()
    endforeach()
    list(APPEND MORPH_WINAPPSDK_PINNED "${_id}")
endforeach()

# Named so the rest of the file reads the way it did before the manifest.
set(_winui       "${MORPH_PKG_MICROSOFT_WINDOWSAPPSDK_WINUI}")
set(_foundation  "${MORPH_PKG_MICROSOFT_WINDOWSAPPSDK_FOUNDATION}")
set(_interactive "${MORPH_PKG_MICROSOFT_WINDOWSAPPSDK_INTERACTIVEEXPERIENCES}")
set(_cppwinrt    "${MORPH_PKG_MICROSOFT_WINDOWS_CPPWINRT}")
set(_webview2    "${MORPH_PKG_MICROSOFT_WEB_WEBVIEW2}")

# Returns TRUE in `out` when every piece is present, and sets the paths used
# below. Reports what is missing rather than failing, because MORPH_BUILD_APP is
# off by default and a machine without the SDK must still build everything else.
function(morph_find_winappsdk out)
    set(missing "")

    set(winui       "${_winui}")
    set(foundation  "${_foundation}")
    set(interactive "${_interactive}")
    set(cppwinrt    "${_cppwinrt}/bin/cppwinrt.exe")
    set(webview2md  "${_webview2}/lib/Microsoft.Web.WebView2.Core.winmd")

    if(_cppwinrt STREQUAL "" OR NOT EXISTS "${cppwinrt}")
        list(APPEND missing "Microsoft.Windows.CppWinRT ${MORPH_VER_MICROSOFT_WINDOWS_CPPWINRT}")
    endif()
    if(_winui STREQUAL "" OR NOT EXISTS "${winui}/metadata")
        list(APPEND missing "Microsoft.WindowsAppSDK.WinUI ${MORPH_VER_MICROSOFT_WINDOWSAPPSDK_WINUI}")
    endif()
    if(_foundation STREQUAL "" OR NOT EXISTS "${foundation}/include")
        list(APPEND missing
             "Microsoft.WindowsAppSDK.Foundation ${MORPH_VER_MICROSOFT_WINDOWSAPPSDK_FOUNDATION}")
    endif()
    if(_interactive STREQUAL "" OR NOT EXISTS "${interactive}/metadata")
        list(APPEND missing
             "Microsoft.WindowsAppSDK.InteractiveExperiences ${MORPH_VER_MICROSOFT_WINDOWSAPPSDK_INTERACTIVEEXPERIENCES}")
    endif()
    # Microsoft.UI.Xaml.WebView2 wraps CoreWebView2, so the projection does not
    # close without WebView2's own metadata -- even though nothing in the app
    # uses it. vcpkg's webview2 port ships headers and no .winmd, so this one
    # comes from NuGet like the rest.
    if(_webview2 STREQUAL "" OR NOT EXISTS "${webview2md}")
        list(APPEND missing "Microsoft.Web.WebView2 ${MORPH_VER_MICROSOFT_WEB_WEBVIEW2}")
    endif()

    # The Windows SDK's own metadata, for the Windows.* half of the projection.
    file(GLOB union_metadata "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/UnionMetadata/10.*")
    if(union_metadata)
        list(SORT union_metadata)
        list(GET union_metadata -1 windows_metadata)
    else()
        list(APPEND missing "the Windows SDK's UnionMetadata")
    endif()

    if(missing)
        list(JOIN missing "\n    " report)
        message(STATUS
            "Morph.App.exe is not being built. Missing:\n    ${report}\n"
            "  Run tools\\fetch-winappsdk.ps1 to fetch the pinned packages, or configure with\n"
            "  -DMORPH_BUILD_APP=OFF to stop asking.")
        set(${out} FALSE PARENT_SCOPE)
        return()
    endif()

    # The interactive-experiences metadata is split per target platform version
    # and cppwinrt must be given exactly one of them: pointed at the parent it
    # reads both and the duplicate types make it fail with an error that names a
    # type rather than the directory.
    file(GLOB interactive_tfms "${interactive}/metadata/10.*")
    list(SORT interactive_tfms)
    list(GET interactive_tfms -1 interactive_metadata)

    set(MORPH_WINAPPSDK_METADATA
        "${windows_metadata}"
        "${winui}/metadata"
        "${foundation}/metadata"
        "${interactive_metadata}"
        "${webview2md}"
        PARENT_SCOPE)
    set(MORPH_WINAPPSDK_INCLUDE "${foundation}/include" "${winui}/include" PARENT_SCOPE)
    set(MORPH_WINAPPSDK_LIB "${foundation}/lib/native/x64/Microsoft.WindowsAppRuntime.Bootstrap.lib" PARENT_SCOPE)
    # An import library, so the DLL has to sit beside the executable -- the same
    # arrangement pdfium.dll has, and the same failure when it does not: the
    # process dies before main with no message at all.
    set(MORPH_WINAPPSDK_DLL
        "${foundation}/runtimes/win-x64/native/Microsoft.WindowsAppRuntime.Bootstrap.dll"
        PARENT_SCOPE)

    # The framework's own resource indexes, which go BESIDE the executable rather
    # than into a resources.pri of ours: Microsoft.WinUI.References.targets
    # copies "$(_WinUIDllDir)*.pri" to the output directory and merges nothing,
    # so this does the same.
    #
    # These are NOT what makes theme resources resolve -- that is the metadata
    # provider in main.cpp, and this was copied while looking for it. They are
    # kept because the SDK ships them to be shipped and an app that reaches for a
    # localised framework string without them finds nothing.
    set(MORPH_WINAPPSDK_PRI
        "${winui}/runtimes-framework/win-x64/native/Microsoft.UI.Xaml.Controls.pri"
        "${interactive}/runtimes-framework/win-x64/native/Microsoft.UI.pri"
        "${foundation}/runtimes-framework/win-x64/native/Microsoft.WindowsAppRuntime.pri"
        PARENT_SCOPE)
    set(MORPH_CPPWINRT_EXE "${cppwinrt}" PARENT_SCOPE)
    set(${out} TRUE PARENT_SCOPE)
endfunction()

# Generates the projection into ${output} and returns a target to depend on.
function(morph_generate_cppwinrt target output)
    set(inputs "")
    foreach(metadata ${MORPH_WINAPPSDK_METADATA})
        list(APPEND inputs -input "${metadata}")
    endforeach()

    # Stamped rather than tracked file by file: cppwinrt writes about sixteen
    # hundred headers and listing them as OUTPUT would put sixteen hundred
    # entries in the build graph to express one dependency.
    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/cppwinrt.stamp")
    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${output}"
        COMMAND "${MORPH_CPPWINRT_EXE}" ${inputs} -output "${output}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS "${MORPH_CPPWINRT_EXE}"
        COMMENT "Generating the C++/WinRT projection for the Windows App SDK"
        VERBATIM)

    add_custom_target(${target} DEPENDS "${stamp}")
endfunction()
