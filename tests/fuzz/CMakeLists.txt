# The fuzz targets (architecture section 16).
#
# The bodies are a library, linked into the ordinary test suite so the
# deterministic replay in tests/unit/test_hostile_input.cpp drives the same
# entry points a fuzzer would. That matters: a target that only exists in a
# libFuzzer build is one that stops compiling the week nobody runs it.
add_library(morph_fuzz_targets STATIC targets.cpp)
add_library(morph::fuzz::targets ALIAS morph_fuzz_targets)

target_include_directories(morph_fuzz_targets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# opc.h exposes pugixml in its public interface, and morph.provider.docooxml
# links it privately, so anything including that header needs the include path
# itself. Named here rather than inherited by accident.
find_package(pugixml CONFIG REQUIRED)
if(TARGET pugixml::static)
    set(MORPH_PUGIXML_TARGET pugixml::static)
else()
    set(MORPH_PUGIXML_TARGET pugixml::pugixml)
endif()

target_link_libraries(morph_fuzz_targets PUBLIC
    morph::core
    morph::settings
    morph::provider::docmd
    morph::provider::dochtml
    morph::provider::docooxml
    ${MORPH_PUGIXML_TARGET})

set_target_properties(morph_fuzz_targets PROPERTIES FOLDER "tests")

# The real fuzzers, one binary per target.
#
# libFuzzer on Windows needs clang-cl; MSVC has no equivalent. Section 16 runs
# these continuously rather than in PR CI, so a build without clang-cl is not
# missing anything a commit depends on -- and is told so rather than silently
# producing nothing.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    foreach(target markdown html opc docx frame caps)
        set(name morph_fuzz_${target})
        add_executable(${name} fuzz_main.cpp)
        target_link_libraries(${name} PRIVATE morph::fuzz::targets)
        target_compile_definitions(${name} PRIVATE
            "MORPH_FUZZ_TARGET=morph::fuzz::Fuzz${target}")
        target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address)
        target_link_options(${name} PRIVATE -fsanitize=fuzzer,address)
        set_target_properties(${name} PROPERTIES
            FOLDER "tests/fuzz"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/fuzz")
    endforeach()
else()
    message(STATUS
        "Fuzz binaries need clang-cl; building the targets for replay only. "
        "The deterministic corpus still runs in the test suite.")
endif()
