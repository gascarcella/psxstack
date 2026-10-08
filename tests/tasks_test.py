#!/usr/bin/env python3
"""The fibers' test: examples/tasks (three tasks on psxstack's fibers, one of them preempted from the vblank handler)
built through psxstack_add_game() and run headless.

    python3 tests/tasks_test.py [--build DIR] [--sanitize] [--m32] [--record]
    python3 tests/tasks_test.py --exe build/tasks-win/tasks.exe --wine

Checks: `--version` names it; 60 vsyncs run to the frame cap, and the picture at vsync 58 (the software GPU) has the
SHA-1 EXPECTED (--record prints the hash to put there); with --trace the log shows every operation (create, switch,
exit, destroy, a slot reused); two runs write the same frame log (every frame's primitive hash); a run that saves
states at vsyncs 20 and 50 is unchanged, and a run resumed from either ends with the same picture and the straight
log's lines after that frame; the state at 50 was captured on a task's fiber, so the running fiber, its
stack and the suspended fibers' are all restored.
--sanitize builds with -DPSXSTACK_SANITIZE=ON (no report allowed; the state runs need ASan's fake stacks off), --m32
with -DPSXSTACK_M32=ON (gcc-multilib). --exe tests a given build instead; --wine runs it through `wine` (the prefix
build/wine-prefix unless WINEPREFIX names one). Exit 0 pass, 1 fail, 2 missing tool.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = "e3fd9d524275d64c4cea4b089d08d0cc372c1237"   # build/tasks/a.ppm at vsync 58
SAVE_AT = 20
SAVE_AT_BUSY = 50
FRAMES = 60
SHOT_AT = 58


def log_lines_after(path, frame):
    """The frame log's lines after `frame` (every line but the header carries its frame second)."""
    return [l for l in path.read_text().splitlines() if not l.startswith("#") and int(l.split()[1]) > frame]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=None)
    ap.add_argument("--record", action="store_true")
    ap.add_argument("--sanitize", action="store_true", help="build with -DPSXSTACK_SANITIZE=ON")
    ap.add_argument("--m32", action="store_true", help="build with -DPSXSTACK_M32=ON")
    ap.add_argument("--exe", help="test this build of tasks instead of building one")
    ap.add_argument("--wine", action="store_true", help="run --exe through wine (a Windows build)")
    args = ap.parse_args()
    env = dict(os.environ)
    runner = []
    if args.exe:
        exe = Path(args.exe).resolve()
        build = exe.parent
        if not exe.is_file():
            print(f"tasks: no binary at {args.exe}")
            return 2
        if args.wine:
            if not shutil.which("wine"):
                print("tasks: --wine: no wine on PATH")
                return 2
            prefix = Path(env.get("WINEPREFIX", ROOT / "build/wine-prefix"))
            prefix.mkdir(parents=True, exist_ok=True)
            runner = ["wine"]
            env.update(WINEPREFIX=str(prefix), WINEDEBUG="-all", SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy")
    else:
        venv = ROOT / "tools/venv/bin"
        if venv.exists():
            env["PATH"] = f"{venv}:{env['PATH']}"
        for tool in ("cmake", "ninja", "gcc", "objcopy", "objdump", "nm"):
            if not shutil.which(tool, path=env["PATH"]):
                print(f"tasks: no {tool} on PATH (scripts/setup.sh cmake; gcc and binutils from the system)")
                return 2
        variant = "-asan" if args.sanitize else "-m32" if args.m32 else ""
        build = Path(args.build) if args.build else ROOT / f"build/tasks{variant}"
        flags = (["-DPSXSTACK_SANITIZE=ON"] if args.sanitize else []) + (["-DPSXSTACK_M32=ON"] if args.m32 else [])
        if not (build / "CMakeCache.txt").exists():
            subprocess.run(["cmake", "-S", str(ROOT / "examples/tasks"), "-B", str(build), "-G", "Ninja", *flags],
                           check=True, env=env, stdout=subprocess.DEVNULL)
        subprocess.run(["cmake", "--build", str(build)], check=True, env=env, stdout=subprocess.DEVNULL)
        exe = build / "tasks"
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + what)
        ok &= bool(cond)

    def run(label, *extra, state_env=False):
        """A run of FRAMES vsyncs with its picture at SHOT_AT and its log; -> (status, stderr, shot path, log path)."""
        shot, log = build / f"{label}.ppm", build / f"{label}.log"
        shot.unlink(missing_ok=True)
        log.unlink(missing_ok=True)
        e = dict(env)
        if state_env:
            # a state cannot hold ASan's fake stacks (frames on the heap): off for the runs that use states
            e["ASAN_OPTIONS"] = (e.get("ASAN_OPTIONS", "") + ":detect_stack_use_after_return=0").lstrip(":")
        cmd = [*runner, str(exe), "--max-frames", str(FRAMES), "--screenshot", f"{SHOT_AT}:{shot}", "--log", str(log),
               *extra]
        r = subprocess.run(cmd, capture_output=True, text=True, env=e, timeout=600)
        if "AddressSanitizer" in r.stderr or "runtime error:" in r.stderr:
            print("FAIL a sanitizer report:\n" + "\n".join(r.stderr.splitlines()[-30:]))
            nonlocal ok
            ok = False
        return r.returncode, r.stderr, shot, log

    v = subprocess.run([*runner, str(exe), "--version"], capture_output=True, text=True, env=env).stdout.strip()
    check(v.startswith("tasks "), f"--version: {v}")

    status, err, shot_a, log_a = run("a")
    last = err.strip().splitlines()[-1] if err.strip() else ""
    check(status == 0, f"{FRAMES} vsyncs headless: status {status}; {last}")
    if shot_a.exists():
        h = hashlib.sha1(shot_a.read_bytes()).hexdigest()
        if args.record:
            print(f"record: EXPECTED = \"{h}\"")
        else:
            check(h == EXPECTED, f"the picture at vsync {SHOT_AT}: sha1 {h} (expected {EXPECTED})")
    else:
        check(False, "a screenshot written")

    status, err, _, _ = run("t", "--trace")
    ops = {"create 1": 0, "create 2": 0, "switch 0 -> 1": 0, "switch 1 -> 0": 0, "exit 2": 0, "destroy 1": 0}
    for line in err.splitlines():
        for op in ops:
            if line.startswith("port: fiber: " + op):
                ops[op] += 1
    switches = sum(1 for line in err.splitlines() if line.startswith("port: fiber: switch "))
    check(status == 0 and ops["create 1"] == 2 and ops["create 2"] == 1 and ops["exit 2"] == 1 and ops["destroy 1"] == 1
          and switches > FRAMES,
          f"the trace: {switches} switches, blink exited, quad destroyed, busy created in the freed slot ({ops})")

    status, _, shot_b, log_b = run("b")
    check(status == 0 and log_a.exists() and log_b.exists() and log_a.read_bytes() == log_b.read_bytes()
          and shot_a.read_bytes() == shot_b.read_bytes(),
          f"a second run: the same frame log ({len(log_a.read_text().splitlines())} lines) and picture")

    # two states: at vsync SAVE_AT (the main fiber's tick: the tasks still switch cooperatively) and at SAVE_AT_BUSY
    # (busy is running by then, and every other tick ends on its fiber, preempted: one of the two states is captured
    # on a fiber, its stack and the suspended ones' restored by the load)
    states = {f: build / f"s{f}.bin" for f in (SAVE_AT, SAVE_AT_BUSY)}
    for st in states.values():
        st.unlink(missing_ok=True)
    status, err, shot_c, log_c = run("c", *[a for f, st in states.items() for a in ("--save-state", f"{f}:{st}")],
                                     state_env=True)
    saved = [l.replace("port: ", "") for l in err.splitlines() if "state: frame" in l]
    on_fiber = [l for l in saved if "on fiber 0)" not in l]
    check(status == 0 and all(st.exists() for st in states.values()) and log_c.read_bytes() == log_a.read_bytes()
          and shot_c.read_bytes() == shot_a.read_bytes(),
          f"a run saving states at vsyncs {SAVE_AT} and {SAVE_AT_BUSY} is the straight run")
    check(len(on_fiber) == 1, "one of the two states was captured on a task's fiber: "
          + "; ".join(l.split(" saved to ")[0] + l.split("ms")[-1] for l in saved))
    for f, st in states.items():
        if not st.exists():
            continue
        status, err, shot_d, log_d = run(f"d{f}", "--load-state", str(st), state_env=True)
        resumed = next((l for l in err.splitlines() if "state: resumed" in l), "no resume line")
        la, ld = log_lines_after(log_a, f), log_lines_after(log_d, -1) if log_d.exists() else []
        check(status == 0 and shot_d.exists() and shot_d.read_bytes() == shot_a.read_bytes(),
              f"resumed from the state at {f}: the picture at vsync {SHOT_AT} is the straight run's "
              f"({resumed.replace('port: ', '')})")
        check(la == ld and len(ld) > 0,
              f"resumed from the state at {f}: the log's {len(ld)} lines after frame {f} are the straight run's {len(la)}")
    print("tasks: " + ("pass" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
