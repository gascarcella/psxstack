#!/usr/bin/env bash
# psyq/check.sh: compiles the Psy-Q shim on its own with the port's flags (-Wall -Wextra -Werror), against the stack's
# own Psy-Q declarations (include/psxstack/psyq/), and, given a game's probe objects, checks that it covers every
# Psy-Q symbol the game's host objects need. Needs no CMake or SDL.
#
#   psyq/check.sh [--game-root DIR] [--probe-dir DIR] [--inventory FILE] [--python EXE] [--game-json FILE]
#                 [--compile] [-I DIR]...
#     --game-root DIR   the game checkout, for the coverage: its probe objects say what it needs
#     --probe-dir DIR   the game's probe output (the game's port_inventory.py probe: DIR/include holds the generated
#                       psxstack_game_gen.h, DIR/m64/obj the game's objects); default <game-root>/build/port_inventory
#     --inventory FILE  the game's inventory wrapper (for the coverage); default <game-root>/tools/port_inventory.py
#     --python EXE      the Python that runs it; default <game-root>/tools/venv/bin/python, else <game-root>/.venv/bin/python,
#                       else python3
#     --game-json FILE  the description to generate psxstack_game_gen.h from when the probe directory holds none;
#                       default examples/hello/game.json
#     --compile         compile only: no game needed (the shim compiles against the stack's own declarations,
#                       include/psxstack/psyq/, with --game-json or hello's standing in for the description)
#     -I DIR            more include directories
#   EXTRA="-O2 -fsanitize=address,undefined" psyq/check.sh --compile   # with more flags
# Exit 0: compiled (and, with the coverage, nothing missing and no duplicate global).
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
CC=${CC:-gcc}
EXTRA=${EXTRA:-}
game= probe= inv= py= game_json= compile_only=0 extra_inc=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --game-root) game="$2"; shift 2 ;;
        --probe-dir) probe="$2"; shift 2 ;;
        --inventory) inv="$2"; shift 2 ;;
        --python) py="$2"; shift 2 ;;
        --game-json) game_json="$2"; shift 2 ;;
        --compile) compile_only=1; shift ;;
        -I) extra_inc+=("-I$2"); shift 2 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "check: unknown argument $1" >&2; exit 2 ;;
    esac
done
if [[ -z "$game" && $compile_only -eq 0 ]]; then
    echo "check: --game-root DIR is required for the coverage (--compile needs no game)" >&2; exit 2
fi
if [[ -n "$game" ]]; then
    probe="${probe:-$game/build/port_inventory}"
    inv="${inv:-$game/tools/port_inventory.py}"
    if [[ -z "$py" ]]; then
        for cand in "$game/tools/venv/bin/python" "$game/.venv/bin/python"; do
            [[ -x "$cand" ]] && { py="$cand"; break; }
        done
    fi
else
    probe="${probe:-$HERE/build/psyq_check}"
fi
py="${py:-python3}"
game_json="${game_json:-$HERE/examples/hello/game.json}"
OUT="$probe/psyq"
mkdir -p "$OUT"
# The generated description header (hooks.h includes it): the game's probe wrote one; without a game, hello's.
if [[ -f "$probe/include/psxstack_game_gen.h" ]]; then
    gen_inc="$probe/include"
else
    [[ -z "$game" ]] || { echo "check: $probe/include is missing: run the game's tools/port_inventory.py probe first" >&2; exit 2; }
    gen_inc="$OUT/include"
    mkdir -p "$gen_inc"
    python3 "$HERE/tools/game_gen.py" "$game_json" --out "$gen_inc/psxstack_game_gen.h"
fi
CFLAGS=(-m64 -std=gnu99 -fsigned-char -fwrapv -fno-strict-aliasing -DPC_PORT -Wall -Wextra -Werror
        -I"$gen_inc" -I"$HERE/psyq" -I"$HERE/include" -I"$HERE/include/psxstack" -I"$HERE/runtime" "${extra_inc[@]}")
rm -f "$OUT"/*.o
for src in "$HERE"/psyq/*.c; do
    "$CC" ${EXTRA} "${CFLAGS[@]}" -c "$src" -o "$OUT/$(basename "${src%.c}").o"
done
ar rcs "$OUT/libpsyq.a" "$OUT"/*.o
echo "compile: $(ls "$OUT"/*.o | wc -l) objects -> $OUT/libpsyq.a"
[[ $compile_only -eq 1 ]] && exit 0

# Coverage: the Psy-Q functions the game's objects need (undefined in the probe's objects and named in the game's
# symbol files as library code: the game's tools/port_inventory.py link -v lists them) against what the shim
# defines (open, read, write, lseek and close as its psyq_api_*: include/psxstack/psyq_names.h); what it does not
# define (LIBC2's string functions, ...) is checked against the host libc's exports instead.
if [[ ! -d "$probe/m64/obj" ]]; then
    echo "check: $probe/m64/obj is missing: run the game's tools/port_inventory.py probe" >&2
    exit 2
fi
game_objs=$(find "$probe/m64/obj" -name '*.o')
[[ -f "$inv" ]] || { echo "check: $inv is missing: pass --inventory FILE (the game's port_inventory.py wrapper)" >&2; exit 2; }
# (`link` exits 1 while the game has duplicate or undefined globals: its lists are what is read here)
link_report=$("$py" "$inv" link -v 2>/dev/null || true)
need=$(echo "$link_report" | awk '/Psy-Q function:/{f=1;next} /asm-only data/{f=0} f && $2 ~ /^0x/ {print $1}' | sort -u)
need_data=$(echo "$link_report" | awk '/Psy-Q data:/{f=1;next} /asm-only data/{f=0} f && $2 ~ /^0x/ {print $1}' | sort -u)
have=$(nm -g --defined-only "$OUT"/*.o | awk '$2 ~ /^[TDBR]$/ {print $3}' | sort -u)
libc=$(nm -D --defined-only "$(gcc -print-file-name=libc.so.6)" | awk '{print $3}' | sed 's/@.*//' | sort -u)
missing=0; via_libc=0; via_shim=0
for s in $need $need_data; do
    # the BIOS file calls the host libc also has: the game's units call the shim's psyq_api_* (psyq_names.h)
    if grep -qx "psyq_api_$s" <<<"$have"; then via_shim=$((via_shim+1))
    elif grep -qx "$s" <<<"$have"; then via_shim=$((via_shim+1))
    elif grep -qx "$s" <<<"$libc"; then via_libc=$((via_libc+1)); echo "  host libc: $s"
    else missing=$((missing+1)); echo "  MISSING: $s"; fi
done
echo "coverage: $via_shim symbols from the shim, $via_libc from the host libc, $missing missing (of $(wc -w <<<"$need $need_data"))"

# Link check: the probe's game objects + the shim; what stays undefined must be the port runtime's (port_*,
# port_unimplemented), the game's own asm-only data (not Psy-Q) and libc.
undef=$(nm -u $game_objs "$OUT"/*.o | awk '{print $2}' | sort -u)
defined=$( (nm -g --defined-only $game_objs "$OUT"/*.o | awk '$2 ~ /^[TDBRCW]$/ {print $3}'; echo "$libc") | sort -u)
left=$(comm -23 <(echo "$undef") <(echo "$defined") | grep -v -e '^port_' -e '^$' || true)
dups=$(nm -g --defined-only $game_objs "$OUT"/*.o | awk '$2 ~ /^[TDBR]$/ {print $3}' | sort | uniq -d || true)
echo "link: undefined after the shim and libc (should be the game's asm-only data only):"
echo "$left" | sed 's/^/  /'
if [ -n "$dups" ]; then echo "link: DUPLICATE globals:"; echo "$dups" | sed 's/^/  /'; exit 1; fi
[ "$missing" -eq 0 ]
