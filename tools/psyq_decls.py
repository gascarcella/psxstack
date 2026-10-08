#!/usr/bin/env python3
"""Do a game's Psy-Q declarations agree with the stack's? (DECISIONS "Psy-Q declarations: the stack's")

The shim (psyq/) is compiled against include/psxstack/psyq/*.h; a game's units are compiled against the game's own
recovered declarations (dw2003's include/psyq/*.h, dcb's include/game.h). The two meet only at link time, so a
disagreement is silent: an `s32` parameter where the shim takes a pointer is read as a 64-bit register whose top
half the caller never set. This script compiles one translation unit per side, each holding
`__typeof__(f) *psxstack_decl_f;` for every function both sides declare, and compares the function types the
compiler recorded in the DWARF (readelf): the parameter count, each parameter's kind (an integer of n bytes, a
float, a pointer, a struct by value) and the return's. Names, typedefs and pointer targets do not matter; the
register ABI does.

  tools/psyq_decls.py --stack-root DIR --gen-include DIR --out DIR --game-header FILE... [-I DIR]... [-D X]...
                      [--cc gcc] [--cflags FLAG]... [-v]
  (a game runs it through its tools/port_inventory.py `decls`, which fills the flags in from its configuration)

Per function, one of:
  same          the types agree exactly
  compatible    they differ only in signedness, or the game ignores the value the stack returns
  unprototyped  the game declares `f()`: the caller promotes its arguments, the parameters are not checked
  MISMATCH      a parameter's or the return's kind or width differs, or the count: a real ABI difference
  missing       the stack declares it and the game does not (nothing to check; listed with -v)
Exit 0 when nothing mismatches. Needs gcc (or --cc) and readelf on PATH; no CMake.
"""
import argparse
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

STACK_ROOT = Path(__file__).resolve().parent.parent

PROTO = re.compile(r"^[ \t]*(?!typedef\b|extern\b|#|return\b)[A-Za-z_][\w \t*]*?[ \t*]([A-Za-z_]\w*)[ \t]*\([^;{}]*\)[ \t]*;",
                   re.M)


def strip_code(t):
    """Comments blanked (newlines kept)."""
    return re.sub(r"/\*.*?\*/|//[^\n]*", lambda m: re.sub(r"[^\n]", " ", m.group()), t, flags=re.S)


def stack_headers(root):
    return sorted((Path(root) / "include" / "psxstack" / "psyq").glob("*.h"))


def declared_functions(text):
    """The function names a header's text declares (prototypes, K&R declarations); macros and data are not among them."""
    return sorted({m.group(1) for m in PROTO.finditer(strip_code(text))})


# ---------------------------------------------------------------------------------------------------------------------
# DWARF: the function types of the psxstack_decl_* variables

DIE = re.compile(r"^ <(\d+)><([0-9a-f]+)>: Abbrev Number: \d+ \(DW_TAG_(\w+)\)")
ATTR = re.compile(r"^ {4}<[0-9a-f]+> +DW_AT_(\w+)\s*:\s*(.*)$")


def read_dwarf(obj):
    """-> {offset: (tag, attrs, [child offsets])} of the object's .debug_info (readelf --debug-dump=info)."""
    out = subprocess.run(["readelf", "--debug-dump=info", str(obj)], capture_output=True, text=True, check=True).stdout
    dies, stack, cur = {}, [], None
    for line in out.splitlines():
        m = DIE.match(line)
        if m:
            depth, off, tag = int(m.group(1)), int(m.group(2), 16), m.group(3)
            cur = (tag, {}, [])
            dies[off] = cur
            while len(stack) > depth:
                stack.pop()
            if stack:
                dies[stack[-1]][2].append(off)
            stack.append(off)
            continue
        m = ATTR.match(line)
        if m and cur is not None:
            name, val = m.group(1), m.group(2).strip()
            if name == "type":
                t = re.match(r"<0x([0-9a-f]+)>", val)
                cur[1]["type"] = int(t.group(1), 16) if t else None
            elif name == "name":
                cur[1]["name"] = val.split(": ")[-1].strip()
            elif name == "byte_size":
                cur[1]["byte_size"] = int(val.split()[0], 0)
            elif name == "encoding":
                cur[1]["encoding"] = val
            elif name == "prototyped":
                cur[1]["prototyped"] = val.strip() not in ("0", "")
    return dies


def kind_of(dies, off):
    """A type's register-ABI kind: void, ptr, i<n>/u<n>/f<n>/b<n> (n bytes), struct<n>, union<n>, fn, or ?."""
    seen = set()
    while off is not None and off not in seen:
        seen.add(off)
        tag, attrs, _ = dies[off]
        if tag in ("typedef", "const_type", "volatile_type", "atomic_type", "restrict_type"):
            off = attrs.get("type")
            if off is None:
                return "void"
            continue
        if tag in ("pointer_type", "reference_type", "array_type"):
            return "ptr"
        if tag == "base_type":
            n, enc = attrs.get("byte_size", 0), attrs.get("encoding", "")
            if "float" in enc:
                return f"f{n}"
            if "boolean" in enc:
                return f"b{n}"
            if "unsigned" in enc:
                return f"u{n}"
            return f"i{n}"
        if tag == "enumeration_type":
            return f"i{attrs.get('byte_size', 4)}"
        if tag in ("structure_type", "union_type"):
            return f"{'struct' if tag == 'structure_type' else 'union'}{attrs.get('byte_size', '?')}"
        if tag == "subroutine_type":
            return "fn"
        return "?"
    return "void"


def function_types(dies):
    """-> {name: (prototyped, return kind, [param kinds] or None, varargs)} from the psxstack_decl_* variables."""
    out = {}
    for off, (tag, attrs, _) in dies.items():
        if tag != "variable" or not attrs.get("name", "").startswith("psxstack_decl_"):
            continue
        name = attrs["name"][len("psxstack_decl_"):]
        ptr = dies.get(attrs.get("type"))
        if not ptr or ptr[0] != "pointer_type":
            out[name] = (True, "?", None, False)
            continue
        fn = dies.get(ptr[1].get("type"))
        if not fn or fn[0] != "subroutine_type":
            out[name] = (True, "?", None, False)
            continue
        ret = kind_of(dies, fn[1].get("type")) if fn[1].get("type") is not None else "void"
        params, varargs = [], False
        for c in fn[2]:
            ctag, cattrs, _ = dies[c]
            if ctag == "formal_parameter":
                params.append(kind_of(dies, cattrs.get("type")))
            elif ctag == "unspecified_parameters":
                varargs = True
        out[name] = (fn[1].get("prototyped", False), ret, params, varargs)
    return out


# ---------------------------------------------------------------------------------------------------------------------
# The comparison

def same_width_int(a, b):
    return a[0] in "iub" and b[0] in "iub" and a[1:] == b[1:]


def compare(stack, game):
    """-> (verdict, detail) for one function: stack = (prototyped, ret, params, varargs), game likewise."""
    sp, sret, sparams, svar = stack
    gp, gret, gparams, gvar = game
    if sparams is None or gparams is None:
        return "MISMATCH", "not a function on one side"
    notes = []
    if sret != gret:
        if same_width_int(sret, gret):
            notes.append(f"return {gret} for {sret}")
        elif gret == "void" and sret != "void":
            notes.append(f"return ignored ({sret})")
        else:
            return "MISMATCH", f"return: the game expects {gret}, the stack gives {sret}"
    if not gp:
        return "unprototyped", f"the game declares it without a prototype; the stack takes ({', '.join(sparams)})"
    if len(sparams) != len(gparams) or svar != gvar:
        return "MISMATCH", (f"{len(gparams)}{'...' if gvar else ''} parameters in the game, "
                            f"{len(sparams)}{'...' if svar else ''} in the stack")
    for i, (s, g) in enumerate(zip(sparams, gparams)):
        if s == g:
            continue
        if same_width_int(s, g):
            notes.append(f"parameter {i + 1} {g} for {s}")
        else:
            return "MISMATCH", f"parameter {i + 1}: the game passes {g}, the stack takes {s}"
    return ("compatible", "; ".join(notes)) if notes else ("same", "")


# ---------------------------------------------------------------------------------------------------------------------

def write_tu(path, includes, names):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("/* generated by tools/psyq_decls.py */\n" + "".join(f'#include "{i}"\n' for i in includes)
                    + "".join(f"__typeof__({n}) *psxstack_decl_{n};\n" for n in names))


def compile_tu(cc, cflags, src, obj):
    r = subprocess.run([cc, "-g", "-O0", "-c", str(src), "-o", str(obj)] + cflags, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"psyq_decls: {src.name} does not compile:\n{r.stderr}")


def run(stack_root, out, gen_include, game_headers, game_cflags, cc="gcc", verbose=False, stack_cflags=None):
    """-> (counts Counter, lines). stack_cflags: the stack side's extra flags (default: the stack's includes)."""
    stack_root, out = Path(stack_root), Path(out)
    names = sorted({n for h in stack_headers(stack_root) for n in declared_functions(h.read_text())})
    # The stack side.
    s_inc = [f"-I{gen_include}", f"-I{stack_root / 'include'}", f"-I{stack_root / 'include/psxstack'}", "-DPC_PORT"]
    write_tu(out / "decls_stack.c", [f"psxstack/psyq/{h.name}" for h in stack_headers(stack_root)], names)
    compile_tu(cc, (stack_cflags or []) + s_inc, out / "decls_stack.c", out / "decls_stack.o")
    stack = function_types(read_dwarf(out / "decls_stack.o"))
    # The game side: which of the names its headers declare (preprocessed, so that #if blocks are honoured).
    write_tu(out / "decls_game.c", [str(Path(h).resolve()) for h in game_headers], [])
    r = subprocess.run([cc, "-E", "-P", str(out / "decls_game.c")] + game_cflags, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"psyq_decls: the game's headers do not preprocess:\n{r.stderr}")
    text = strip_code(r.stdout)
    declared = [n for n in names if re.search(rf"\b{n}[ \t]*\(", text)]
    write_tu(out / "decls_game.c", [str(Path(h).resolve()) for h in game_headers], declared)
    compile_tu(cc, game_cflags, out / "decls_game.c", out / "decls_game.o")
    game = function_types(read_dwarf(out / "decls_game.o"))
    counts, lines = Counter(), []
    for n in names:
        if n not in game:
            counts["missing"] += 1
            if verbose:
                lines.append(f"  missing       {n}")
            continue
        verdict, detail = compare(stack[n], game[n])
        counts[verdict] += 1
        if verdict != "same" or verbose:
            lines.append(f"  {verdict:<13} {n}" + (f": {detail}" if detail else ""))
    return counts, lines


def report(counts, lines, what):
    print(f"psyq_decls: {what}")
    for ln in lines:
        print(ln)
    total = sum(counts.values())
    print(f"psyq_decls: {total} functions the stack declares: " + ", ".join(
        f"{counts[k]} {k}" for k in ("same", "compatible", "unprototyped", "MISMATCH", "missing") if counts[k]))
    return 1 if counts["MISMATCH"] else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stack-root", default=str(STACK_ROOT))
    ap.add_argument("--gen-include", required=True, help="a directory holding the game's psxstack_game_gen.h")
    ap.add_argument("--out", required=True, help="where the two translation units and objects go")
    ap.add_argument("--game-header", action="append", required=True, help="a game header that declares Psy-Q functions")
    ap.add_argument("-I", dest="inc", action="append", default=[], help="the game side's include directories")
    ap.add_argument("-D", dest="defs", action="append", default=[], help="the game side's defines (PC_PORT is not implied)")
    ap.add_argument("--cflags", action="append", default=[], help="more flags for the game side")
    ap.add_argument("--cc", default="gcc")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args(argv)
    cflags = [f"-I{a.gen_include}"] + [f"-I{d}" for d in a.inc] + [f"-D{d}" for d in a.defs] + a.cflags
    counts, lines = run(a.stack_root, a.out, a.gen_include, a.game_header, cflags, a.cc, a.verbose)
    return report(counts, lines, f"{len(a.game_header)} game header(s) against {a.stack_root}/include/psxstack/psyq")


if __name__ == "__main__":
    sys.exit(main())
