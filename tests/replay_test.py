#!/usr/bin/env python3
"""The replay runners' test, without a game, a disc or an emulator (tools/replay/; CI): a fake `pcsx-redux` that
reads the environment the driver sets and writes result.json and the checkpoint images, so the driver's record,
its stable hash, `check`'s comparison and the cross-core view are exercised end to end; plus the pure helpers
(lua_literal, parse_ints, compare, port_test.same_output).

  python3 tests/replay_test.py
"""
import json
import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/replay"))
import emulator  # noqa: E402
import port_test  # noqa: E402

FAKE_REDUX = r'''#!/usr/bin/env python3
# A fake PCSX-Redux: takes the driver's flags, reads the environment run.lua would, and writes what run.lua writes.
import json, os, sys
args = sys.argv[1:]
out = os.environ["PSXSTACK_REPLAY_OUT"]
assert os.path.exists(os.environ["PSXSTACK_REPLAY_SCRIPT"]), "no script"
assert os.path.exists(os.environ["PSXSTACK_REPLAY_PROBES"]), "no probes"
assert os.environ["PSXSTACK_REPLAY_SLOT1_BASE"] == str(0x80082CB0), os.environ["PSXSTACK_REPLAY_SLOT1_BASE"]
assert os.environ["FAKE_REPLAY_OUT"] == out, "the game's alias"
assert "-iso" in args and "-bios" in args and "-dofile" in args, args
lua = args[args.index("-dofile") + 1]
assert os.path.exists(lua), lua
# The emulated machine: the frame a checkpoint lands on depends on the core (FAKE_CORE), the image does not.
frame = 500 + (100 if "-interpreter" in args else 0)
image = bytes(range(64)) * 4                      # 256 bytes; bytes 0..3 are "the playtime" (volatile)
image = bytearray(image); image[0:4] = frame.to_bytes(4, "little")
with open(os.path.join(out, "cp01_first.bin"), "wb") as f:
    f.write(image)
result = {"script": "fake", "frames": frame * 2, "status": "ok",
          "checkpoints": [{"name": "first", "frame": frame, "stage": 3, "map": 0x2D7, "random_index": frame // 7,
                           "gamestate_file": "cp01_first.bin"}],
          "overlay_sequence": [{"frame": 1, "stage": 0, "file": 0}, {"frame": frame - 10, "stage": 3, "file": -1}],
          "map_sequence": [{"frame": 1, "map": 0}, {"frame": frame - 5, "map": 0x2D7}],
          "inputs": [{"frame": 1, "buttons": []}, {"frame": 10, "buttons": ["START"]}]}
with open(os.path.join(out, "result.json"), "w") as f:
    json.dump(result, f)
print("replay: ok script complete")
'''


def check(cond, what):
    if not cond:
        print(f"FAIL: {what}")
        sys.exit(1)


def test_helpers():
    v = {"name": "s", "steps": [{"type": "wait_map", "map": "0x2D7", "timeout": 100}, {"type": "press", "buttons": ["CROSS"]}]}
    lua = emulator.lua_literal(emulator.parse_ints(v))
    check('["map"] = 727' in lua and '"CROSS"' in lua and '["timeout"] = 100' in lua, f"lua_literal: {lua}")
    a = {"checkpoints": [{"name": "x", "stage": 1, "map": 2, "gamestate_sha1_stable": "a"}], "frames": 10, "tree_commit": "1"}
    b = {"checkpoints": [{"name": "x", "stage": 1, "map": 3, "gamestate_sha1_stable": "a"}], "frames": 10, "tree_commit": "2"}
    diffs = emulator.compare(a, b)
    check(diffs == ["checkpoint 0 (x) map: expected 2, got 3"], f"compare: {diffs}")
    check(emulator.compare(a, dict(a, tree_commit="3")) == [], "compare ignores tree_commit")
    rec = {"checkpoints": [{"name": "x", "frame": 5, "stage": 1, "map": 2, "random_index": 9, "gamestate_sha1": "f",
                            "gamestate_sha1_stable": "a"}],
           "overlay_sequence": [{"frame": 1, "stage": 0, "file": 0}], "map_sequence": [{"frame": 1, "map": 0}]}
    view = emulator.cross_core_view(rec)
    check(view == {"checkpoints": [{"name": "x", "stage": 1, "map": 2, "gamestate_sha1_stable": "a"}],
                   "overlay_sequence": [(0, 0)], "map_sequence": [0]}, f"cross_core_view: {view}")
    run1 = (b"a\nb\n", b"{}", {}, "", b"spu")
    run2 = (b"a\nc\n", b"{}", {}, "", b"spu2")
    diffs = port_test.same_output(run1, run2, "x")
    check(len(diffs) == 2 and "logs differ from line 2" in diffs[0] and "SPU trace" in diffs[1], f"same_output: {diffs}")
    check(port_test.same_output(run1, run1, "x") == [], "same_output equal")
    print("helpers: ok")


def test_driver(tmp):
    redux_dir = tmp / "redux"
    (redux_dir / "app/usr/share/pcsx-redux/resources").mkdir(parents=True)
    (redux_dir / "app/usr/share/pcsx-redux/resources/version.json").write_text(
        json.dumps({"version": "fake", "buildId": "0", "changeset": "deadbeef"}))
    (redux_dir / "app/usr/share/pcsx-redux/resources/openbios.bin").write_bytes(b"\0" * 16)
    fake = redux_dir / "pcsx-redux"
    fake.write_text(FAKE_REDUX)
    fake.chmod(fake.stat().st_mode | stat.S_IEXEC)
    (tmp / "game.cue").write_text("FILE game.bin BINARY\n")
    (tmp / "probes.lua").write_text("return {}\n")
    scripts, expected = tmp / "scripts", tmp / "expected"
    scripts.mkdir()
    (scripts / "fake.json").write_text(json.dumps({"name": "fake", "max_frames": 2000,
                                                   "steps": [{"type": "wait_map", "map": "0x2D7"}, {"type": "checkpoint", "name": "first"}]}))
    game_json = tmp / "game.json"
    game_json.write_text(json.dumps({"id": "fake", "env_prefix": "FAKE", "memory": {"slots": [{"base": "0x80082CB0"}]}}))
    emulator.configure(root=tmp, game_json=game_json, redux_dir=redux_dir, iso=tmp / "game.cue", scripts_dir=scripts,
                       expected_dir=expected, probes=tmp / "probes.lua", volatile_ranges=((0, 4),))
    # record (two runs identical), then check against the expected file, then the cross-core view on the other core
    rc = emulator.main(["run", str(scripts / "fake.json"), "--record", "--repeat", "2", "--out", str(tmp / "out")])
    check(rc == 0, "run --record")
    rec = json.loads((expected / "fake.json").read_text())
    check(rec["frames"] == 1000 and rec["bios"]["name"] == "openbios" and rec["emulator"]["changeset"] == "deadbeef",
          f"record: {rec}")
    cp = rec["checkpoints"][0]
    check(cp["name"] == "first" and cp["stage"] == 3 and cp["map"] == 0x2D7 and cp["frame"] == 500
          and cp["gamestate_sha1"] != cp["gamestate_sha1_stable"], f"checkpoint: {cp}")
    check(rec["script_sha1"] == emulator.sha1_file(scripts / "fake.json"), "script_sha1")
    check(emulator.main(["check"]) == 0, "check")
    # the interpreter core lands 100 frames later: the full record differs, the cross-core view does not
    check(emulator.main(["run", str(scripts / "fake.json"), "--out", str(tmp / "out2")]) == 0, "run again")
    check(emulator.main(["run", str(scripts / "fake.json"), "--interpreter", "--out", str(tmp / "out3")]) == 0,
          "run --interpreter: the cross-core view matches")
    (scripts / "fake.json").write_text((scripts / "fake.json").read_text() + "\n")
    check(emulator.main(["check"]) == 1, "check refuses a changed script")
    print("driver: ok")


def main():
    test_helpers()
    with tempfile.TemporaryDirectory(prefix="psxstack_replay_test_") as d:
        os.environ.pop("FAKE_JOBS", None)
        test_driver(Path(d))
    print("replay_test: pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
