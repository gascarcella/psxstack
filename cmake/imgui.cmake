# Dear ImGui as a CMake target, psxstack_imgui (MIT; docs/THIRD_PARTY.md): the core and its SDL3 backends (the
# platform backend, SDL_Renderer and SDL_GPU), linked to SDL3::SDL3, C++17. Shared by the launcher
# (launcher/CMakeLists.txt) and the game's in-game menu (cmake/psxstack.cmake, PSXSTACK_MENU), each of which
# calls psxstack_imgui_target() after it found SDL3; the second call finds the target made and does nothing.
#
# The source: PSXSTACK_IMGUI_DIR when given, else the pinned clone (scripts/setup.sh imgui) in the first of the
# PSXSTACK_TOOL_DIRS (this checkout's tools/, PSXSTACK_TOOLS_DIR, the main checkout's in a worktree) holding one.
set(PSXSTACK_IMGUI_DIR "" CACHE PATH "Dear ImGui's source directory (imgui.cpp, backends/)")

function(psxstack_imgui_target)
    if(TARGET psxstack_imgui)
        return()
    endif()
    set(_dir "${PSXSTACK_IMGUI_DIR}")
    if(NOT _dir)
        foreach(d IN LISTS PSXSTACK_TOOL_DIRS)
            if(EXISTS "${d}/imgui/imgui.cpp")
                set(_dir "${d}/imgui")
                break()
            endif()
        endforeach()
    endif()
    if(NOT EXISTS "${_dir}/imgui.cpp")
        message(FATAL_ERROR "Dear ImGui not found: run scripts/setup.sh imgui (or set PSXSTACK_IMGUI_DIR)")
    endif()
    message(STATUS "Dear ImGui: ${_dir}")
    add_library(psxstack_imgui STATIC
                "${_dir}/imgui.cpp" "${_dir}/imgui_draw.cpp" "${_dir}/imgui_tables.cpp" "${_dir}/imgui_widgets.cpp"
                "${_dir}/backends/imgui_impl_sdl3.cpp" "${_dir}/backends/imgui_impl_sdlrenderer3.cpp"
                "${_dir}/backends/imgui_impl_sdlgpu3.cpp")
    target_include_directories(psxstack_imgui SYSTEM PUBLIC "${_dir}" "${_dir}/backends")
    target_compile_features(psxstack_imgui PUBLIC cxx_std_17)
    target_link_libraries(psxstack_imgui PUBLIC SDL3::SDL3)
    set(PSXSTACK_IMGUI_DIR_FOUND "${_dir}" PARENT_SCOPE)
endfunction()
