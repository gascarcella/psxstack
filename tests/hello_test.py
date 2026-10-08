#!/usr/bin/env python3
"""The stack's smoke test: examples/hello built through psxstack_add_game() and run headless, its picture hashed.

    python3 tests/hello_test.py [--build DIR] [--record]

Configures and builds examples/hello (cmake, ninja, the host gcc and binutils; no SDL, no disc), checks `--version`
names it, runs it for 40 vsyncs with a screenshot at vsync 30 (the software GPU: the same bytes on every machine) and
compares the PPM's SHA-1 with EXPECTED; --record prints the hash to put there. Exit 0 pass, 1 fail, 2 missing tool.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = "fbf8eb30c160296161eed21c9790ec8a6c2622b0"   # build/hello/shot.ppm at vsync 30


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=str(ROOT / "build/hello"))
    ap.add_argument("--record", action="store_true")
    args = ap.parse_args()
    env = dict(os.environ)
    venv = ROOT / "tools/venv/bin"
    if venv.exists():
        env["PATH"] = f"{venv}:{env['PATH']}"
    for tool in ("cmake", "ninja", "gcc", "objcopy", "objdump", "nm"):
        if not shutil.which(tool, path=env["PATH"]):
            print(f"hello: no {tool} on PATH (scripts/setup.sh cmake; gcc and binutils from the system)")
            return 2
    build = Path(args.build)
    if not (build / "CMakeCache.txt").exists():
        subprocess.run(["cmake", "-S", str(ROOT / "examples/hello"), "-B", str(build), "-G", "Ninja"], check=True,
                       env=env, stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", str(build)], check=True, env=env, stdout=subprocess.DEVNULL)
    exe = build / "hello"
    ok = True
    v = subprocess.run([str(exe), "--version"], capture_output=True, text=True).stdout.strip()
    print(("ok   " if v.startswith("hello ") else "FAIL ") + f"--version: {v}")
    ok &= v.startswith("hello ")
    shot = build / "shot.ppm"
    shot.unlink(missing_ok=True)
    r = subprocess.run([str(exe), "--max-frames", "40", "--screenshot", f"30:{shot}"], capture_output=True, text=True)
    last = r.stderr.strip().splitlines()[-1] if r.stderr.strip() else ""
    print(("ok   " if r.returncode == 0 else "FAIL ") + f"40 vsyncs headless: status {r.returncode}; {last}")
    ok &= r.returncode == 0
    if shot.exists():
        h = hashlib.sha1(shot.read_bytes()).hexdigest()
        if args.record:
            print(f"record: EXPECTED = \"{h}\"")
        else:
            print(("ok   " if h == EXPECTED else "FAIL ") + f"the picture at vsync 30: sha1 {h} (expected {EXPECTED})")
            ok &= h == EXPECTED
    else:
        print("FAIL no screenshot written")
        ok = False
    print("hello: " + ("pass" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
