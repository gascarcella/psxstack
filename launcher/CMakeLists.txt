# The launcher (docs/LAUNCHER.md "Launcher"; launcher/README.md): Dear ImGui on SDL3 + SDL_Renderer, C++ (the game stays
# C). Its own CMake project, branded from the game's description (GAME_CONTRACT.md "1. game.json"):
#
#   scripts/setup.sh sdl3 imgui
#   cmake -S launcher -B build/launcher -G Ninja [-DPSXSTACK_GAME_JSON=/path/to/game.json]
#   cmake --build build/launcher
#   build/launcher/<id>-launcher            # SDL_VIDEO_DRIVER=offscreen ... --self-test DIR: the headless self-test
#
# PSXSTACK_GAME_JSON (default: examples/dw2003.game.json) is read at configure time by tools/game_gen.py into
# gen/psxstack_game_gen.h (every name, string, hash and number the launcher shows) and gen/psxstack_game.cmake (the id
# that names the executable). A game repo points it at its own description.
#
# It shares two files of the runtime, compiled here as they are (never modified for the launcher): the JSON reader
# (runtime/json.c) and SHA-1 (runtime/sha1.c).
cmake_minimum_required(VERSION 3.20)
project(psxstack_launcher C CXX)

get_filename_component(PSXSTACK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(PSXSTACK_GEN "${CMAKE_BINARY_DIR}/gen")

# ---- The game's description -> the generated header and the CMake variables (tools/game_gen.py).
set(PSXSTACK_GAME_JSON "${PSXSTACK_ROOT}/examples/dw2003.game.json" CACHE FILEPATH
    "The game's description (schema/game.schema.json): the launcher's branding, discs and rates")
find_package(Python3 3.8 COMPONENTS Interpreter REQUIRED)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PSXSTACK_GAME_JSON}"
             "${PSXSTACK_ROOT}/tools/game_gen.py" "${PSXSTACK_ROOT}/schema/game.schema.json")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${PSXSTACK_ROOT}/tools/game_gen.py" "${PSXSTACK_GAME_JSON}"
                        --out "${PSXSTACK_GEN}/psxstack_game_gen.h" --cmake "${PSXSTACK_GEN}/psxstack_game.cmake"
                RESULT_VARIABLE r OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT r EQUAL 0)
    message(FATAL_ERROR "tools/game_gen.py ${PSXSTACK_GAME_JSON} failed:\n${out}${err}")
endif()
include("${PSXSTACK_GEN}/psxstack_game.cmake")   # PSXSTACK_GAME_ID, PSXSTACK_GAME_TITLE, PSXSTACK_GAME_ENV_PREFIX
set(PSXSTACK_LAUNCHER "${PSXSTACK_GAME_ID}-launcher")
message(STATUS "Game: ${PSXSTACK_GAME_TITLE} (${PSXSTACK_GAME_ID}; ${PSXSTACK_GAME_JSON}) -> ${PSXSTACK_LAUNCHER}")

# ---- Built tools: this checkout's tools/ (scripts/setup.sh, or scripts/dev_link_tools.sh linking another checkout's),
# the main checkout's for a worktree, and PSXSTACK_TOOLS_DIR when given.
set(PSXSTACK_TOOLS_DIR "" CACHE PATH "A tools/ directory holding sdl3/, imgui/, ... (else this checkout's tools/)")
set(PSXSTACK_TOOL_DIRS "")
if(PSXSTACK_TOOLS_DIR)
    list(APPEND PSXSTACK_TOOL_DIRS "${PSXSTACK_TOOLS_DIR}")
endif()
list(APPEND PSXSTACK_TOOL_DIRS "${PSXSTACK_ROOT}/tools")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PSXSTACK_ROOT}" rev-parse --path-format=absolute --git-common-dir
                    OUTPUT_VARIABLE PSXSTACK_GIT_COMMON OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE r)
    if(r EQUAL 0 AND PSXSTACK_GIT_COMMON)
        get_filename_component(PSXSTACK_MAIN_ROOT "${PSXSTACK_GIT_COMMON}" DIRECTORY)
        list(APPEND PSXSTACK_TOOL_DIRS "${PSXSTACK_MAIN_ROOT}/tools")
    endif()
endif()

# SDL3: SDL3_DIR or CMAKE_PREFIX_PATH when given, else the pinned build (scripts/setup.sh sdl3).
set(PSXSTACK_SDL_HINTS "")
foreach(d IN LISTS PSXSTACK_TOOL_DIRS)
    list(APPEND PSXSTACK_SDL_HINTS "${d}/sdl3")
endforeach()
find_package(SDL3 3.2 CONFIG REQUIRED HINTS ${PSXSTACK_SDL_HINTS})
message(STATUS "SDL3 ${SDL3_VERSION}: ${SDL3_DIR}")

# Dear ImGui: PSXSTACK_IMGUI_DIR when given, else the pinned clone (scripts/setup.sh imgui).
set(PSXSTACK_IMGUI_DIR "" CACHE PATH "Dear ImGui's source directory (imgui.cpp, backends/)")
if(NOT PSXSTACK_IMGUI_DIR)
    foreach(d IN LISTS PSXSTACK_TOOL_DIRS)
        if(EXISTS "${d}/imgui/imgui.cpp")
            set(PSXSTACK_IMGUI_DIR "${d}/imgui")
            break()
        endif()
    endforeach()
endif()
if(NOT EXISTS "${PSXSTACK_IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR "Dear ImGui not found: run scripts/setup.sh imgui (or set PSXSTACK_IMGUI_DIR)")
endif()
message(STATUS "Dear ImGui: ${PSXSTACK_IMGUI_DIR}")

add_library(psxstack_imgui STATIC
            "${PSXSTACK_IMGUI_DIR}/imgui.cpp" "${PSXSTACK_IMGUI_DIR}/imgui_draw.cpp" "${PSXSTACK_IMGUI_DIR}/imgui_tables.cpp"
            "${PSXSTACK_IMGUI_DIR}/imgui_widgets.cpp" "${PSXSTACK_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
            "${PSXSTACK_IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp")
target_include_directories(psxstack_imgui SYSTEM PUBLIC "${PSXSTACK_IMGUI_DIR}" "${PSXSTACK_IMGUI_DIR}/backends")
target_compile_features(psxstack_imgui PUBLIC cxx_std_17)
target_link_libraries(psxstack_imgui PUBLIC SDL3::SDL3)

# ---- The launcher's version stamp (launcher_version, launcher_commit: the Copy text's header), from git describe at
# every build (cmake/version.cmake rewrites the file only when it changed). PSXSTACK_VERSION_ROOT: the repository whose
# tags and commit name the build; a game repo passes its own, the stack alone names itself.
set(PSXSTACK_VERSION_ROOT "${PSXSTACK_ROOT}" CACHE PATH "The repository `git describe` names the build after")
set(PSXSTACK_LAUNCHER_VERSION_C "${PSXSTACK_GEN}/launcher_version.c")
add_custom_target(psxstack_launcher_version COMMAND "${CMAKE_COMMAND}" -DROOT=${PSXSTACK_VERSION_ROOT}
                  -DOUT=${PSXSTACK_LAUNCHER_VERSION_C} -DPREFIX=launcher -P "${PSXSTACK_ROOT}/cmake/version.cmake"
                  BYPRODUCTS "${PSXSTACK_LAUNCHER_VERSION_C}"
                  COMMENT "cmake/version.cmake (the launcher's version stamp)" VERBATIM)

# ---- The self-test's fixture manifests (tests/fixtures/mods/<id>/mod.json) as string constants, so the self-test
# needs no files beside the binary (it runs inside a release package and under Wine).
set(PSXSTACK_FIXTURES_H "${PSXSTACK_GEN}/selftest_fixtures.h")
set(fixtures "/* Generated by launcher/CMakeLists.txt from tests/fixtures/mods/; do not edit. */\n#pragma once\n")
foreach(id fast_forward skip_dialogues every_type presets)
    set(f "${PSXSTACK_ROOT}/tests/fixtures/mods/${id}/mod.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${f}")
    file(READ "${f}" text)
    string(TOUPPER "${id}" ID)
    string(APPEND fixtures "static const char ${ID}_MANIFEST[] = R\"json(${text})json\";\n")
endforeach()
file(CONFIGURE OUTPUT "${PSXSTACK_FIXTURES_H}" CONTENT "${fixtures}" @ONLY NEWLINE_STYLE UNIX)

# ---- The executable: <id>-launcher.
file(GLOB PSXSTACK_LAUNCHER_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
set(PSXSTACK_LAUNCHER_SHARED "${PSXSTACK_ROOT}/runtime/json.c" "${PSXSTACK_ROOT}/runtime/sha1.c"
    "${PSXSTACK_LAUNCHER_VERSION_C}")
add_executable(${PSXSTACK_LAUNCHER} ${PSXSTACK_LAUNCHER_SRCS} ${PSXSTACK_LAUNCHER_SHARED})
add_dependencies(${PSXSTACK_LAUNCHER} psxstack_launcher_version)
set_target_properties(${PSXSTACK_LAUNCHER} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
                      C_STANDARD 99)
target_include_directories(${PSXSTACK_LAUNCHER} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${PSXSTACK_GEN}"
                           "${PSXSTACK_ROOT}/include/psxstack")
target_link_libraries(${PSXSTACK_LAUNCHER} PRIVATE psxstack_imgui SDL3::SDL3)
set_source_files_properties(${PSXSTACK_LAUNCHER_SRCS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
# The release links the C++ runtime statically, so an AppImage needs only the C library of the player's system
# (libstdc++ and libgcc_s are newer on the build machine than on older desktops). The GCC runtime library exception
# covers it: no notice is owed.
option(PSXSTACK_LAUNCHER_STATIC_RUNTIME "Link libstdc++ and libgcc statically (the release)" OFF)
if(PSXSTACK_LAUNCHER_STATIC_RUNTIME)
    target_link_options(${PSXSTACK_LAUNCHER} PRIVATE -static-libstdc++ -static-libgcc)
endif()

# ---- Windows (cmake/windows-x86_64.cmake): a GUI-subsystem program (no console window behind the launcher; SDL_main.h
# keeps `main` the entry) with the manifest resource (launcher/windows/: the UTF-8 code page, long paths, per-monitor
# DPI awareness), both configured from the game's id and compiled by the toolchain's windres.
if(WIN32)
    enable_language(RC)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/windows/launcher.manifest.in"
                   "${PSXSTACK_GEN}/windows/${PSXSTACK_LAUNCHER}.manifest" @ONLY)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/windows/launcher.rc.in"
                   "${PSXSTACK_GEN}/windows/${PSXSTACK_LAUNCHER}.rc" @ONLY)
    target_sources(${PSXSTACK_LAUNCHER} PRIVATE "${PSXSTACK_GEN}/windows/${PSXSTACK_LAUNCHER}.rc")
    set_target_properties(${PSXSTACK_LAUNCHER} PROPERTIES WIN32_EXECUTABLE ON)
endif()
