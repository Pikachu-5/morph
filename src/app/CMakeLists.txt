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
