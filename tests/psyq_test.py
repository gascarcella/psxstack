#!/usr/bin/env python3
"""The shim's LIBGTE and LIBGPU functions on their own: tests/psyq/psyq_test.c built with every psyq/*.c and the flags
of psyq/check.sh (-Wall -Wextra -Werror, hello's game.json standing in for a game's description), then run.

    python3 tests/psyq_test.py [--sanitize] [--build DIR]

The C checks documented cases and properties (csqrt(16.0) = 4.0, ratan2 at the axes and against atan2, RotMatrix
against the exact product, RotTransPers3 = three RotTransPers, the matrix stack's round trip and its 20 entries,
AddPrim's links drawn by DrawOTag, the primitives' length and command bytes, StoreImage of a LoadImage, ReadTIM, ...);
what only the PS1 can settle is the consumers' goldens' (psyq/README.md "Behaviour assumed"). Built twice, at -O0 and
-O2 (a compiler's built-in csqrt/catan must not replace LIBGTE's); --sanitize adds a build under AddressSanitizer and
UBSan. Exit 0 pass, 1 fail, 2 no compiler.
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=str(ROOT / "build/psyq_test"))
    ap.add_argument("--sanitize", action="store_true", help="also a build under -fsanitize=address,undefined")
    args = ap.parse_args()
    cc = shutil.which("gcc")
    if cc is None:
        print("psyq_test: no gcc on PATH")
        return 2
    build = Path(args.build)
    inc = build / "include"
    inc.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, str(ROOT / "tools/game_gen.py"), str(ROOT / "examples/hello/game.json"),
                    "--out", str(inc / "psxstack_game_gen.h")], check=True)
    srcs = sorted(str(p) for p in (ROOT / "psyq").glob("*.c")) + [str(ROOT / "tests/psyq/psyq_test.c")]
    flags = ["-m64", "-std=gnu99", "-fsigned-char", "-fwrapv", "-fno-strict-aliasing", "-DPC_PORT", "-Wall", "-Wextra",
             "-Werror", f"-I{inc}", f"-I{ROOT / 'psyq'}", f"-I{ROOT / 'include'}", f"-I{ROOT / 'include/psxstack'}",
             f"-I{ROOT / 'runtime'}"]
    variants = [("O0", ["-O0"]), ("O2", ["-O2"])]
    if args.sanitize:
        variants.append(("asan", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]))
    failed = 0
    for name, extra in variants:
        exe = build / f"psyq_test_{name}"
        r = subprocess.run([cc, *flags, *extra, *srcs, "-lm", "-o", str(exe)], capture_output=True, text=True)
        if r.returncode != 0:
            print(f"psyq_test ({name}): the build failed\n{r.stderr}")
            failed += 1
            continue
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
        out = (r.stdout + r.stderr).strip()
        print(f"[{name}] {out}")
        if r.returncode != 0:
            failed += 1
    print("psyq_test: " + ("FAIL" if failed else "ok"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
