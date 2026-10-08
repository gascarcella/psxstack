#!/usr/bin/env python3
"""The port's replay test (GAME_CONTRACT.md "6. Tests"; docs/RUNTIME.md "The replay runners"): the port replays the
game's pad scripts and must reach what the emulator reached. The game configures it (configure() below) and adds
its own checks through the hooks; its own driver is a few lines:

    sys.path.insert(0, str(ROOT / "psxstack/tools/replay"))
    import port_test
    CFG = port_test.configure(root=ROOT, game_json=ROOT / "port/game/game.json", disc=ROOT / "iso/game.cue",
                              scripts_dir=..., expected_dir=..., venv_bin=ROOT / "tools/venv/bin",
                              ubsan_suppressions=..., m32_log_exact=("new_game",), m32_note="...",
                              before_scripts=..., after_script=...)
    if __name__ == "__main__": sys.exit(port_test.main())

  <driver> [SCRIPT ...] [--m32] [--sanitize] [--cd-speed instant|realistic] [--out DIR] [-j N]
  <driver> [SCRIPT ...] --exe build/port-win/<game>.exe --wine     # a Windows build, under Wine

Builds the port if needed (cmake -S <port dir> -B build/port -G Ninja; cmake --build), and for each script (default:
every <scripts dir>/<name>.json with an <expected dir>/<name>.json; or the names given) runs
  build/port/<game> --disc <disc> --script <script> --log ... --record ... --spu-trace ...
twice and requires the two logs, records and SPU traces to be byte-identical (determinism), then compares the
record's cross-core view (emulator.py cross_core_view, compare) with the expected file: the same checkpoints (name,
stage, map, stable image hash) and the same overlay and map sequences, without frames.
  --m32       also build build/port-m32 (-DPSXSTACK_M32=ON, needs gcc-multilib) and require its cross-core view to
              equal the 64-bit build's, and for the scripts in `m32_log_exact` its log and record byte for byte (the
              layout check: pointers are 4 bytes there, as on the PS1)
  --sanitize  also build build/port-san (-DPSXSTACK_SANITIZE=ON), run it once, and fail on any ASan/UBSan report
              (its log and record must equal the plain build's too)
  --cd-speed  the port's CD timing (default: the port's, realistic)
  --exe PATH  run this binary instead of building build/port; build/port is only configured (a game's own checks may
              compile against its generated headers); --m32 and --sanitize do not apply to it
  --wine      run the binary (--exe, a Windows build) through `wine`, headless (SDL's dummy video and audio drivers,
              the prefix in build/wine-prefix/); its log and record must equal the Linux build's
  --out DIR   where the logs, records and checkpoint dumps go (default build/port-test/)
The hooks: before_scripts(variants, out) -> [failure, ...] runs once after the plain build (variants: "m64", "m32",
"san" as built; a game's LIBSND-on-the-emulator's-timeline check); after_script(name, out_dir, binary, run1) ->
[failure, ...] runs per script (run1 = (log bytes, record bytes, record, stderr text, SPU trace bytes)).
Needs only the disc, the host gcc, CMake and Ninja (on PATH, or in the venv's bin) and the venv.
Exit codes: 0 pass, 1 fail, 2 something missing.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from emulator import compare, cross_core_view, has_image, image_diffs, sha1_file  # noqa: E402  (the emulator test's own comparison)

SANITIZER_MARKS = ("runtime error:", "ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "SUMMARY: AddressSanitizer",
                   "SUMMARY: UndefinedBehaviorSanitizer")


class Missing(Exception):
    """Something the test needs is not there (exit 2)."""


class Config:
    """What a game tells the test (its driver calls configure())."""

    def __init__(self, root, game_json, disc, scripts_dir, expected_dir, port_dir=None, venv_bin=None,
                 ubsan_suppressions=None, m32_log_exact=(), m32_note="", run_timeout=600, before_scripts=None,
                 after_script=None):
        self.root = Path(root)
        self.game_json = Path(game_json)
        g = json.loads(self.game_json.read_text())
        self.game_id = g["id"]
        self.env_prefix = g.get("env_prefix", g["id"].upper())
        self.disc = Path(disc)
        self.scripts_dir = Path(scripts_dir)
        self.expected_dir = Path(expected_dir)
        self.port_dir = Path(port_dir) if port_dir else self.root / "port"
        self.venv_bin = Path(venv_bin) if venv_bin else None
        self.ubsan_suppressions = Path(ubsan_suppressions) if ubsan_suppressions else None
        # Scripts whose -m32 log must equal the -m64 log byte for byte; m32_note says why the others' frames may
        # differ (a game's heap layout, for instance), printed when their cross-core view still agrees.
        self.m32_log_exact = tuple(m32_log_exact)
        self.m32_note = m32_note
        self.run_timeout = run_timeout
        self.before_scripts = before_scripts
        self.after_script = after_script


CFG = None
RUNNER = []          # the command prefix of a run (--wine: ["wine"])
RUNNER_ENV = {}      # its environment additions (--wine: the prefix, no debug output, SDL's dummy drivers)


def configure(**kwargs):
    global CFG
    CFG = Config(**kwargs)
    return CFG


def tool_env():
    """The environment for cmake/ninja: the system's first, then the venv's bin."""
    env = dict(os.environ)
    if CFG.venv_bin:
        env["PATH"] = env.get("PATH", "") + os.pathsep + str(CFG.venv_bin)
    for tool in ("cmake", "ninja"):
        if shutil.which(tool, path=env["PATH"]) is None:
            raise Missing(f"no {tool} on PATH" + (f" or in {CFG.venv_bin}" if CFG.venv_bin else ""))
    if shutil.which("gcc") is None and shutil.which("cc") is None:
        raise Missing("no host C compiler (gcc)")
    return env


def configure_build(build_dir, options, env):
    """Configures the port into build_dir once (the generated headers are written then); returns the directory."""
    build_dir = CFG.root / build_dir
    if not (build_dir / "CMakeCache.txt").exists():
        print(f"  configure {build_dir.relative_to(CFG.root)} {' '.join(options)}".rstrip())
        subprocess.run(["cmake", "-S", str(CFG.port_dir), "-B", str(build_dir), "-G", "Ninja", *options],
                       check=True, env=env, stdout=subprocess.DEVNULL)
    return build_dir


def build(build_dir, options, jobs, env):
    """Configures (once) and builds the port into build_dir; returns the binary."""
    build_dir = configure_build(build_dir, options, env)
    cmd = ["cmake", "--build", str(build_dir)] + (["-j", str(jobs)] if jobs else [])
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stdout.write(proc.stdout[-4000:] + proc.stderr[-4000:])
        raise RuntimeError(f"the build of {build_dir.relative_to(CFG.root)} failed")
    return build_dir / CFG.game_id


def run_port(binary, script, out_dir, label, cd_speed, env=None):
    """One run of the script, with its SPU trace (<label>.spu.trace); returns (log bytes, record bytes, record, stderr
    text, SPU trace bytes)."""
    out_dir.mkdir(parents=True, exist_ok=True)
    dumps = out_dir / f"{label}_checkpoints"
    if dumps.exists():
        shutil.rmtree(dumps)
    dumps.mkdir()
    log, record, err = out_dir / f"{label}.log", out_dir / f"{label}.json", out_dir / f"{label}.stderr"
    spu = out_dir / f"{label}.spu.trace"
    cmd = [*RUNNER, str(binary), "--disc", str(CFG.disc), "--script", str(script), "--log", str(log), "--record",
           str(record), "--spu-trace", str(spu)]
    if cd_speed:
        cmd += ["--cd-speed", cd_speed]
    run_env = dict(env or os.environ, **{f"{CFG.env_prefix}_PORT_CHECKPOINT_DIR": str(dumps)}, **RUNNER_ENV)
    with open(err, "w") as f:
        proc = subprocess.run(cmd, cwd=CFG.root, env=run_env, stdout=f, stderr=subprocess.STDOUT,
                              timeout=CFG.run_timeout)
    err_text = err.read_text(errors="replace")
    if not record.exists() or record.stat().st_size == 0:
        tail = "\n    ".join(err_text.splitlines()[-15:])
        raise RuntimeError(f"{label}: exit {proc.returncode} without a record ({err}):\n    {tail}")
    rec = json.loads(record.read_text())
    if proc.returncode != 0 or rec.get("status") != 0:
        tail = "\n    ".join(err_text.splitlines()[-8:])
        raise RuntimeError(f"{label}: exit {proc.returncode}, status {rec.get('status')}: {rec.get('reason')} "
                           f"({err}):\n    {tail}")
    return log.read_bytes(), record.read_bytes(), rec, err_text, spu.read_bytes()


def same_output(a, b, what):
    """Byte comparison of two runs' (log, record, SPU trace); returns the differences as text."""
    diffs = []
    for i, name in ((0, "log"), (1, "record"), (4, "SPU trace")):
        if a[i] != b[i]:
            la, lb = a[i].decode(errors="replace").splitlines(), b[i].decode(errors="replace").splitlines()
            first = next((n for n, (x, y) in enumerate(zip(la, lb)) if x != y), min(len(la), len(lb)))
            diffs.append(f"{what}: the {name}s differ from line {first + 1}: "
                         f"{la[first] if first < len(la) else '<end>'!r} vs {lb[first] if first < len(lb) else '<end>'!r}")
    return diffs


def summary(expected, rec):
    """The checkpoints side by side, and the sequences."""
    exp_cps = {cp["name"]: cp for cp in expected["checkpoints"]}
    print(f"  {'checkpoint':<16} {'stage':>5} {'map':>7} {'port frame':>10} {'emulator':>8}  stable hash")
    for cp in rec["checkpoints"]:
        e = exp_cps.get(cp["name"], {})
        if not has_image(cp):
            hashes = "no image" + ("" if not has_image(e) else " (DIFFERS: the emulator hashed it)")
        else:
            mark = "ok" if e.get("gamestate_sha1_stable") == cp["gamestate_sha1_stable"] else \
                f"DIFFERS (emulator {e.get('gamestate_sha1_stable', '?')[:12]})"
            hashes = f"{cp['gamestate_sha1_stable'][:12]} {mark}"
        print(f"  {cp['name']:<16} {cp['stage']:>5} {cp['map']:>#7x} {cp['frame']:>10} {e.get('frame', '-'):>8}  {hashes}")
    view = cross_core_view(rec)
    print("  overlay sequence: " + " ".join(f"({s},{f})" for s, f in view["overlay_sequence"]))
    print("  map sequence:     " + " ".join(f"{m:#x}" for m in view["map_sequence"]))


def check_script(name, args, env, out):
    """The test of one script; returns its failures."""
    script, expected_path = CFG.scripts_dir / f"{name}.json", CFG.expected_dir / f"{name}.json"
    expected = json.loads(expected_path.read_text())
    if expected.get("script_sha1") != sha1_file(script):
        return [f"{script.relative_to(CFG.root)} changed since {expected_path.relative_to(CFG.root)} was recorded "
                "(re-record it with the emulator runner's `run --record`)"]
    out = out / name
    out.mkdir(parents=True, exist_ok=True)
    failures = []
    print(f"port test: {script.relative_to(CFG.root)} vs {expected_path.relative_to(CFG.root)} (cross-core view)")
    try:
        binary = Path(args.exe).resolve() if args.exe else build("build/port", [], args.jobs, env)
        if args.exe:
            print(f"  binary: {binary}{' under wine' if args.wine else ''}")
        run1 = run_port(binary, script, out, "run1", args.cd_speed)
        run2 = run_port(binary, script, out, "run2", args.cd_speed)
        rec = run1[2]
        for what, r in (("expected file", expected), ("port record", rec)):
            diffs = image_diffs(json.loads(script.read_text()), r, what)
            failures += diffs
            if diffs:
                print("  script vs " + what + ": " + "; ".join(diffs))
        print(f"  runs: {rec['frames']} frames (emulator {expected['frames']}), {rec['reason']}; exit 0")
        diffs = same_output(run1, run2, "run 1 vs run 2")
        failures += diffs
        if not diffs:
            print("  determinism: two runs, identical logs, records and SPU traces")
        summary(expected, rec)
        diffs = compare(cross_core_view(expected), cross_core_view(rec))
        if diffs:
            failures += diffs
            print(f"  checkpoint dumps (the checkpoint image): {out / 'run1_checkpoints'}; the emulator's: the "
                  f"emulator runner's `run {script.relative_to(CFG.root)} -v --out DIR`")
        else:
            print(f"  cross-core view: matches {expected_path.relative_to(CFG.root)}")
        if args.m32:
            m32 = build("build/port-m32", ["-DPSXSTACK_M32=ON"], args.jobs, env)
            run = run_port(m32, script, out, "m32", args.cd_speed)
            diffs = compare(cross_core_view(rec), cross_core_view(run[2]))
            failures += [f"-m32: {d}" for d in diffs]
            log_diffs = same_output(run1, run, "-m64 vs -m32")
            if name in CFG.m32_log_exact:
                failures += log_diffs
            if not diffs and not log_diffs:
                print("  -m32: log, record and SPU trace identical to the 64-bit build's")
            elif not diffs:
                print(f"  -m32: the same cross-core view; the frames differ ({CFG.m32_note or 'allowed for this script'}): "
                      + log_diffs[0].split(": ", 1)[1][:120])
        if args.sanitize:
            san = build("build/port-san", ["-DPSXSTACK_SANITIZE=ON"], args.jobs, env)
            ubsan = "print_stacktrace=1" + (f":suppressions={CFG.ubsan_suppressions}" if CFG.ubsan_suppressions else "")
            san_env = dict(os.environ, UBSAN_OPTIONS=ubsan, ASAN_OPTIONS=os.environ.get("ASAN_OPTIONS", "detect_leaks=1"))
            run = run_port(san, script, out, "san", args.cd_speed, san_env)
            reports = [l for l in run[3].splitlines() if any(m in l for m in SANITIZER_MARKS)]
            if reports:
                failures.append(f"sanitizer: {len(reports)} report line(s) in {out / 'san.stderr'}: {reports[0]}")
            else:
                print("  sanitizer: no ASan/UBSan report")
            diffs = same_output(run1, run, "plain vs sanitizer build")
            failures += diffs
            if not diffs:
                print("  sanitizer build: log, record and SPU trace identical to the plain build's")
        if CFG.after_script:
            failures += list(CFG.after_script(name, out, binary, run1))
    except (RuntimeError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
        failures.append(str(e))
    return failures


def audio_renders(binary, out):
    """Whether the port's audio output renders the SPU (a build whose audio.c refuses --wav does not)."""
    proc = subprocess.run([*RUNNER, str(binary), "--max-frames", "1", "--wav", str(out / "audio_probe.wav")],
                          cwd=CFG.root, env=dict(os.environ, **RUNNER_ENV), capture_output=True, text=True, timeout=120)
    return proc.returncode == 0


def build_parser(prog=None):
    jobs_env = f"{CFG.env_prefix}_JOBS" if CFG else "PSXSTACK_JOBS"
    ap = argparse.ArgumentParser(prog=prog, description=__doc__.split("\n")[0])
    ap.add_argument("scripts", nargs="*", help="script names (default: every script with an expected file)")
    ap.add_argument("--m32", action="store_true", help="also the -m32 build; its log and record must be the same")
    ap.add_argument("--sanitize", action="store_true", help="also an ASan/UBSan build; no report allowed")
    ap.add_argument("--cd-speed", choices=("instant", "realistic"), help="the port's CD timing")
    ap.add_argument("--out", help="output directory (default build/port-test/)")
    ap.add_argument("--exe", help="run this binary instead of build/port's (no --m32/--sanitize for it)")
    ap.add_argument("--wine", action="store_true", help="run --exe through wine (a Windows build), headless")
    ap.add_argument("-j", "--jobs", type=int, default=int(os.environ.get(jobs_env, "0")) or None,
                    help=f"build jobs (default: ${jobs_env}, else Ninja's)")
    return ap


def main(argv=None):
    if CFG is None:
        sys.exit("port test: not configured: a game's driver calls port_test.configure() first")
    args = build_parser().parse_args(argv)
    try:
        if not CFG.disc.exists():
            raise Missing(f"no disc image ({CFG.disc})")
        env = tool_env()
        if args.exe and not Path(args.exe).is_file():
            raise Missing(f"no binary at {args.exe}")
        if args.wine:
            if not args.exe:
                raise Missing("--wine needs --exe (a Windows build)")
            if shutil.which("wine") is None:
                raise Missing("no wine on PATH")
            prefix = CFG.root / "build/wine-prefix"
            prefix.mkdir(parents=True, exist_ok=True)
            RUNNER[:] = ["wine"]
            RUNNER_ENV.update(WINEPREFIX=str(prefix), WINEDEBUG="-all", SDL_VIDEO_DRIVER="dummy",
                              SDL_AUDIO_DRIVER="dummy")
        if args.exe and (args.m32 or args.sanitize):
            raise Missing("--m32 and --sanitize build their own binaries: not with --exe")
    except Missing as e:
        print(f"port test: {e}", file=sys.stderr)
        return 2
    names = args.scripts or sorted(p.stem for p in CFG.expected_dir.glob("*.json") if (CFG.scripts_dir / p.name).exists())
    for name in names:
        if not (CFG.scripts_dir / f"{name}.json").exists() or not (CFG.expected_dir / f"{name}.json").exists():
            print(f"port test: no script or no expected file for {name}", file=sys.stderr)
            return 2
    out = Path(args.out) if args.out else CFG.root / "build/port-test"
    out.mkdir(parents=True, exist_ok=True)
    out = out.resolve()

    failed = []
    try:  # a game's checks may compile against the port's generated headers (build/port/gen): build it first
        if args.exe:
            configure_build("build/port", [], env)  # the headers only: the binary under test is --exe's
        else:
            build("build/port", [], args.jobs, env)
    except (RuntimeError, subprocess.CalledProcessError) as e:
        print(f"port test: FAIL: {e}")
        return 1
    if CFG.before_scripts and not args.exe:
        variants = ["m64"] + (["m32"] if args.m32 else []) + (["san"] if args.sanitize else [])
        failures = list(CFG.before_scripts(variants, out))
        if failures:
            print("port test: FAIL\n  " + "\n  ".join(failures))
            failed.append("before_scripts")
    for name in names:
        failures = check_script(name, args, env, out)
        if failures:
            print(f"port test: {name}: FAIL\n  " + "\n  ".join(failures))
            failed.append(name)
    if failed:
        print(f"port test: FAIL ({', '.join(failed)}; outputs: {out})")
        return 1
    print(f"port test: pass ({', '.join(names)}; outputs: {out})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
