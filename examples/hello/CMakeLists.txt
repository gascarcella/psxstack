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
# -DHELLO_DISC_REQUIRED=ON (tests/hello_test.py --menu): hello described as a game that needs its disc (game.json's
# disc_required, with a made-up disc no image matches), so a run without one starts no game: the input self-test on
# the pump alone. Its Steam artwork is left out (the paths are relative to the original description).
option(HELLO_DISC_REQUIRED "hello as a disc_required game, for the no-game window path" OFF)
set(HELLO_GAME_JSON "${CMAKE_CURRENT_SOURCE_DIR}/game.json")
if(HELLO_DISC_REQUIRED)
    file(READ "${HELLO_GAME_JSON}" _hello_json)
    string(JSON _hello_json SET "${_hello_json}" disc_required "true")
    string(JSON _hello_json SET "${_hello_json}" discs
           [=[[{ "label": "no disc at all", "serial": "TEST-00000", "sha1": "0000000000000000000000000000000000000000", "size": 2352, "region": "NTSC-U" }]]=])
    string(JSON _hello_json REMOVE "${_hello_json}" launcher steam_art)
    set(HELLO_GAME_JSON "${CMAKE_BINARY_DIR}/gen/game.json")
    file(WRITE "${HELLO_GAME_JSON}" "${_hello_json}\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/game.json")
endif()
psxstack_add_game(hello
    GAME_JSON "${HELLO_GAME_JSON}"
    UNITS "${HELLO_GEN}/units.txt"
    MAIN_UNIT "${CMAKE_CURRENT_SOURCE_DIR}/hello.c"
    ADAPTER "${CMAKE_CURRENT_SOURCE_DIR}/adapter.c"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include")
