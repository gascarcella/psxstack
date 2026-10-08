# examples/tasks: a disc-free Psy-Q program whose work runs in tasks on psxstack's fibers (runtime/fiber.c), the
# stack's test of them (tests/tasks_test.py; CI). One unit, one slot, no overlays, no disc, no adapter; hello's
# common.h (examples/hello/include) serves it too, the Psy-Q declarations are the stack's:
#
#   cmake -S examples/tasks -B build/tasks -G Ninja && cmake --build build/tasks
#   build/tasks/tasks --max-frames 60 --screenshot 45:shot.ppm --log run.log
cmake_minimum_required(VERSION 3.20)
project(tasks C)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/psxstack.cmake")
set(TASKS_GEN "${CMAKE_BINARY_DIR}/gen/inputs")
file(WRITE "${TASKS_GEN}/units.txt" "${CMAKE_CURRENT_SOURCE_DIR}/tasks.c\tMAIN\n")
psxstack_add_game(tasks
    GAME_JSON "${CMAKE_CURRENT_SOURCE_DIR}/game.json"
    UNITS "${TASKS_GEN}/units.txt"
    MAIN_UNIT "${CMAKE_CURRENT_SOURCE_DIR}/tasks.c"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../hello/include")
