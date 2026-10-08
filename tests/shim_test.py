#!/usr/bin/env python3
"""The Psy-Q shim's deterministic pieces on their own: tests/shim/shim_test.c compiled with psyq/*.c (no runtime, no
game, no disc) and run.

    python3 tests/shim_test.py [--sanitize] [--m32] [--cc CC]

What it checks (one line each): LIBAPI's events and root counter 3 (one RCntCNT3 event per vsync tick, after the
VSyncCallback handler; EvMdNOINTR events through TestEvent; disable, stop, close), the critical sections; LIBCARD and
the BIOS's file calls on a fresh memory card image (the new-card flag through _card_info/_card_clear, FCREAT, write,
lseek, read back, the asynchronous calls completing at the next tick with their SwCARD/HwCARD events,
firstfile/nextfile, erase, format, an unformatted card, LIBMCRD reading the same card); LIBCD's CdSearchFile over an
ISO 9660 image the test builds (a subdirectory of two sectors, missing names, no disc) and CdRead/CdReadSync/CdSync;
LIBSPU's voice and common attributes; LIBSND's SEQ calls and SsStart's own tick. --sanitize adds AddressSanitizer and
UBSan, --m32 builds for i386. Exit 0 pass, 1 fail, 2 no compiler.
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sanitize", action="store_true")
    ap.add_argument("--m32", action="store_true")
    ap.add_argument("--cc", default="gcc")
    args = ap.parse_args()
    if not shutil.which(args.cc):
        print(f"shim test: no {args.cc}")
        return 2
    out = ROOT / "build" / ("shim_test" + ("-asan" if args.sanitize else "") + ("-m32" if args.m32 else ""))
    gen = out / "include"
    gen.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, str(ROOT / "tools/game_gen.py"), str(ROOT / "examples/hello/game.json"),
                    "--out", str(gen / "psxstack_game_gen.h")], check=True)
    flags = ["-m32" if args.m32 else "-m64", "-std=gnu99", "-O1", "-g", "-fsigned-char", "-fwrapv",
             "-fno-strict-aliasing", "-DPC_PORT", "-Wall", "-Wextra", "-Werror", f"-I{gen}", f"-I{ROOT / 'psyq'}",
             f"-I{ROOT / 'include'}", f"-I{ROOT / 'include/psxstack'}", f"-I{ROOT / 'runtime'}"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-sanitize-recover=undefined"]
    exe = out / "shim_test"
    srcs = [str(ROOT / "tests/shim/shim_test.c")] + [str(p) for p in sorted((ROOT / "psyq").glob("*.c"))]
    r = subprocess.run([args.cc, *flags, *srcs, "-o", str(exe), "-lm"], capture_output=True, text=True)
    if r.returncode != 0:
        print("shim test: the build failed:\n" + r.stderr)
        return 1
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
    print(r.stdout, end="")
    if r.stderr.strip():
        print(r.stderr, end="")
    if "AddressSanitizer" in r.stderr or "runtime error:" in r.stderr:
        print("shim test: FAIL (a sanitizer report)")
        return 1
    return 0 if r.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
