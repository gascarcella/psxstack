#!/usr/bin/env python3
"""The emulator replay runner (GAME_CONTRACT.md "6. Tests"; docs/RUNTIME.md "The replay runners"): boots a game's disc
in PCSX-Redux headless, feeds a pad script, hashes the game's checkpoint image at named checkpoints, and compares
the record with the game's expected file. The game configures it (configure() below) and gives it the probes
(a Lua chunk, see run.lua); the game's own driver is a few lines:

    sys.path.insert(0, str(ROOT / "psxstack/tools/replay"))
    import emulator
    CFG = emulator.configure(root=ROOT, game_json=ROOT / "port/game/game.json", redux_dir=ROOT / "tools/redux",
                             iso=ROOT / "iso/game.cue", scripts_dir=..., expected_dir=..., probes=ROOT / "tests/replay/probes.lua",
                             volatile_ranges=((0x00, 0x01), ...), retail_bios=..., tree_paths=("src", "include", "config"))
    if __name__ == "__main__": sys.exit(emulator.main())

  <driver> run <script.json> [--record] [--repeat N] [--bios openbios|retail|FILE] [--speed S] [--interpreter]
                             [--iso CUE] [--prelude LUA] [--expected-dir DIR] [--out DIR] [-v]
  <driver> check [--interpreter] [--iso CUE] [-j N] [script.json ...]   # every script with an expected file
  <driver> boot [--bios ...] [--iso CUE] [--frames N]                    # the boot check (boot_check.lua): the probes' booted()

A script (a JSON file; the grammar is runtime/script.c's, the same the port replays) is a list of steps; run.lua
executes it in the emulator and writes result.json plus one checkpoint image per checkpoint. This driver hashes the
images (SHA-1, and the stable SHA-1 with the game's volatile ranges zeroed), builds the record and compares it with
the expected file, or writes that file with --record. --repeat N runs the script N times and requires identical
records (determinism).

The record (the expected file's and the port's --record share the shape: docs/RUNTIME.md "The record"): script,
script_sha1, emulator {name, version, build_id, changeset}, bios {name, sha1}, tree_commit, frames, checkpoints
[{name, frame, stage, map, random_index, gamestate_sha1, gamestate_sha1_stable}], overlay_sequence, map_sequence,
inputs. The environment the emulator gets: PSXSTACK_REPLAY_SCRIPT, _OUT, _VERBOSE, _SPEED, _PROBES, _SLOT1_BASE,
and the same under the game's prefix (<PREFIX>_REPLAY_*), for a game's own Lua preludes.

Exit codes: 0 pass, 1 mismatch or emulator failure, 2 usage / missing tool.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUN_LUA = HERE / "run.lua"
BOOT_LUA = HERE / "boot_check.lua"
# Fields of a checkpoint / sequence entry that the record keeps (in this order).
CHECKPOINT_FIELDS = ("name", "frame", "stage", "map", "random_index", "gamestate_sha1", "gamestate_sha1_stable")
# A checkpoint step with `"image": false` is recorded without the two hashes and with `"image": false` (absent: true).
CHECKPOINT_PLAIN_FIELDS = CHECKPOINT_FIELDS[:5]
IMAGE_FIELDS = CHECKPOINT_FIELDS[5:]


def has_image(cp):
    """Whether a script step or a record's checkpoint carries the checkpoint image (the default)."""
    return cp.get("image", True) is not False


def image_diffs(script, record, what="record"):
    """The checkpoint steps' `image` flags of a script against a record's checkpoints (in order): a record that hashes
    a no-image checkpoint, or lacks the hashes of an image one, does not belong to the script."""
    steps = [st for st in script.get("steps", []) if st.get("type") == "checkpoint"]
    diffs = []
    for i, (st, cp) in enumerate(zip(steps, record["checkpoints"])):
        if has_image(st) != has_image(cp):
            diffs.append(f"checkpoint {i} ({cp.get('name')}): the script says image {has_image(st)}, the {what} says "
                         f"{has_image(cp)}")
        elif not has_image(cp) and any(f in cp for f in IMAGE_FIELDS):
            diffs.append(f"checkpoint {i} ({cp.get('name')}): the {what} has a hash for a checkpoint without an image")
        elif has_image(cp) and not all(f in cp for f in IMAGE_FIELDS):
            diffs.append(f"checkpoint {i} ({cp.get('name')}): the {what} lacks the image hashes")
    return diffs


class Config:
    """What a game tells the runner (its driver calls configure())."""

    def __init__(self, root, game_json, redux_dir, iso, scripts_dir, expected_dir, probes, volatile_ranges=(),
                 retail_bios=None, tree_paths=("src", "include", "config"), interpreter_args=("-interpreter",),
                 default_max_frames=20000):
        self.root = Path(root)
        self.game_json = Path(game_json)
        g = json.loads(self.game_json.read_text())
        self.game_id = g["id"]
        self.env_prefix = g.get("env_prefix", g["id"].upper())
        base = g["memory"]["slots"][0]["base"]
        self.slot1_base = int(base, 0) if isinstance(base, str) else int(base)
        self.redux_dir = Path(redux_dir)
        self.redux = self.redux_dir / "pcsx-redux"                      # the wrapper redux.sh writes
        self.redux_version = self.redux_dir / "app/usr/share/pcsx-redux/resources/version.json"
        self.openbios = self.redux_dir / "app/usr/share/pcsx-redux/resources/openbios.bin"
        self.retail_bios = Path(retail_bios) if retail_bios else None    # cross-check only: goldens come from OpenBIOS
        self.iso = Path(iso)
        self.scripts_dir = Path(scripts_dir)
        self.expected_dir = Path(expected_dir)
        self.probes = Path(probes)
        # Byte ranges [lo, hi) of the checkpoint image that change with the timing (zeroed for the stable hash).
        self.volatile_ranges = tuple((int(lo), int(hi)) for lo, hi in volatile_ranges)
        self.tree_paths = tuple(tree_paths)      # the paths whose last commit names the matching tree (tree_commit)
        self.interpreter_args = tuple(interpreter_args)   # the emulator flags of --interpreter
        self.default_max_frames = default_max_frames


CFG = None


def configure(**kwargs):
    global CFG
    CFG = Config(**kwargs)
    return CFG


def sha1_file(path):
    return hashlib.sha1(Path(path).read_bytes()).hexdigest()


def lua_literal(v, indent=""):
    """A JSON value as a Lua literal (the step table run.lua loads)."""
    if isinstance(v, bool):
        return "true" if v else "false"
    if v is None:
        return "nil"
    if isinstance(v, (int, float)):
        return repr(v)
    if isinstance(v, str):
        return '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'
    inner = indent + "  "
    if isinstance(v, list):
        return "{\n" + ",\n".join(inner + lua_literal(x, inner) for x in v) + "\n" + indent + "}"
    if isinstance(v, dict):
        parts = []
        for k, x in v.items():
            if isinstance(x, str) and k in ("addr", "map", "value", "word0") and x.startswith("0x"):
                x = int(x, 16)
            parts.append(f"{inner}[{lua_literal(k)}] = {lua_literal(x, inner)}")
        return "{\n" + ",\n".join(parts) + "\n" + indent + "}"
    raise TypeError(type(v))


def parse_ints(obj):
    """Hex strings in the script ("0x2D7") become ints for the Lua side."""
    if isinstance(obj, dict):
        return {k: (int(v, 16) if isinstance(v, str) and v.startswith("0x") and k in ("addr", "map", "value", "word0")
                    else parse_ints(v)) for k, v in obj.items()}
    if isinstance(obj, list):
        return [parse_ints(x) for x in obj]
    return obj


def tree_commit():
    """The last commit that touched the matching tree (not HEAD: test and doc commits don't change the game)."""
    try:
        paths = list(CFG.tree_paths)
        rev = subprocess.run(["git", "-C", str(CFG.root), "log", "-1", "--format=%H", "--", *paths],
                             check=True, capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(CFG.root), "status", "--porcelain", "--", *paths],
                               check=True, capture_output=True, text=True).stdout.strip() != ""
        return rev + ("-dirty" if dirty else "")
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unknown"


def emulator_info():
    info = json.loads(CFG.redux_version.read_text())
    return {"name": "pcsx-redux", "version": info["version"], "build_id": info["buildId"],
            "changeset": info["changeset"]}


def bios_path(name):
    if name == "openbios":
        return CFG.openbios
    if name == "retail":
        if not CFG.retail_bios:
            sys.exit("replay: this game configures no retail BIOS")
        return CFG.retail_bios
    return Path(name)


def bios_name(bios):
    bios = Path(bios)
    if bios == CFG.openbios:
        return "openbios"
    if CFG.retail_bios and bios == CFG.retail_bios:
        return "retail"
    return bios.name


def replay_env(script_lua, out_dir, verbose=False, speed=0):
    """The emulator's environment: the stack's names and the game's aliases."""
    values = {"SCRIPT": str(script_lua), "OUT": str(out_dir), "SPEED": str(speed), "PROBES": str(CFG.probes),
              "SLOT1_BASE": str(CFG.slot1_base)}
    if verbose:
        values["VERBOSE"] = "1"
    env = dict(os.environ)
    for k, v in values.items():
        env[f"PSXSTACK_REPLAY_{k}"] = v
        env[f"{CFG.env_prefix}_REPLAY_{k}"] = v
    return env


def run_once(script_path, script, bios, out_dir, verbose=False, lua=None, emu_args=(), speed=0, iso=None):
    """Runs the script in the emulator once; returns the record (without the checkpoint images) or raises. `lua`
    replaces run.lua as the -dofile chunk (a game's wrapper that loads its own Lua first, then run.lua), `emu_args`
    are extra emulator flags (-debugger -interpreter for breakpoints); `speed` is PCSX-Redux's spu.Speed (0:
    unthrottled: it paces the host only, the emulated machine is the same); `iso` another disc image's .cue."""
    # Absolute: PCSX-Redux resolves a relative -memcard path elsewhere (not the cwd), and a card that is not the fresh
    # one changes the boot.
    out_dir = Path(out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    lua_script = out_dir / "script.lua"
    lua_script.write_text("return " + lua_literal(parse_ints(script)) + "\n")
    env = replay_env(lua_script, out_dir, verbose, speed)
    # Fresh (empty) memory cards per run: the user's ~/.config cards must not leak into a replay.
    mcd1, mcd2 = out_dir / "memcard1.mcd", out_dir / "memcard2.mcd"
    for m in (mcd1, mcd2):
        if m.exists():
            m.unlink()
    cmd = [str(CFG.redux), "-no-ui", "-stdout", "-testmode", "-run", *emu_args, "-iso", str(iso or CFG.iso),
           "-bios", str(bios), "-memcard1", str(mcd1), "-memcard2", str(mcd2), "-dofile", str(lua or RUN_LUA)]
    max_frames = script.get("max_frames", CFG.default_max_frames)
    # real time (--speed 1) is ~50 frames/s; unthrottled ~500 (dynarec) or ~250 (interpreter): 10x margins
    timeout = max_frames / (10 if speed and speed > 0 else 25) + 120
    log_path = out_dir / "emulator.log"
    t0 = time.time()
    with open(log_path, "w") as log:
        proc = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
    elapsed = time.time() - t0
    result_path = out_dir / "result.json"
    log_text = log_path.read_text(errors="replace")
    if verbose:
        sys.stdout.write("".join(l + "\n" for l in log_text.splitlines() if l.startswith("replay:")))
    if not result_path.exists():
        tail = "\n".join(log_text.splitlines()[-15:])
        raise RuntimeError(f"emulator exited {proc.returncode} without result.json (log {log_path}):\n{tail}")
    result = json.loads(result_path.read_text())
    if result.get("status") != "ok" or proc.returncode != 0:
        raise RuntimeError(f"replay failed (exit {proc.returncode}): {result.get('message')} (log {log_path})")
    checkpoints = []
    for cp in result["checkpoints"]:
        entry = {k: cp[k] for k in CHECKPOINT_FIELDS if k in cp}
        if not has_image(cp):
            checkpoints.append({**{k: entry[k] for k in CHECKPOINT_PLAIN_FIELDS}, "image": False})
            continue
        data = (out_dir / cp["gamestate_file"]).read_bytes()
        entry["gamestate_sha1"] = hashlib.sha1(data).hexdigest()
        stable = bytearray(data)
        for lo, hi in CFG.volatile_ranges:
            stable[lo:hi] = bytes(hi - lo)
        entry["gamestate_sha1_stable"] = hashlib.sha1(stable).hexdigest()
        checkpoints.append({k: entry[k] for k in CHECKPOINT_FIELDS})
    record = {
        "script": script["name"],
        "script_sha1": sha1_file(script_path),
        "emulator": emulator_info(),
        "bios": {"name": bios_name(bios), "sha1": sha1_file(bios)},
        "tree_commit": tree_commit(),
        "frames": result["frames"],
        "checkpoints": checkpoints,
        "overlay_sequence": result["overlay_sequence"],
        "map_sequence": result["map_sequence"],
        "inputs": result["inputs"],
    }
    diffs = image_diffs(script, record)
    if diffs:
        raise RuntimeError("the script and the record disagree: " + "; ".join(diffs))
    print(f"  run: {result['frames']} frames, {len(checkpoints)} checkpoints, {elapsed:.0f} s wall")
    return record


def compare(expected, actual, ignore=("tree_commit",)):
    """Lists the differences between two records (empty when identical)."""
    diffs = []
    for key in sorted(set(expected) | set(actual)):
        if key in ignore:
            continue
        if expected.get(key) != actual.get(key):
            if key == "checkpoints":
                for i, (e, a) in enumerate(zip(expected[key], actual[key])):
                    for f in (*CHECKPOINT_FIELDS, "image"):
                        ev, av = (has_image(e), has_image(a)) if f == "image" else (e.get(f), a.get(f))
                        if ev != av:
                            diffs.append(f"checkpoint {i} ({e.get('name')}) {f}: expected {ev!r}, got {av!r}")
                if len(expected[key]) != len(actual[key]):
                    diffs.append(f"checkpoints: expected {len(expected[key])}, got {len(actual[key])}")
            else:
                diffs.append(f"{key}: expected {json.dumps(expected.get(key))[:200]}, got {json.dumps(actual.get(key))[:200]}")
    return diffs


def cross_core_view(record):
    """The part of a record that must not depend on the CPU core (dynarec or -interpreter), nor on the port's timing:
    checkpoint names, stages, maps and stable hashes, and the overlay and map sequences without frames. Frames, RNG
    draw counts, the full hash (timers) and the input trace follow the emulated timing. A checkpoint without an image
    (`"image": false`) has no hash: it is its name, stage and map, and `"image": false`."""
    return {
        "checkpoints": [{k: cp[k] for k in ("name", "stage", "map", "gamestate_sha1_stable")} if has_image(cp) else
                        {**{k: cp[k] for k in ("name", "stage", "map")}, "image": False}
                        for cp in record["checkpoints"]],
        "overlay_sequence": [(o["stage"], o["file"]) for o in record["overlay_sequence"]],
        "map_sequence": [m["map"] for m in record["map_sequence"]],
    }


def load_script(path):
    script = json.loads(Path(path).read_text())
    script.setdefault("name", Path(path).stem)
    return script


def check_tools():
    missing = [str(p) for p in (CFG.redux, CFG.iso, CFG.probes) if not p.exists()]
    if missing:
        print("replay: missing " + ", ".join(missing) + " (the emulator: tools/replay/redux.sh through the game's "
              "setup; the disc; the probes)", file=sys.stderr)
        sys.exit(2)


def shown(path):
    """A path for messages: relative to the game's repository when inside it."""
    path = Path(path).resolve()
    return path.relative_to(CFG.root) if path.is_relative_to(CFG.root) else path


def cmd_run(args):
    check_tools()
    script_path = Path(args.script)
    script = load_script(script_path)
    bios = bios_path(args.bios)
    base_out = Path(args.out) if args.out else Path(tempfile.mkdtemp(prefix=f"{CFG.game_id}_replay_"))
    records = []
    emu_args, lua = CFG.interpreter_args if args.interpreter else (), None
    if args.prelude:
        # a Lua chunk run before run.lua (a game's patch or trace); its exec breakpoints need the debugger and the
        # interpreter core
        emu_args = ("-debugger", "-interpreter")
        base_out.mkdir(parents=True, exist_ok=True)
        lua = base_out.resolve() / "prelude_wrapper.lua"
        lua.write_text(f"dofile({json.dumps(str(Path(args.prelude).resolve()))})\ndofile({json.dumps(str(RUN_LUA))})\n")
    for i in range(args.repeat):
        print(f"replay {script['name']} (bios {args.bios}), run {i + 1}/{args.repeat}")
        try:
            records.append(run_once(script_path, script, bios, base_out / f"run{i + 1}", verbose=args.verbose,
                                    lua=lua, emu_args=emu_args, speed=args.speed, iso=args.iso))
        except (RuntimeError, subprocess.TimeoutExpired) as e:
            print(f"  FAIL: {e}")
            return 1
    status = 0
    for i, rec in enumerate(records[1:], start=2):
        diffs = compare(records[0], rec, ignore=())
        if diffs:
            status = 1
            print(f"  run 1 vs run {i} DIFFER (non-deterministic):\n    " + "\n    ".join(diffs))
    if args.repeat > 1 and status == 0:
        print(f"  determinism: {args.repeat} runs identical")
    if args.prelude:
        records[0]["prelude"] = Path(args.prelude).name
    expected_path = (Path(args.expected_dir) if args.expected_dir else CFG.expected_dir) / f"{script['name']}.json"
    if args.record and args.interpreter and not args.prelude:
        print("  not recording: expected files come from the default core (dynarec)")
    elif args.record:
        if status:
            print("  not recording: runs differ")
        else:
            expected_path.parent.mkdir(parents=True, exist_ok=True)
            expected_path.write_text(json.dumps(records[0], indent=1) + "\n")
            print(f"  recorded {shown(expected_path)}")
    elif expected_path.exists():
        expected = json.loads(expected_path.read_text())
        diffs = (compare(cross_core_view(expected), cross_core_view(records[0])) if args.interpreter or args.prelude
                 else compare(expected, records[0]))
        if diffs:
            status = 1
            print(f"  MISMATCH vs {shown(expected_path)}:\n    " + "\n    ".join(diffs))
        else:
            print(f"  matches {shown(expected_path)}" + (" (cross-core view)" if args.interpreter else ""))
    else:
        print(f"  no expected file ({shown(expected_path)}); use --record")
    print(f"  outputs: {base_out}")
    return status


def check_one(script_path, args):
    """One script's replay against its expected file: (status, the lines to print)."""
    script = load_script(script_path)
    expected_path = CFG.expected_dir / f"{script['name']}.json"
    if not expected_path.exists():
        return 0, [f"replay {script['name']}: no expected file, skipped"]
    expected = json.loads(expected_path.read_text())
    if expected.get("script_sha1") != sha1_file(script_path):
        return 1, [f"replay {script['name']}: FAIL (the script changed since it was recorded; re-record)"]
    diffs = image_diffs(script, expected, "expected file")
    if diffs:
        return 1, [f"replay {script['name']}: FAIL (script and expected file disagree: " + "; ".join(diffs) + ")"]
    bios = bios_path(expected["bios"]["name"])
    out = Path(tempfile.mkdtemp(prefix=f"{CFG.game_id}_replay_{script['name']}_"))
    lines = [f"replay {script['name']} (bios {expected['bios']['name']})"]
    try:
        record = run_once(script_path, script, bios, out, emu_args=CFG.interpreter_args if args.interpreter else (),
                          iso=args.iso)
    except (RuntimeError, subprocess.TimeoutExpired) as e:
        return 1, lines + [f"  FAIL: {e}"]
    diffs = compare(cross_core_view(expected), cross_core_view(record)) if args.interpreter else compare(expected, record)
    if diffs:
        return 1, lines + [f"  FAIL: mismatch vs {shown(expected_path)}:\n    " + "\n    ".join(diffs)]
    shutil.rmtree(out, ignore_errors=True)
    return 0, lines + [f"  pass ({record['frames']} frames, {len(record['checkpoints'])} checkpoints)"]


def cmd_check(args):
    """Every script (or the ones named), -j at a time (default <PREFIX>_JOBS, else all at once: each is one emulator,
    single-threaded); each one's lines are printed together, in the scripts' order."""
    check_tools()
    scripts = [Path(s) for s in args.scripts] or sorted(CFG.scripts_dir.glob("*.json"))
    status = 0
    jobs = max(1, min(args.jobs or len(scripts), len(scripts)))
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        for rc, lines in pool.map(lambda s: check_one(s, args), scripts):
            print("\n".join(lines), flush=True)
            status |= rc
    return status


def cmd_boot(args):
    """The boot check: the disc boots headless until the probes' booted() holds (boot_check.lua)."""
    check_tools()
    bios = bios_path(args.bios)
    if not bios.exists():
        print(f"replay: missing BIOS {bios}", file=sys.stderr)
        return 2
    iso = Path(args.iso) if args.iso else CFG.iso
    out = Path(tempfile.mkdtemp(prefix=f"{CFG.game_id}_boot_"))
    env = replay_env(out / "none.lua", out)
    env["PSXSTACK_BOOT_FRAMES"] = str(args.frames)
    cmd = [str(CFG.redux), "-no-ui", "-stdout", "-testmode", "-run", "-iso", str(iso), "-bios", str(bios),
           "-dofile", str(BOOT_LUA)]
    print(f"booting {iso.name} with {bios.name} (up to {args.frames} frames)")
    log_path = out / "emulator.log"
    with open(log_path, "w") as log:
        proc = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=args.frames / 10 + 120)
    text = log_path.read_text(errors="replace")
    for line in text.splitlines():
        if line.startswith(("Loaded BIOS", "Known BIOS", "OpenBIOS detected", "boot check:")):
            print(line)
    if proc.returncode != 0 or "boot check: OK" not in text:
        print(f"FAIL (emulator exit {proc.returncode}); last lines of the log ({log_path}):", file=sys.stderr)
        print("\n".join(text.splitlines()[-15:]), file=sys.stderr)
        return 1
    shutil.rmtree(out, ignore_errors=True)
    print("check passed: the game booted")
    return 0


def build_parser(prog=None):
    jobs_env = f"{CFG.env_prefix}_JOBS" if CFG else "PSXSTACK_JOBS"
    ap = argparse.ArgumentParser(prog=prog, description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="run one script (record or compare)")
    r.add_argument("script")
    r.add_argument("--record", action="store_true", help="write the expected file")
    r.add_argument("--repeat", type=int, default=1, help="run N times and require identical records")
    r.add_argument("--bios", default="openbios", help="openbios (default), retail, or a BIOS file")
    r.add_argument("--out", help="output directory (default: a temp dir)")
    r.add_argument("--speed", type=float, default=0,
                   help="emulation speed (PCSX-Redux spu.Speed): 0 = unthrottled (default), 1 = real time")
    r.add_argument("--interpreter", action="store_true",
                   help="run on the interpreter core; compares the cross-core view with the expected file (no --record)")
    r.add_argument("--iso", help="another disc image (.cue) instead of the game's")
    r.add_argument("--prelude", help="a Lua chunk run before run.lua; implies the debugger and the interpreter core; "
                                     "compares the cross-core view")
    r.add_argument("--expected-dir", help="where the expected file is read or --record writes it")
    r.add_argument("-v", "--verbose", action="store_true")
    r.set_defaults(func=cmd_run)
    c = sub.add_parser("check", help="run every recorded script and compare")
    c.add_argument("scripts", nargs="*")
    c.add_argument("--interpreter", action="store_true", help="run on the interpreter core; compare the cross-core view")
    c.add_argument("--iso", help="another disc image (.cue) instead of the game's")
    c.add_argument("-j", "--jobs", type=int, default=int(os.environ.get(jobs_env, "0")) or None,
                   help=f"scripts replayed at once (default: ${jobs_env}, else all)")
    c.set_defaults(func=cmd_check)
    b = sub.add_parser("boot", help="the boot check: the disc boots until the probes' booted() holds")
    b.add_argument("--bios", default="openbios", help="openbios (default), retail, or a BIOS file")
    b.add_argument("--iso", help="another disc image (.cue) instead of the game's")
    b.add_argument("--frames", type=int, default=3000, help="give up after N vsyncs (default 3000)")
    b.set_defaults(func=cmd_boot)
    return ap


def main(argv=None):
    if CFG is None:
        sys.exit("replay: not configured: a game's driver calls emulator.configure() first")
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
