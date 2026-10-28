#!/usr/bin/env python3
"""The stack's smoke test: examples/hello built through psxstack_add_game() and run headless, its picture hashed.

    python3 tests/hello_test.py [--build DIR] [--record]
    python3 tests/hello_test.py --exe build/hello-win/hello.exe --wine [--gpu]
    python3 tests/hello_test.py --menu [--gpu] [--build DIR]

Configures and builds examples/hello (cmake, ninja, the host gcc and binutils; no SDL, no disc), checks `--version`
names it, runs it for 40 vsyncs with a screenshot at vsync 30 (the software GPU: the same bytes on every machine) and
compares the PPM's SHA-1 with EXPECTED; --record prints the hash to put there. Then the script check: a script's
write_mem steps (a value, a masked value, `data`; through the example's adapter probe, examples/hello/adapter.c, and
into the arena) read back by wait_mem in the same frame (timeout 0), and a write to an address nothing maps ends the
run with status 1. Then the second pad (PAD2): a script with "pad2" and a press on port 1 traces port 1 in its log and
record, and hello (which tints its gradient from pad 2) draws another picture than with the press on port 0 or none; a
press on port 1 without "pad2", and a "pad2" that is not a boolean, are refused when the script loads. Exit 0 pass, 1
fail, 2 missing tool.
--exe tests a given build instead (CI: the Windows cross-build with -DPSXSTACK_SDL=ON); --wine runs it through `wine`
(the prefix build/wine-prefix unless WINEPREFIX names one); --gpu also takes the hardware renderer's picture of the
same vsync (an SDL build), which must be the software one byte for byte when a device opens (on Windows, Direct3D 12
first) and is skipped, with the reason, when none does (CI's Wine has none).
--menu builds hello with the window and the in-game menu instead and runs it on SDL's offscreen driver (menu_check);
--menu --gpu also takes the menu's picture on the hardware renderer (Mesa's lavapipe when it is installed: NVIDIA's
driver cannot present to the offscreen driver), which must be the software one but for a few edge pixels
(GPU_MENU_LEVELS, GPU_MENU_EDGES). It ends with hello as a game that needs its disc (nogame_check): without one it
refuses to start, and its input self-test runs without the game (no save states: the quick slot's round trip skipped).
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import zlib
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


# The second pad (#101): the same press on port 1 (with "pad2") and on port 0, and no press; hello shows a press 4
# vsyncs after it (its read at the next vsync, its double buffer), so vsync 8 has the press of vsync 4.
PAD2_PRESS = [{"type": "wait_frames", "frames": 3}, {"type": "press", "buttons": ["CROSS"], "frames": 4, "release": 2}]
PAD2 = {
    "port1": {"pad2": True, "steps": [dict(PAD2_PRESS[0]), dict(PAD2_PRESS[1], port=1)]},
    "port0": {"steps": PAD2_PRESS},
    "none": {"steps": [{"type": "wait_frames", "frames": 9}]},
}
PAD2_LOG = ["I 1 port 1 buttons 0x0000", "I 4 port 1 buttons 0x4000", "I 8 port 1 buttons 0x0000"]
PAD2_BAD = {
    "port1_without_pad2": ({"steps": [{"type": "press", "port": 1, "buttons": ["CROSS"]}]}, "`port` is 0, or 1"),
    "pad2_not_bool": ({"pad2": 1, "steps": []}, "`pad2`: true or false"),
}


def pad2_check(runner, exe, build, env):
    """PAD2: port 1 from a script, traced and seen by the game; the refusals."""
    ok, shots = True, {}
    for name, script in PAD2.items():
        path, log, rec, shot = (build / f"pad2_{name}{ext}" for ext in (".json", ".log", ".record.json", ".ppm"))
        path.write_text(json.dumps(dict(script, name=f"pad2_{name}", max_frames=100)))
        shot.unlink(missing_ok=True)
        r = subprocess.run([*runner, str(exe), "--script", str(path), "--log", str(log), "--record", str(rec),
                            "--screenshot", f"8:{shot}"], capture_output=True, text=True, env=env, timeout=600)
        good = r.returncode == 0 and shot.exists()
        shots[name] = shot.read_bytes() if shot.exists() else None
        if name == "port1" and good:
            lines = log.read_text().splitlines()
            entries = [e for e in json.loads(rec.read_text())["inputs"] if e.get("port") == 1]
            good = all(l in lines for l in PAD2_LOG) and [e["frame"] for e in entries] == [1, 4, 8]
        print(("ok   " if good else "FAIL ") + f"script pad2_{name}: status {r.returncode}"
              + ("; port 1 in the log and the record" if name == "port1" else ""))
        ok &= good
    differ = shots["port1"] not in (shots["port0"], shots["none"])
    print(("ok   " if differ else "FAIL ") + "the press on port 1 reaches the game (vsync 8: another picture than "
          "port 0's and none's)")
    ok &= differ
    for name, (script, msg) in PAD2_BAD.items():
        path = build / f"pad2_{name}.json"
        path.write_text(json.dumps(dict(script, name=name)))
        r = subprocess.run([*runner, str(exe), "--script", str(path), "--crash-dir", str(build / f"pad2_{name}_crash")],
                           capture_output=True, text=True, env=env, timeout=600)
        good = r.returncode == 1 and msg in r.stderr
        print(("ok   " if good else "FAIL ") + f"script {name}: refused (status {r.returncode})")
        ok &= good
    return ok


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


def menu_check(build, env, gpu):
    """--menu: hello with the window and the in-game menu (-DPSXSTACK_SDL=ON -DPSXSTACK_MENU=ON: tools/sdl3, dxc and
    imgui), its input self-test on SDL's offscreen driver, which ends with the menu's round trip (runtime/input.c), and
    the picture of the menu up on its Graphics page (HELLO_PORT_MENU_SHOT and _SCREEN, launcher/ui/menu.cpp) at
    build/menu.png; with gpu the same on the hardware renderer, build/menu-gpu.png (menu_gpu); then the other settings
    pages' at 640x480 (MENU_PAGES), build/menu-<page>.png; then the Mods page's at three window sizes and one menu size
    (MENU_SIZES), build/menu-mods-*.png; then the input self-test again with each
    input.player2 that gives a second pad (PLAYER2: its players' part), the first also with the picture of the Controls
    screen's Gamepad tab at 640x480, build/menu-controls-gamepad.png."""
    for tool in ("cmake", "ninja", "gcc", "g++", "objcopy", "objdump", "nm"):
        if not shutil.which(tool, path=env["PATH"]):
            print(f"hello: no {tool} on PATH (scripts/setup.sh cmake; gcc, g++ and binutils from the system)")
            return 2
    if not (build / "CMakeCache.txt").exists():
        subprocess.run(["cmake", "-S", str(ROOT / "examples/hello"), "-B", str(build), "-G", "Ninja",
                        "-DPSXSTACK_SDL=ON", "-DPSXSTACK_MENU=ON"], check=True, env=env, stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", str(build)], check=True, env=env, stdout=subprocess.DEVNULL)
    exe, shot = build / "hello", build / "menu.png"
    cfg = build / "cfg"
    cfg.mkdir(exist_ok=True)
    r = menu_run(exe, cfg, shot, env)
    lines = r.stderr.strip().splitlines()
    menu = next((l for l in lines if "input test: the menu:" in l), "(no menu line)")
    live = next((l for l in lines if "re-applied live in the menu:" in l), "(no live line)")
    ff_on = any(l.endswith("mods: fast_forward on") for l in lines)
    opened = next((i for i, l in enumerate(lines) if l.endswith("audio: unmuted: the device opens")), None)
    sound = opened is not None and any("audio: SDL audio driver" in l for l in lines[opened:])
    ok = r.returncode == 0 and "the menu: passed" in menu and "live in the menu: passed" in live and ff_on and sound
    print(("ok   " if ok else "FAIL ") + f"the input self-test with the menu: status {r.returncode}; {menu}; {live}; "
          f"fast-forward {'switched on live' if ff_on else 'NOT switched on'}; "
          f"{'unmuted live: the device opened' if sound else 'unmuted live: NO device opened'}")
    if not ok:
        print("\n".join(lines[-15:]))
    fit = shot_fit(lines)
    if fit != "nothing wider than its window":
        print(f"FAIL the Graphics page at 640x480: {fit}")
        ok = False
    if shot.exists() and shot.stat().st_size > 1000 and shot.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n":
        print(f"ok   the menu's picture: {shot} ({shot.stat().st_size} bytes)")
    else:
        print("FAIL no PNG of the menu")
        ok = False
    if gpu:
        ok &= menu_gpu(exe, cfg, shot, env)
    ok &= menu_pages(exe, cfg, env)
    ok &= menu_sizes(exe, cfg, env)
    ok &= players_check(exe, cfg, env)
    ok &= nogame_check(build.parent / (build.name + "-nodisc"), env)
    print("hello: " + ("pass" if ok else "FAIL"))
    return 0 if ok else 1


def menu_run(exe, cfg, shot, env, *extra):
    """The input self-test with the menu on its Graphics page (the largest of the settings pages: every setting of
    the hardware renderer, greyed out under the software one), its picture to `shot`. The game starts muted, from a
    settings file of its own, so the menu's pages have something to edit. The live apply with a change
    (runtime/input.c HELLO_PORT_MENU_LIVE_FILE): a second file, put in place while the menu is up, rebinds the menu
    hotkey, switches fast-forward on and the mute off; the round trip then closes the menu with the new key, the mod's
    "on" line must follow, and the audio device must open (runtime/audio.c: a game started muted has none)."""
    shot.unlink(missing_ok=True)
    (cfg / "settings.json").write_text('{ "schema": 1, "audio": { "mute": true } }\n')
    (cfg / "live.json").write_text('{ "schema": 1, "input": { "hotkeys": { "menu": "F10" } }, '
                                   '"mods": { "fast_forward": { "enabled": true } }, "audio": { "mute": false } }\n')
    run_env = dict(env, SDL_VIDEO_DRIVER="offscreen", SDL_AUDIO_DRIVER="dummy", HELLO_PORT_MENU_SHOT=str(shot),
                   HELLO_PORT_MENU_SCREEN="graphics", HELLO_PORT_MENU_LIVE_FILE=str(cfg / "live.json"))
    return subprocess.run([str(exe), "--config", str(cfg / "settings.json"), "--mods-dir", str(cfg / "mods"),
                           "--input-test", "--fps", "0", *extra], capture_output=True, text=True, env=run_env,
                          timeout=600)


# The hardware renderer's picture of the menu against the software one: ImGui's triangles rasterised by another API
# round their anti-aliased edges differently (lavapipe against OpenGL: 5 % of the pixels, by at most 2 levels a
# channel; NVIDIA's Direct3D 12: 67 pixels of a radio button's rim, by up to 33). At most GPU_MENU_EDGES of the
# pixels may differ by more than GPU_MENU_LEVELS; a missing, misplaced or other page differs by far more (another
# page: 18 % of the pixels).
GPU_MENU_LEVELS = 8
GPU_MENU_EDGES = 0.001
LAVAPIPE = Path("/usr/share/vulkan/icd.d/lvp_icd.x86_64.json")


def menu_gpu(exe, cfg, soft, env):
    """--menu --gpu: the run of menu_run on the hardware renderer (render_gpu.c draws the present with the menu into a
    texture it reads back), build/menu-gpu.png, on Mesa's lavapipe when it is installed (VK_DRIVER_FILES; NVIDIA's
    driver cannot present to the offscreen driver and the window would fall back to SDL_Renderer)."""
    shot = exe.parent / "menu-gpu.png"
    run_env = dict(env)
    if LAVAPIPE.exists() and "VK_DRIVER_FILES" not in env:
        run_env["VK_DRIVER_FILES"] = str(LAVAPIPE)
    r = menu_run(exe, cfg, shot, run_env, "--renderer", "gpu")
    lines = r.stderr.strip().splitlines()
    window = next((l.split("window: ", 1)[1] for l in lines if "window: SDL" in l), "(no window line)")
    if "renderer gpu" not in window:
        print(f"FAIL the menu on the hardware renderer: the window has none ({window}); install Mesa's lavapipe "
              f"(mesa-vulkan-drivers), which presents to SDL's offscreen driver")
        print("\n".join(l for l in lines if "renderer" in l))
        return False
    menu = next((l for l in lines if "input test: the menu:" in l), "(no menu line)")
    fit = shot_fit(lines)
    good = r.returncode == 0 and "the menu: passed" in menu and fit == "nothing wider than its window"
    if good and shot.exists() and soft.exists():
        (w, h, a), (sw, sh, b) = png_rgb(shot), png_rgb(soft)
        if (w, h) != (sw, sh):
            diff = f"{w}x{h}, the software one {sw}x{sh}"
            good = False
        else:
            delta = [max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
                     for i in range(0, len(a), 3)]
            pixels, far = sum(1 for d in delta if d), sum(1 for d in delta if d > GPU_MENU_LEVELS)
            diff = (f"{pixels} of {w * h} pixels differ from the software one, by at most {max(delta)}; "
                    f"{far} by more than {GPU_MENU_LEVELS} (at most {int(w * h * GPU_MENU_EDGES)})")
            good = far <= w * h * GPU_MENU_EDGES
    else:
        diff = "no picture" if good else f"status {r.returncode}; {menu}; {fit}"
        good = False
    print(("ok   " if good else "FAIL ") + f"the menu on the hardware renderer ({window}): {diff}; {shot}")
    if not good and r.returncode != 0:
        print("\n".join(lines[-15:]))
    return good


def png_rgb(path):
    """An 8-bit RGB or RGBA PNG, not interlaced (SDL_SavePNG's), as (width, height, its RGB bytes): no Pillow needed."""
    data, pos, idat = path.read_bytes(), 8, b""
    while pos < len(data):
        n, kind = int.from_bytes(data[pos:pos + 4], "big"), data[pos + 4:pos + 8]
        if kind == b"IHDR":
            w, h = int.from_bytes(data[pos + 8:pos + 12], "big"), int.from_bytes(data[pos + 12:pos + 16], "big")
            depth, ctype, interlace = data[pos + 16], data[pos + 17], data[pos + 20]
            if depth != 8 or ctype not in (2, 6) or interlace:
                raise ValueError(f"{path}: depth {depth}, colour type {ctype}, interlace {interlace}")
            bpp = 3 if ctype == 2 else 4
        elif kind == b"IDAT":
            idat += data[pos + 8:pos + 8 + n]
        pos += 12 + n
    raw, stride = zlib.decompress(idat), w * bpp
    prev, out = bytearray(stride), bytearray()
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line if bpp == 3 else bytes(v for i, v in enumerate(line) if i % 4 != 3)
        prev = line
    return w, h, bytes(out)


# The settings pages menu_run does not take (it takes Graphics), at the smallest window: nothing may be wider than it.
MENU_PAGES = ["video", "audio", "system"]


def menu_pages(exe, cfg, env):
    ok = True
    pages = cfg / "pages"
    pages.mkdir(exist_ok=True)
    (pages / "settings.json").write_text('{ "schema": 1 }\n')
    for page in MENU_PAGES:
        shot = exe.parent / f"menu-{page}.png"
        shot.unlink(missing_ok=True)
        run_env = dict(env, SDL_VIDEO_DRIVER="offscreen", SDL_AUDIO_DRIVER="dummy", SDL_RENDER_DRIVER="software",
                       HELLO_PORT_MENU_SHOT=str(shot), HELLO_PORT_MENU_SCREEN=page, HELLO_PORT_MENU_WINDOW="640x480")
        r = subprocess.run([str(exe), "--config", str(pages / "settings.json"), "--input-test", "--fps", "0"],
                           capture_output=True, text=True, env=run_env, timeout=600)
        lines = r.stderr.strip().splitlines()
        fit = shot_fit(lines)
        good = (r.returncode == 0 and fit == "nothing wider than its window" and shot.exists() and
                shot.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n")
        print(("ok   " if good else "FAIL ") + f"the {page.capitalize()} page at 640x480: {fit}; {shot}")
        if not good and r.returncode != 0:
            print("\n".join(lines[-15:]))
        ok &= good
    return ok


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
# checks player 2's never fires a hotkey (the default, "off", is the first run's: both on port 0). The first run also
# takes the menu's Gamepad tab (its Player 2 row) at the smallest window, which must fit it.
PLAYER2 = ["second", "first"]


def players_check(exe, cfg, env):
    ok = True
    players = cfg / "players"
    players.mkdir(exist_ok=True)
    for mode in PLAYER2:
        (players / "settings.json").write_text(json.dumps({"schema": 1, "input": {"player2": mode}}))
        run_env = dict(env, SDL_VIDEO_DRIVER="offscreen", SDL_AUDIO_DRIVER="dummy")
        shot = exe.parent / "menu-controls-gamepad.png"
        if mode == PLAYER2[0]:
            shot.unlink(missing_ok=True)
            run_env.update(SDL_RENDER_DRIVER="software", HELLO_PORT_MENU_SHOT=str(shot),
                           HELLO_PORT_MENU_SCREEN="controls:gamepad", HELLO_PORT_MENU_WINDOW="640x480")
        r = subprocess.run([str(exe), "--config", str(players / "settings.json"), "--input-test", "--fps", "0"],
                           capture_output=True, text=True, env=run_env, timeout=600)
        lines = r.stderr.strip().splitlines()
        line = next((l.split("input test: ", 1)[1] for l in lines if "input test: the players (" in l and
                     "checks passed" in l), "no players' line")
        good = r.returncode == 0 and line.startswith(f'the players (input.player2 "{mode}")')
        print(("ok   " if good else "FAIL ") + f"the input self-test, player2 {mode}: status {r.returncode}; {line}")
        if mode == PLAYER2[0]:
            fit = shot_fit(lines)
            pic = shot.exists() and shot.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"
            print(("ok   " if fit == "nothing wider than its window" and pic else "FAIL ") +
                  f"the Gamepad tab at 640x480: {fit}; {shot if pic else 'no PNG'}")
            good &= fit == "nothing wider than its window" and pic
        if not good:
            print("\n".join(lines[-15:]))
        ok &= good
    return ok


def nogame_check(build, env):
    """A game without the disc it needs (game.json disc_required): hello configured with -DHELLO_DISC_REQUIRED=ON
    (examples/hello/CMakeLists.txt), with the window and the menu, into build/hello-menu-nodisc. Without --disc it
    refuses to start (status 64); with --input-test it runs the self-test on the pump alone, no game started: no save
    states (nothing to hold), so the quick slot's round trip is skipped and the menu's still runs; status 0."""
    if not (build / "CMakeCache.txt").exists():
        subprocess.run(["cmake", "-S", str(ROOT / "examples/hello"), "-B", str(build), "-G", "Ninja",
                        "-DPSXSTACK_SDL=ON", "-DPSXSTACK_MENU=ON", "-DHELLO_DISC_REQUIRED=ON"], check=True, env=env,
                       stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", str(build)], check=True, env=env, stdout=subprocess.DEVNULL)
    exe = build / "hello"
    run_env = dict(env, SDL_VIDEO_DRIVER="offscreen", SDL_AUDIO_DRIVER="dummy")
    r = subprocess.run([str(exe), "--max-frames", "10"], capture_output=True, text=True, env=run_env, timeout=600)
    good = r.returncode == 64 and "no disc" in r.stderr
    print(("ok   " if good else "FAIL ") + f"disc_required, no disc: status {r.returncode} (64: refused)")
    if not good:
        print(r.stderr.strip())
    ok = good
    cfg = build / "cfg"
    cfg.mkdir(exist_ok=True)
    (cfg / "settings.json").write_text('{ "schema": 1 }\n')
    r = subprocess.run([str(exe), "--config", str(cfg / "settings.json"), "--input-test", "--fps", "0"],
                       capture_output=True, text=True, env=run_env, timeout=600)
    lines = r.stderr.strip().splitlines()
    quick = next((l.split("input test: ", 1)[1] for l in lines if "input test: the quick slot" in l), "no quick slot line")
    menu = next((l.split("input test: ", 1)[1] for l in lines if "input test: the menu:" in l), "no menu line")
    good = (r.returncode == 0 and any("the input self-test runs without the game" in l for l in lines) and
            quick == "the quick slot: skipped (no game in this run)" and menu.startswith("the menu: passed") and
            not (cfg / "quick.state.input-test").exists())
    print(("ok   " if good else "FAIL ") + f"disc_required, --input-test without the disc: status {r.returncode}; "
          f"{quick}; {menu}")
    if not good:
        print("\n".join(lines[-15:]))
    return ok and good


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
                          env, args.gpu)
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
    ok &= pad2_check(runner, exe, build, env)
    print("hello: " + ("pass" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
