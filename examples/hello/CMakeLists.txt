# examples/hello: a disc-free Psy-Q program built through psxstack_add_game() (cmake/psxstack.cmake), the stack's
# own smoke test (tests/hello_test.py; CI). One unit, one slot, no overlays, no disc; the adapter only maps a probe for
# the script check (adapter.c):
#
#   cmake -S examples/hello -B build/hello -G Ninja && cmake --build build/hello
#   build/hello/hello --max-frames 40 --screenshot 30:shot.ppm
cmake_minimum_required(VERSION 3.20)
project(hello C)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/psxstack.cmake")
set(HELLO_GEN "${CMAKE_BINARY_DIR}/gen/inputs")
file(WRITE "${HELLO_GEN}/units.txt" "${CMAKE_CURRENT_SOURCE_DIR}/hello.c\tMAIN\n")
psxstack_add_game(hello
    GAME_JSON "${CMAKE_CURRENT_SOURCE_DIR}/game.json"
    UNITS "${HELLO_GEN}/units.txt"
    MAIN_UNIT "${CMAKE_CURRENT_SOURCE_DIR}/hello.c"
    ADAPTER "${CMAKE_CURRENT_SOURCE_DIR}/adapter.c"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include")
