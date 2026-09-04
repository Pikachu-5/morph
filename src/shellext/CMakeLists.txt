# morph.shellext.dll — the Explorer context menu (architecture section 9).
#
# The menu LOGIC is a separate static library from the COM shell around it, so
# that what the menu decides can be tested without Explorer, a surrogate, or a
# registered class. Everything COM is in the DLL; everything decidable is not.

add_library(morph_shellext_core STATIC menu.cpp)
add_library(morph::shellext::core ALIAS morph_shellext_core)

target_include_directories(morph_shellext_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morph_shellext_core PUBLIC morph::core morph::settings)
set_target_properties(morph_shellext_core PROPERTIES FOLDER "processes")

add_library(morph_shellext SHARED
    dllmain.cpp
    command.cpp
    shellext.def
)

target_link_libraries(morph_shellext PRIVATE
    morph::shellext::core
    # RuntimeObject carries the WRL helpers; shlwapi and the shell headers come
    # with the SDK.
    shlwapi
    runtimeobject)

set_target_properties(morph_shellext PROPERTIES
    FOLDER "processes"
    OUTPUT_NAME "morph.shellext")
