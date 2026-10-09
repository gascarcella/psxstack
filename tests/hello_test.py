#!/usr/bin/env python3
"""The stack's smoke test: examples/hello built through psxstack_add_game() and run headless, its picture hashed.

    python3 tests/hello_test.py [--build DIR] [--record]
    python3 tests/hello_test.py --exe build/hello-win/hello.exe --wine [--gpu]

Configures and builds examples/hello (cmake, ninja, the host gcc and binutils; no SDL, no disc), checks `--version`
names it, runs it for 40 vsyncs with a screenshot at vsync 30 (the software GPU: the same bytes on every machine) and
compares the PPM's SHA-1 with EXPECTED; --record prints the hash to put there. Then the script check: a script's
write_mem steps (a value, a masked value, `data`; through the example's adapter probe, examples/hello/adapter.c, and
into the arena) read back by wait_mem in the same frame (timeout 0), and a write to an address nothing maps ends the
run with status 1. Exit 0 pass, 1 fail, 2 missing tool.
--exe tests a given build instead (CI: the Windows cross-build with -DPSXSTACK_SDL=ON); --wine runs it through `wine`
(the prefix build/wine-prefix unless WINEPREFIX names one); --gpu also takes the hardware renderer's picture of the
same vsync (an SDL build), which must be the software one byte for byte when a device opens (on Windows, Direct3D 12
first) and is skipped, with the reason, when none does (CI's Wine has none).
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = "fbf8eb30c160296161eed21c9790ec8a6c2622b0"   # build/hello/shot.ppm at vsync 30


PROBE = 0x80010000  # examples/hello/adapter.c HELLO_PROBE_ADDR
WRITES = [
    {"type": "wait_frames", "frames": 3},
    {"type": "write_mem", "addr": hex(PROBE), "value": "0x12345678"},
    {"type": "wait_mem", "addr": hex(PROBE), "value": "0x12345678", "timeout": 0},
    {"type": "write_mem", "addr": hex(PROBE), "value": -1, "mask": "0xFF00"},
    {"type": "wait_mem", "addr": hex(PROBE), "value": "0x1234FF78", "timeout": 0},
    {"type": "write_mem", "addr": hex(PROBE + 6), "size": 2, "value": -2},
    {"type": "wait_mem", "addr": hex(PROBE + 6), "size": 2, "value": -2, "signed": True, "timeout": 0},
    {"type": "write_mem", "addr": hex(PROBE + 9), "data": "a1B2c3"},
    {"type": "wait_mem", "addr": hex(PROBE + 8), "value": "0xC3B2A100", "timeout": 0},
    {"type": "write_mem", "addr": "0x80100000", "data": "00" * 256},
    {"type": "checkpoint", "name": "written"},
]


def script_check(runner, exe, build, env):
    """write_mem against wait_mem in the same frame; an unmapped address is status 1."""
    ok = True
    for name, steps, want in (("write_mem", WRITES, 0),
                              ("write_mem_unmapped", [{"type": "write_mem", "addr": "0x80020000", "value": 1}], 1)):
        path = build / f"{name}.json"
        path.write_text(json.dumps({"name": name, "max_frames": 100, "steps": steps}))
        r = subprocess.run([*runner, str(exe), "--script", str(path), "--crash-dir", str(build / f"{name}_crash")],
                           capture_output=True, text=True, env=env, timeout=600)
        last = r.stderr.strip().splitlines()[-1] if r.stderr.strip() else ""
        good = r.returncode == want and (want == 0 or "write_mem: 0x80020000" in r.stderr)
        print(("ok   " if good else "FAIL ") + f"script {name}: status {r.returncode} (expected {want}); {last}")
        ok &= good
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=str(ROOT / "build/hello"))
    ap.add_argument("--record", action="store_true")
    ap.add_argument("--exe", help="test this build of hello instead of building one")
    ap.add_argument("--wine", action="store_true", help="run --exe through wine (a Windows build)")
    ap.add_argument("--gpu", action="store_true", help="also the hardware renderer's picture (an SDL build)")
    args = ap.parse_args()
    env = dict(os.environ)
    runner = []
    if args.exe:
        exe = Path(args.exe).resolve()
        build = exe.parent
        if not exe.is_file():
            print(f"hello: no binary at {args.exe}")
            return 2
        if args.wine:
            if not shutil.which("wine"):
                print("hello: --wine: no wine on PATH")
                return 2
            prefix = Path(env.get("WINEPREFIX", ROOT / "build/wine-prefix"))
            prefix.mkdir(parents=True, exist_ok=True)
            runner = ["wine"]
            env.update(WINEPREFIX=str(prefix), WINEDEBUG="-all", SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                       VKD3D_SHADER_CACHE_PATH="0")
    else:
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
    v = subprocess.run([*runner, str(exe), "--version"], capture_output=True, text=True, env=env).stdout.strip()
    print(("ok   " if v.startswith("hello ") else "FAIL ") + f"--version: {v}")
    ok &= v.startswith("hello ")
    shot, gpu = build / "shot.ppm", build / "gpu.ppm"
    shot.unlink(missing_ok=True)
    gpu.unlink(missing_ok=True)
    cmd = [*runner, str(exe), "--max-frames", "40", "--screenshot", f"30:{shot}"]
    if args.gpu:
        cmd += ["--gpu-screenshot", f"30:{gpu}"]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=600)
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
    if args.gpu:
        device = next((l.split("renderer: ", 1)[1] for l in r.stderr.splitlines() if "renderer: gpu" in l), "?")
        if gpu.exists() and shot.exists():
            same = gpu.read_bytes() == shot.read_bytes()
            print(("ok   " if same else "FAIL ") + f"the hardware renderer's picture ({device}) is the software one")
            ok &= same
        else:
            print(f"skip the hardware renderer's picture: {device}")
    ok &= script_check(runner, exe, build, env)
    print("hello: " + ("pass" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
