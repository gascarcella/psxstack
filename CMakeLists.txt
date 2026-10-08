# psxstack: a game repo adds this directory (add_subdirectory(psxstack) or include(psxstack/cmake/psxstack.cmake))
# and calls psxstack_add_game() with its inputs (cmake/psxstack.cmake; GAME_CONTRACT.md "5. The build inputs").
# The launcher is its own project (launcher/CMakeLists.txt); the disc-free example is examples/hello.
cmake_minimum_required(VERSION 3.20)
project(psxstack C)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/psxstack.cmake")
