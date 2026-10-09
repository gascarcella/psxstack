#!/usr/bin/env python3
"""tools/game_gen.py on examples/dw2003.game.json: the header compiles as C and as C++, a few values are right, the
descriptions that must fail do, and a one-slot game and a game without a stack heap generate. Run: python3 tests/game_gen_test.py (needs cc and c++; ~1 s)."""
import copy
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN = ROOT / "tools/game_gen.py"
EXAMPLE = ROOT / "examples/dw2003.game.json"


def run(*args):
    return subprocess.run([sys.executable, str(GEN), *args], capture_output=True, text=True)


def compile_header(header, lang, cc, tmp):
    src = tmp / f"probe.{'c' if lang == 'c' else 'cpp'}"
    src.write_text(f'''#include "{header.name}"
#include <stdio.h>
#include <string.h>
static const PsxstackGameDisc discs[PSXSTACK_GAME_DISC_COUNT] = PSXSTACK_GAME_DISCS;
static const PsxstackGameBiosStandin bios[] = PSXSTACK_GAME_BIOS_STANDINS;
static const int rates[PSXSTACK_GAME_RATE_COUNT] = PSXSTACK_GAME_RATES;
int main(void) {{
    printf("%s|%s|%s|%s|%d|%d|%d|%u|%u|%u|%u|%s|%llu|%s|%s|%u|%s|%s\\n", PSXSTACK_GAME_ID, PSXSTACK_GAME_ID_UPPER,
           PSXSTACK_GAME_TITLE, PSXSTACK_GAME_ENV_PREFIX, PSXSTACK_GAME_RATE, rates[PSXSTACK_GAME_RATE_COUNT - 1],
           PORT_SLOT_COUNT, PORT_SLOT1_BASE, PORT_SLOT2_BASE, PORT_HEAP_START_ADDR, PORT_HEAP_SIZE, discs[0].sha1,
           discs[0].size, discs[0].cue, PORT_SLOT2_NAME, bios[0].address, bios[0].text, PSXSTACK_GAME_WEBSITE);
    return 0;
}}
''')
    exe = tmp / f"probe_{lang}"
    flags = ["-Wall", "-Wextra", "-Werror", "-pedantic"] + (["-std=c99"] if lang == "c" else ["-std=c++17"])
    r = subprocess.run([cc, "-x", lang, *flags, "-I", str(header.parent), str(src), "-o", str(exe)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None, r.stderr
    return subprocess.run([str(exe)], capture_output=True, text=True).stdout.strip(), ""


def main():
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)
            print(f"FAILED: {what}")

    tmp = Path(tempfile.mkdtemp(prefix="game_gen_test."))
    try:
        header = tmp / "psxstack_game_gen.h"
        r = run(str(EXAMPLE), "--out", str(header), "--cmake", str(tmp / "game.cmake"))
        check(r.returncode == 0, f"the example generates: {r.stderr}")
        text = header.read_text()
        for line in ["#define PSXSTACK_GAME_ID \"dw2003\"", "#define PSXSTACK_GAME_ENV_PREFIX \"DW3\"",
                     "#define PSXSTACK_GAME_RATE 50", "#define PSXSTACK_GAME_RATE_COUNT 2",
                     "#define PSXSTACK_GAME_RATES { 50, 60 }", "#define PORT_SLOT_COUNT 2",
                     "#define PORT_SLOT1_BASE 0x80082cb0u", "#define PORT_SLOT1_SIZE 0x23130u",
                     "#define PORT_SLOT2_BASE 0x800a5de0u", "#define PORT_SLOT2_SIZE 0x5a20u",
                     "#define PORT_HEAP_START_ADDR 0x800ab800u", "#define PORT_HEAP_END_ADDR 0x801ff000u",
                     "#define PORT_HEAP_SIZE 0x400000u", "#define PSXSTACK_GAME_DISC_COUNT 1",
                     "#define PSXSTACK_GAME_DISC_REQUIRED 0",
                     "#define PSXSTACK_GAME_BIOS_STANDIN_COUNT 1", "\"457cb233349ba841e03b33d8060f8fbcadd45cb3\"",
                     "692146560ull"]:
            check(line in text, f"the header has: {line}")
        cm = (tmp / "game.cmake").read_text()
        check('set(PSXSTACK_GAME_ID "dw2003")' in cm and 'set(PSXSTACK_GAME_TITLE "Digimon World 2003")' in cm,
              "the CMake variables")
        expect = ("dw2003|DW2003|Digimon World 2003|DW3|50|60|2|2148019376|2148163040|2148186112|4194304|"
                  "457cb233349ba841e03b33d8060f8fbcadd45cb3|692146560|dw2003.cue|file|532676908|PC-PORT-M1|"
                  "https://github.com/gascarcella/dw2003recomp")
        for lang, cc in (("c", "cc"), ("c++", "c++")):
            if shutil.which(cc) is None:
                print(f"skipped: no {cc}")
                continue
            out, err = compile_header(header, lang, cc, tmp)
            check(out is not None, f"the header compiles as {lang}: {err}")
            if out is not None:
                check(out == expect, f"the values as {lang}: {out}")

        # Stable output: a second run rewrites nothing.
        mtime = header.stat().st_mtime_ns
        run(str(EXAMPLE), "--out", str(header))
        check(header.stat().st_mtime_ns == mtime, "an unchanged header is not rewritten")

        # The failure cases.
        base = json.loads(EXAMPLE.read_text())

        def failing(mutate, what, needle):
            g = copy.deepcopy(base)
            mutate(g)
            p = tmp / "bad.json"
            p.write_text(json.dumps(g))
            r = run(str(p), "--check")
            check(r.returncode != 0 and needle in r.stderr, f"{what} fails with {needle!r}: {r.stderr.strip()}")

        def set_slot_base(g):
            g["memory"]["slots"][1]["base"] = "0x800A5DE4"
        failing(set_slot_base, "a gap between the slots", "contiguous")

        def swap_slots(g):
            g["memory"]["slots"].reverse()
        failing(swap_slots, "slots out of order", "contiguous")

        def heap_gap(g):
            g["memory"]["heap"]["start"] = "0x800AB804"
        failing(heap_gap, "a heap that does not follow the slots", "heap must follow")

        def small_host_heap(g):
            g["memory"]["heap"]["host_size"] = "0x1000"
        failing(small_host_heap, "a host heap smaller than the PS1's", "smaller than the PS1 heap")

        def bad_rate(g):
            g["video"]["rates"] = [60]
        failing(bad_rate, "rates without the nominal one", "must include video.rate")

        def bad_sha(g):
            g["discs"][0]["sha1"] = "abc"
        failing(bad_sha, "a short sha1", "sha1")

        def bad_id(g):
            g["id"] = "DW2003"
        failing(bad_id, "an upper-case id", "id")

        def missing(g):
            del g["discs"]
        failing(missing, "no discs", "discs")

        def unknown(g):
            g["colour"] = "red"
        failing(unknown, "an unknown key", "colour")

        def required_no_disc(g):
            g["disc_required"] = True
            g["discs"] = []
        failing(required_no_disc, "a game that needs a disc and lists none", "disc_required")

        def required_not_bool(g):
            g["disc_required"] = "yes"
        failing(required_not_bool, "disc_required not a boolean", "disc_required")

        # A game that cannot run without its disc: the macro is 1.
        req = copy.deepcopy(base)
        req["disc_required"] = True
        (tmp / "req.json").write_text(json.dumps(req))
        r = run(str(tmp / "req.json"), "--out", str(tmp / "req.h"))
        check(r.returncode == 0 and "#define PSXSTACK_GAME_DISC_REQUIRED 1" in (tmp / "req.h").read_text(),
              f"disc_required true generates PSXSTACK_GAME_DISC_REQUIRED 1: {r.stderr}")

        def bad_addr(g):
            g["memory"]["ram"]["base"] = "80000000"
        failing(bad_addr, "an address without 0x", "")

        # A one-slot game (Digimon World 2's shape) generates.
        one = copy.deepcopy(base)
        one["memory"]["slots"] = [{"name": "stage", "base": "0x80063360", "size": "0x11CA0"}]
        one["memory"]["heap"] = {"start": "0x80075000", "end": "0x801FF000"}
        one["video"] = {"rate": 60}
        del one["memory"]["bios_standin"]
        (tmp / "one.json").write_text(json.dumps(one))
        r = run(str(tmp / "one.json"), "--out", str(tmp / "one.h"))
        t = (tmp / "one.h").read_text() if r.returncode == 0 else ""
        check(r.returncode == 0 and "#define PORT_SLOT_COUNT 1" in t and "PORT_SLOT2_BASE" not in t and
              "#define PSXSTACK_GAME_RATE_COUNT 1" in t and "#define PSXSTACK_GAME_BIOS_STANDIN_COUNT 0" in t,
              f"a one-slot, one-rate game generates: {r.stderr}")
        if shutil.which("cc"):
            r2 = subprocess.run(["cc", "-x", "c", "-std=c99", "-Wall", "-Werror", "-fsyntax-only", "-include",
                                 str(tmp / "one.h"), "-"], input="int x;\n", capture_output=True, text=True)
            check(r2.returncode == 0, f"the one-slot header compiles: {r2.stderr}")

        # A game with no stack heap (the second game's shape: its heap is its own data) generates; the arena is the
        # slots alone; the header compiles as C and C++ through hooks.h; a HEAP_* hook then refuses to compile.
        noheap = copy.deepcopy(one)
        del noheap["memory"]["heap"]
        (tmp / "noheap.json").write_text(json.dumps(noheap))
        r = run(str(tmp / "noheap.json"), "--check")
        check(r.returncode == 0 and "no heap" in r.stdout, f"a game without a heap validates: {r.stderr}{r.stdout}")
        hdr = tmp / "noheap" / "psxstack_game_gen.h"
        r = run(str(tmp / "noheap.json"), "--out", str(hdr))
        t = hdr.read_text() if r.returncode == 0 else ""
        check(r.returncode == 0 and "#define PORT_HEAP_PRESENT 0" in t and "PORT_HEAP_START_ADDR" not in t and
              "#define PORT_HEAP_OFS 0x11ca0u" in t and "#define PORT_HEAP_SIZE 0u" in t,
              f"the no-heap header: the arena is the slots alone: {r.stderr}")
        hooks = ROOT / "include/psxstack"
        for lang, cc in (("c", "cc"), ("c++", "c++")):
            if shutil.which(cc) is None:
                continue
            std = ["-std=c99"] if lang == "c" else ["-std=c++17"]
            r2 = subprocess.run([cc, "-x", lang, *std, "-Wall", "-Werror", "-fsyntax-only", "-I", str(hdr.parent),
                                 "-I", str(hooks), "-"],
                                input='#include "hooks.h"\nunsigned char *p = port_slot1;\n',
                                capture_output=True, text=True)
            check(r2.returncode == 0, f"hooks.h compiles as {lang} without a heap: {r2.stderr}")
            r2 = subprocess.run([cc, "-x", lang, *std, "-fsyntax-only", "-I", str(hdr.parent), "-I", str(hooks), "-"],
                                input='#include "hooks.h"\nvoid *q = HEAP_START(void *);\n',
                                capture_output=True, text=True)
            check(r2.returncode != 0 and "has_no_memory_heap" in r2.stderr,
                  f"HEAP_START without a heap fails to compile as {lang} with the reason: {r2.stderr[:300]}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("game_gen_test: " + ("passed" if not failures else f"{len(failures)} failure(s)"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
