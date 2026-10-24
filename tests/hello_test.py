#!/usr/bin/env python3
"""The stack's smoke test: examples/hello built through psxstack_add_game() and run headless, its picture hashed.

    python3 tests/hello_test.py [--build DIR] [--record]
    python3 tests/hello_test.py --exe build/hello-win/hello.exe --wine [--gpu]
    python3 tests/hello_test.py --menu [--build DIR]

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


def menu_check(build, env):
    """--menu: hello with the window and the in-game menu (-DPSXSTACK_SDL=ON -DPSXSTACK_MENU=ON: tools/sdl3, dxc and
    imgui), its input self-test on SDL's offscreen driver, which ends with the menu's round trip (runtime/input.c), and
    the picture of the menu up on its Settings page (HELLO_PORT_MENU_SHOT and _SCREEN, launcher/ui/menu.cpp) at
    build/menu.png; then the Mods page's at three window sizes and one menu size (MENU_SIZES), build/menu-mods-*.png;
    then the input self-test again with each input.player2 that gives a second pad (PLAYER2: its players' part)."""
    for tool in ("cmake", "ninja", "gcc", "g++", "objcopy", "objdump", "nm"):
        if not shutil.which(tool, path=env["PATH"]):
            print(f"hello: no {tool} on PATH (scripts/setup.sh cmake; gcc, g++ and binutils from the system)")
            return 2
    if not (build / "CMakeCache.txt").exists():
        subprocess.run(["cmake", "-S", str(ROOT / "examples/hello"), "-B", str(build), "-G", "Ninja",
                        "-DPSXSTACK_SDL=ON", "-DPSXSTACK_MENU=ON"], check=True, env=env, stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", str(build)], check=True, env=env, stdout=subprocess.DEVNULL)
    exe, shot = build / "hello", build / "menu.png"
    shot.unlink(missing_ok=True)
    # A settings file for the run, so the menu's Settings, Controls and Mods pages have something to edit; the
    # picture is the Settings page.
    cfg = build / "cfg"
    cfg.mkdir(exist_ok=True)
    (cfg / "settings.json").write_text('{ "schema": 1 }\n')
    # The live apply with a change (runtime/input.c HELLO_PORT_MENU_LIVE_FILE): a second file, put in place while the
    # menu is up, rebinds the menu hotkey and switches fast-forward on; the round trip then closes the menu with the
    # new key, and the mod's "on" line must follow.
    (cfg / "live.json").write_text('{ "schema": 1, "input": { "hotkeys": { "menu": "F10" } }, '
                                   '"mods": { "fast_forward": { "enabled": true } } }\n')
    run_env = dict(env, SDL_VIDEO_DRIVER="offscreen", SDL_AUDIO_DRIVER="dummy", HELLO_PORT_MENU_SHOT=str(shot),
                   HELLO_PORT_MENU_SCREEN="settings", HELLO_PORT_MENU_LIVE_FILE=str(cfg / "live.json"))
    r = subprocess.run([str(exe), "--config", str(cfg / "settings.json"), "--mods-dir", str(cfg / "mods"),
                        "--input-test", "--fps", "0"], capture_output=True, text=True, env=run_env, timeout=600)
    lines = r.stderr.strip().splitlines()
    menu = next((l for l in lines if "input test: the menu:" in l), "(no menu line)")
    live = next((l for l in lines if "re-applied live in the menu:" in l), "(no live line)")
    ff_on = any(l.endswith("mods: fast_forward on") for l in lines)
    ok = r.returncode == 0 and "the menu: passed" in menu and "live in the menu: passed" in live and ff_on
    print(("ok   " if ok else "FAIL ") + f"the input self-test with the menu: status {r.returncode}; {menu}; {live}; "
          f"fast-forward {'switched on live' if ff_on else 'NOT switched on'}")
    if not ok:
        print("\n".join(lines[-15:]))
    fit = shot_fit(lines)
    if fit != "nothing wider than its window":
        print(f"FAIL the Settings page at 640x480: {fit}")
        ok = False
    if shot.exists() and shot.stat().st_size > 1000 and shot.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n":
        print(f"ok   the menu's picture: {shot} ({shot.stat().st_size} bytes)")
    else:
        print("FAIL no PNG of the menu")
        ok = False
    ok &= menu_sizes(exe, cfg, env)
    ok &= players_check(exe, cfg, env)
    print("hello: " + ("pass" if ok else "FAIL"))
    return 0 if ok else 1


# The Mods page's pictures (#92): a desktop's 1920x1080, the Steam Deck's 1280x800 and the smallest window, 640x480,
# at the fitted size, and the Deck's at video.menu_size "larger" (the narrow layout at a larger scale), with
# tests/fixtures/mods/multipliers selected (a page at least as large as the widest a game ships), its values set so
# every Default button shows. Nothing may be wider than its window (the menu logs that with the shot).
MENU_SIZES = [(1920, 1080, "auto"), (1280, 800, "auto"), (640, 480, "auto"), (1280, 800, "larger")]


def shot_fit(lines):
    """The menu's line on the windows wider than they are, logged with its shot (launcher/ui/menu.cpp)."""
    return next((l.split("menu: shot: ", 1)[1] for l in lines if "menu: shot: " in l and " -> " not in l),
                "no overflow line")


def menu_sizes(exe, cfg, env):
    ok = True
    mods = exe.parent / "mods"
    shutil.copytree(ROOT / "tests/fixtures/mods/multipliers", mods / "multipliers", dirs_exist_ok=True)
    sizes = cfg / "sizes"
    sizes.mkdir(exist_ok=True)
    for w, h, size in MENU_SIZES:
        (sizes / "settings.json").write_text(json.dumps({"schema": 1, "video": {"menu_size": size}, "mods": {
            "multipliers": {"enabled": True, "exp": 5, "second_exp": 5, "money": 5, "rounds": 12,
                            "toggle": ["F6", "pad:north"]}}}))
        shot = exe.parent / f"menu-mods-{w}x{h}{'' if size == 'auto' else '-' + size}.png"
        shot.unlink(missing_ok=True)
        # SDL's software renderer: the offscreen driver's OpenGL keeps the size the window was opened at.
        run_env = dict(env, SDL_VIDEO_DRIVER="offscreen", SDL_AUDIO_DRIVER="dummy", SDL_RENDER_DRIVER="software",
                       HELLO_PORT_MENU_SHOT=str(shot), HELLO_PORT_MENU_SCREEN="mods:multipliers",
                       HELLO_PORT_MENU_WINDOW=f"{w}x{h}")
        r = subprocess.run([str(exe), "--config", str(sizes / "settings.json"), "--input-test", "--fps", "0"],
                           capture_output=True, text=True, env=run_env, timeout=600)
        lines = r.stderr.strip().splitlines()
        scale = next((l.split("menu: ", 1)[1] for l in lines if "menu: scale" in l), "no scale logged")
        fit = shot_fit(lines)
        good = (r.returncode == 0 and fit == "nothing wider than its window" and shot.exists() and
                shot.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n")
        print(("ok   " if good else "FAIL ") + f"the Mods page at {w}x{h}, {size}: {scale}; {fit}; {shot}")
        if not good and r.returncode != 0:
            print("\n".join(lines[-15:]))
        ok &= good
    return ok


# input.player2 with a second pad (#101): the self-test's players' part puts one virtual gamepad on each port and
# checks player 2's never fires a hotkey (the default, "off", is the first run's: both on port 0).
PLAYER2 = ["second", "first"]


def players_check(exe, cfg, env):
    ok = True
    players = cfg / "players"
    players.mkdir(exist_ok=True)
    for mode in PLAYER2:
        (players / "settings.json").write_text(json.dumps({"schema": 1, "input": {"player2": mode}}))
        run_env = dict(env, SDL_VIDEO_DRIVER="offscreen", SDL_AUDIO_DRIVER="dummy")
        r = subprocess.run([str(exe), "--config", str(players / "settings.json"), "--input-test", "--fps", "0"],
                           capture_output=True, text=True, env=run_env, timeout=600)
        lines = r.stderr.strip().splitlines()
        line = next((l.split("input test: ", 1)[1] for l in lines if "input test: the players (" in l and
                     "checks passed" in l), "no players' line")
        good = r.returncode == 0 and line.startswith(f'the players (input.player2 "{mode}")')
        print(("ok   " if good else "FAIL ") + f"the input self-test, player2 {mode}: status {r.returncode}; {line}")
        if not good:
            print("\n".join(lines[-15:]))
        ok &= good
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=str(ROOT / "build/hello"))
    ap.add_argument("--record", action="store_true")
    ap.add_argument("--exe", help="test this build of hello instead of building one")
    ap.add_argument("--wine", action="store_true", help="run --exe through wine (a Windows build)")
    ap.add_argument("--gpu", action="store_true", help="also the hardware renderer's picture (an SDL build)")
    ap.add_argument("--menu", action="store_true",
                    help="the window and the in-game menu instead: the input self-test offscreen and the menu's picture")
    args = ap.parse_args()
    env = dict(os.environ)
    runner = []
    if args.menu:
        venv = ROOT / "tools/venv/bin"
        if venv.exists():
            env["PATH"] = f"{venv}:{env['PATH']}"
        return menu_check(Path(args.build if args.build != str(ROOT / "build/hello") else ROOT / "build/hello-menu"),
                          env)
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
