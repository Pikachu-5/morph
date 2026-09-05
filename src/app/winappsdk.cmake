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
# THE PACKAGES ARE NOT PINNED YET, and that is the one loose end. They are read
# from the local NuGet cache, which means this builds on a machine that has
# opened a WinUI project before and nowhere else -- which is why
# MORPH_BUILD_APP defaults to OFF. The fix is the shape tools/fetch-pdfium.ps1
# already has: a pinned version, a SHA-256, and a download that fails loudly.
# That belongs with the packaging milestone, where the same versions have to be
# written into the MSIX manifest anyway.

set(MORPH_NUGET_PACKAGES "$ENV{USERPROFILE}/.nuget/packages"
    CACHE PATH "Where the Windows App SDK NuGet packages are unpacked")

# Read together with the versions the runtime bootstrapper will be asked for in
# main.cpp. The SDK splits into sub-packages that version independently, which is
# why there are four numbers here rather than one.
set(MORPH_WINAPPSDK_WINUI_VERSION       "1.8.260204000" CACHE STRING "Microsoft.WindowsAppSDK.WinUI")
set(MORPH_WINAPPSDK_FOUNDATION_VERSION  "1.8.260203002" CACHE STRING "Microsoft.WindowsAppSDK.Foundation")
set(MORPH_WINAPPSDK_INTERACTIVE_VERSION "1.8.260125001" CACHE STRING "Microsoft.WindowsAppSDK.InteractiveExperiences")

# The runtime the bootstrapper asks for, as the packed major/minor the API takes:
# 0x00010008 is 1.8, 0x00020000 is 2.0. It MUST match the line the versions above
# come from. Building against one line's headers and resolving another's
# framework package is not a link error and not a crash at startup either -- it
# is XamlControlsResources failing to find a theme brush that was renamed between
# them, which reads as a bug in the app.
set(MORPH_WINAPPSDK_MAJORMINOR "0x00010008" CACHE STRING "Windows App Runtime major.minor, packed")
set(MORPH_CPPWINRT_VERSION              "2.0.250303.1"  CACHE STRING "Microsoft.Windows.CppWinRT")
set(MORPH_WEBVIEW2_NUGET_VERSION        "1.0.3719.77"   CACHE STRING "Microsoft.Web.WebView2")

# Returns TRUE in `out` when every piece is present, and sets the paths used
# below. Reports what is missing rather than failing, because MORPH_BUILD_APP is
# off by default and a machine without the SDK must still build everything else.
function(morph_find_winappsdk out)
    set(missing "")

    set(winui       "${MORPH_NUGET_PACKAGES}/microsoft.windowsappsdk.winui/${MORPH_WINAPPSDK_WINUI_VERSION}")
    set(foundation  "${MORPH_NUGET_PACKAGES}/microsoft.windowsappsdk.foundation/${MORPH_WINAPPSDK_FOUNDATION_VERSION}")
    set(interactive "${MORPH_NUGET_PACKAGES}/microsoft.windowsappsdk.interactiveexperiences/${MORPH_WINAPPSDK_INTERACTIVE_VERSION}")
    set(cppwinrt    "${MORPH_NUGET_PACKAGES}/microsoft.windows.cppwinrt/${MORPH_CPPWINRT_VERSION}/bin/cppwinrt.exe")
    set(webview2md  "${MORPH_NUGET_PACKAGES}/microsoft.web.webview2/${MORPH_WEBVIEW2_NUGET_VERSION}/lib/Microsoft.Web.WebView2.Core.winmd")

    if(NOT EXISTS "${cppwinrt}")
        list(APPEND missing "Microsoft.Windows.CppWinRT ${MORPH_CPPWINRT_VERSION}")
    endif()
    if(NOT EXISTS "${winui}/metadata")
        list(APPEND missing "Microsoft.WindowsAppSDK.WinUI ${MORPH_WINAPPSDK_WINUI_VERSION}")
    endif()
    if(NOT EXISTS "${foundation}/include")
        list(APPEND missing "Microsoft.WindowsAppSDK.Foundation ${MORPH_WINAPPSDK_FOUNDATION_VERSION}")
    endif()
    if(NOT EXISTS "${interactive}/metadata")
        list(APPEND missing
             "Microsoft.WindowsAppSDK.InteractiveExperiences ${MORPH_WINAPPSDK_INTERACTIVE_VERSION}")
    endif()
    # Microsoft.UI.Xaml.WebView2 wraps CoreWebView2, so the projection does not
    # close without WebView2's own metadata -- even though nothing in the app
    # uses it. vcpkg's webview2 port ships headers and no .winmd, so this one
    # comes from NuGet like the rest.
    if(NOT EXISTS "${webview2md}")
        list(APPEND missing "Microsoft.Web.WebView2 ${MORPH_WEBVIEW2_NUGET_VERSION}")
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
        message(STATUS "Morph.App.exe is not being built. Missing:\n    ${report}")
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
    # than into a resources.pri of ours. That is what the SDK's own MSBuild
    # targets do -- Microsoft.WinUI.References.targets copies "$(_WinUIDllDir)*.pri"
    # to the output directory and merges nothing -- and it took an afternoon of
    # makepri to find out, because without them every templated control fails to
    # resolve its theme brushes and takes the window down on the first layout
    # pass.
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
