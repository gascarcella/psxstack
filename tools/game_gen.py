#!/usr/bin/env python3
"""The game description -> psxstack_game_gen.h (GAME_CONTRACT.md "1. game.json", "The generated header").

  tools/game_gen.py GAME.json --out DIR/psxstack_game_gen.h      # the header (C and C++)
  tools/game_gen.py GAME.json --cmake DIR/psxstack_game.cmake    # the id and title as CMake variables (the target names)
  tools/game_gen.py GAME.json --check                            # validate only

The description is validated against schema/game.schema.json (with `jsonschema` when it is importable, else by hand:
the same required keys, types, patterns and ranges) and against what the schema cannot say: the slots are in address
order and contiguous with each other and with the heap, the rates include the nominal one. Every string reaches the
header as a C string literal; addresses as hex integer constants. Nothing reads the description at run time.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCHEMA = ROOT / "schema/game.schema.json"


class GameError(Exception):
    pass


def fail(msg):
    raise GameError(msg)


def addr(v, what):
    if isinstance(v, bool):
        fail(f"{what}: a hex string or an integer, not a boolean")
    if isinstance(v, int):
        if v < 0:
            fail(f"{what}: negative")
        return v
    if isinstance(v, str) and re.fullmatch(r"0x[0-9A-Fa-f]{1,8}", v):
        return int(v, 16)
    fail(f"{what}: a hex string (\"0x80082CB0\") or an integer, not {v!r}")


def validate_schema(game, schema):
    """jsonschema when available, else the hand checks."""
    try:
        import jsonschema
    except ImportError:
        jsonschema = None
    if jsonschema is not None:
        try:
            jsonschema.validate(game, schema, cls=jsonschema.Draft202012Validator)
        except jsonschema.ValidationError as e:
            path = "/".join(str(p) for p in e.absolute_path) or "(top)"
            fail(f"{path}: {e.message}")
        return
    # By hand: the schema's required keys, types and patterns.
    if not isinstance(game, dict):
        fail("the description must be an object")
    for k in ("schema", "id", "title", "discs", "video", "memory"):
        if k not in game:
            fail(f"missing key: {k}")
    for k in game:
        if k not in schema["properties"]:
            fail(f"unknown key: {k}")
    if game["schema"] != 1:
        fail("schema: must be 1")
    if not isinstance(game["id"], str) or not re.fullmatch(r"[a-z][a-z0-9_]{0,31}", game["id"]):
        fail("id: lower-case identifier")
    if not isinstance(game["title"], str) or not game["title"]:
        fail("title: a non-empty string")
    if "env_prefix" in game and not re.fullmatch(r"[A-Z][A-Z0-9]{0,15}", str(game["env_prefix"])):
        fail("env_prefix: upper-case letters and digits")
    discs = game["discs"]
    if not isinstance(discs, list):
        fail("discs: a list (empty for a disc-free program, which then runs with the disc check off)")
    for i, d in enumerate(discs):
        for k in ("label", "serial", "sha1", "size", "region"):
            if k not in d:
                fail(f"discs/{i}: missing {k}")
        if not re.fullmatch(r"[A-Z]{4}-[0-9]{5}", d["serial"]):
            fail(f"discs/{i}/serial: XXXX-NNNNN")
        if not re.fullmatch(r"[0-9a-f]{40}", d["sha1"]):
            fail(f"discs/{i}/sha1: 40 lower-case hex digits")
        if not isinstance(d["size"], int) or isinstance(d["size"], bool) or d["size"] < 1:
            fail(f"discs/{i}/size: a positive integer")
        if d["region"] not in ("PAL", "NTSC-U", "NTSC-J"):
            fail(f"discs/{i}/region: PAL, NTSC-U or NTSC-J")
    video = game["video"]
    if not isinstance(video, dict) or video.get("rate") not in (50, 60):
        fail("video.rate: 50 or 60")
    for r in video.get("rates", []):
        if r not in (50, 60):
            fail("video.rates: 50 or 60 each")
    mem = game["memory"]
    if not isinstance(mem, dict):
        fail("memory: an object")
    for k in ("ram", "slots", "heap"):
        if k not in mem:
            fail(f"memory: missing {k}")
    if not isinstance(mem["slots"], list) or not 1 <= len(mem["slots"]) <= 8:
        fail("memory.slots: 1 to 8 slots")
    for i, s in enumerate(mem["slots"]):
        for k in ("name", "base", "size"):
            if k not in s:
                fail(f"memory/slots/{i}: missing {k}")
        if not re.fullmatch(r"[a-z][a-z0-9_]{0,31}", str(s["name"])):
            fail(f"memory/slots/{i}/name: lower-case identifier")
    for k in ("start", "end"):
        if k not in mem["heap"]:
            fail(f"memory/heap: missing {k}")


def load(path):
    try:
        game = json.loads(Path(path).read_text())
    except (OSError, json.JSONDecodeError) as e:
        fail(f"{path}: {e}")
    schema = json.loads(SCHEMA.read_text())
    validate_schema(game, schema)
    return game


def resolve(game):
    """The description with addresses as integers and defaults filled in, checked for consistency."""
    g = {
        "id": game["id"],
        "title": game["title"],
        "env_prefix": game.get("env_prefix", game["id"].upper()),
        "discs": [dict(d, cue=d.get("cue", "")) for d in game["discs"]],
        "rate": game["video"]["rate"],
        "rates": game["video"].get("rates") or [game["video"]["rate"]],
        "rate_note": game["video"].get("rate_note", ""),
        "about": game.get("launcher", {}).get("about", game["title"]),
        "disc_hint": game.get("launcher", {}).get("disc_hint", ""),
        "website": game.get("launcher", {}).get("website", ""),
    }
    if g["rate"] not in g["rates"]:
        fail("video.rates: must include video.rate")
    if len(set(g["rates"])) != len(g["rates"]):
        fail("video.rates: a rate is listed twice")
    mem = game["memory"]
    g["ram_base"] = addr(mem["ram"]["base"], "memory.ram.base")
    g["ram_size"] = addr(mem["ram"]["size"], "memory.ram.size")
    slots = []
    for i, s in enumerate(mem["slots"]):
        slots.append({"name": s["name"], "base": addr(s["base"], f"memory.slots[{i}].base"),
                      "size": addr(s["size"], f"memory.slots[{i}].size")})
    for i in range(1, len(slots)):
        prev, cur = slots[i - 1], slots[i]
        if cur["base"] != prev["base"] + prev["size"]:
            fail(f"memory.slots[{i}] ({cur['name']}): base {cur['base']:#x} is not where slot {i - 1} ({prev['name']}) "
                 f"ends, {prev['base'] + prev['size']:#x}: the slots must be in address order and contiguous")
    heap_start = addr(mem["heap"]["start"], "memory.heap.start")
    heap_end = addr(mem["heap"]["end"], "memory.heap.end")
    heap_size = addr(mem["heap"].get("host_size", 0x400000), "memory.heap.host_size")
    last = slots[-1]
    if heap_start != last["base"] + last["size"]:
        fail(f"memory.heap.start {heap_start:#x} is not where the last slot ({last['name']}) ends, "
             f"{last['base'] + last['size']:#x}: the heap must follow the slots")
    if heap_end <= heap_start:
        fail("memory.heap.end must be above memory.heap.start")
    if heap_size < heap_end - heap_start:
        fail(f"memory.heap.host_size {heap_size:#x} is smaller than the PS1 heap ({heap_end - heap_start:#x})")
    ram_end = g["ram_base"] + g["ram_size"]
    for s in slots:
        if not g["ram_base"] <= s["base"] < ram_end or s["base"] + s["size"] > ram_end:
            fail(f"memory.slots ({s['name']}): outside the RAM")
    if heap_end > ram_end:
        fail("memory.heap.end: outside the RAM")
    for n in set(s["name"] for s in slots):
        if sum(1 for s in slots if s["name"] == n) > 1:
            fail(f"memory.slots: the name {n!r} is used twice")
    g.update(slots=slots, heap_start=heap_start, heap_end=heap_end, heap_size=heap_size)
    g["bios_standin"] = [{"address": addr(b["address"], f"memory.bios_standin[{i}].address"), "text": b["text"]}
                         for i, b in enumerate(mem.get("bios_standin", []))]
    return g


def cstr(s):
    """A C string literal (also valid C++): UTF-8 kept, the rest escaped."""
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ord(ch) < 0x20 or ch == "\x7f":
            out.append(f"\\{ord(ch):03o}")
        elif ch == "?":
            out.append("\\?")  # no trigraphs
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def header(g, source):
    L = []
    w = L.append
    w(f"/* Generated by tools/game_gen.py from {source}; do not edit. GAME_CONTRACT.md \"The generated header\". */")
    w("#ifndef PSXSTACK_GAME_GEN_H")
    w("#define PSXSTACK_GAME_GEN_H")
    w("")
    w("/* Identity */")
    w(f"#define PSXSTACK_GAME_ID {cstr(g['id'])}")
    w(f"#define PSXSTACK_GAME_ID_UPPER {cstr(g['id'].upper())}")
    w(f"#define PSXSTACK_GAME_ID_IDENT {g['id']} /* the id as a bare token, for pasted symbol names */")
    w(f"#define PSXSTACK_GAME_TITLE {cstr(g['title'])}")
    w(f"#define PSXSTACK_GAME_ENV_PREFIX {cstr(g['env_prefix'])}")
    w("")
    w("/* Timing: the nominal vsyncs per second, and the rates the launcher offers (the nominal one among them) */")
    w(f"#define PSXSTACK_GAME_RATE {g['rate']}")
    w(f"#define PSXSTACK_GAME_RATE_COUNT {len(g['rates'])}")
    w(f"#define PSXSTACK_GAME_RATES {{ {', '.join(str(r) for r in g['rates'])} }}")
    w(f"#define PSXSTACK_GAME_RATE_NOTE {cstr(g['rate_note'])}")
    w("")
    w("/* Memory: the PS1 RAM as the game addresses it, the overlay slots (1-based: the hook macros' `tier`), the heap */")
    w(f"#define PSXSTACK_GAME_RAM_BASE {g['ram_base']:#010x}u")
    w(f"#define PSXSTACK_GAME_RAM_SIZE {g['ram_size']:#x}u")
    w(f"#define PORT_SLOT_COUNT {len(g['slots'])}")
    base0 = g["slots"][0]["base"]
    for i, s in enumerate(g["slots"], 1):
        w(f"#define PORT_SLOT{i}_BASE {s['base']:#010x}u")
        w(f"#define PORT_SLOT{i}_SIZE {s['size']:#x}u")
        w(f"#define PORT_SLOT{i}_NAME {cstr(s['name'])}")
        w(f"#define PORT_SLOT{i}_OFS {s['base'] - base0:#x}u /* from the arena's start */")
        w(f"#define port_slot{i} (port_arena + PORT_SLOT{i}_OFS)")
    w(f"#define PORT_HEAP_START_ADDR {g['heap_start']:#010x}u")
    w(f"#define PORT_HEAP_END_ADDR {g['heap_end']:#010x}u")
    w(f"#define PORT_HEAP_OFS {g['heap_start'] - base0:#x}u")
    w(f"#define PORT_HEAP_SIZE {g['heap_size']:#x}u /* the host heap region; the PS1's is "
      f"{g['heap_end'] - g['heap_start']:#x} */")
    w("#define PORT_ARENA_SIZE (PORT_HEAP_OFS + PORT_HEAP_SIZE)")
    w("")
    w("/* The discs the port accepts: the whole BIN's SHA-1 and size; `label` completes \"This is <label>.\" and "
      "\"<file> is not <label>\" */")
    w("typedef struct PsxstackGameDisc {")
    w("    const char *label, *serial, *sha1, *cue, *region;")
    w("    unsigned long long size;")
    w("} PsxstackGameDisc;")
    w(f"#define PSXSTACK_GAME_DISC_COUNT {len(g['discs'])}")
    items = ", ".join("{ " + ", ".join([cstr(d["label"]), cstr(d["serial"]), cstr(d["sha1"]), cstr(d["cue"]),
                                        cstr(d["region"]), f"{d['size']}ull"]) + " }" for d in g["discs"])
    if g["discs"]:
        w(f"#define PSXSTACK_GAME_DISCS {{ {items} }}")
    else:
        w('#define PSXSTACK_GAME_DISCS { { "", "", "", "", "", 0ull } } /* none (a disc-free program): one empty entry '
          'keeps the array non-empty */')
    w("")
    w("/* Reads the game makes from the BIOS ROM: what the stand-in holds at each address */")
    w("typedef struct PsxstackGameBiosStandin {")
    w("    unsigned address;")
    w("    const char *text;")
    w("} PsxstackGameBiosStandin;")
    w(f"#define PSXSTACK_GAME_BIOS_STANDIN_COUNT {len(g['bios_standin'])}")
    if g["bios_standin"]:
        items = ", ".join(f"{{ {b['address']:#010x}u, {cstr(b['text'])} }}" for b in g["bios_standin"])
        w(f"#define PSXSTACK_GAME_BIOS_STANDINS {{ {items} }}")
    else:
        w("#define PSXSTACK_GAME_BIOS_STANDINS { { 0u, \"\" } } /* none: one empty entry keeps the array non-empty */")
    w("")
    w("/* The launcher's strings */")
    w(f"#define PSXSTACK_GAME_ABOUT {cstr(g['about'])}")
    w(f"#define PSXSTACK_GAME_DISC_HINT {cstr(g['disc_hint'])}")
    w(f"#define PSXSTACK_GAME_WEBSITE {cstr(g['website'])}")
    w("")
    w("#endif /* PSXSTACK_GAME_GEN_H */")
    return "\n".join(L) + "\n"


def cmake_vars(g, source):
    def q(s):
        return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace(";", "\\;") + '"'
    return (f"# Generated by tools/game_gen.py from {source}; do not edit.\n"
            f"set(PSXSTACK_GAME_ID {q(g['id'])})\n"
            f"set(PSXSTACK_GAME_TITLE {q(g['title'])})\n"
            f"set(PSXSTACK_GAME_ENV_PREFIX {q(g['env_prefix'])})\n"
            f"set(PSXSTACK_GAME_RATE {g['rate']})\n")


def write(path, text):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", help="the game description (game.json)")
    ap.add_argument("--out", help="write the C/C++ header here")
    ap.add_argument("--cmake", help="write the CMake variables here")
    ap.add_argument("--check", action="store_true", help="validate only")
    args = ap.parse_args()
    try:
        g = resolve(load(args.game))
    except GameError as e:
        print(f"game_gen: {args.game}: {e}", file=sys.stderr)
        return 1
    if args.out:
        write(args.out, header(g, Path(args.game).name))
    if args.cmake:
        write(args.cmake, cmake_vars(g, Path(args.game).name))
    if not args.out and not args.cmake:
        if not args.check:
            sys.stdout.write(header(g, Path(args.game).name))
        else:
            print(f"game_gen: {args.game}: ok ({g['id']}: {len(g['slots'])} slots, {len(g['discs'])} discs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
