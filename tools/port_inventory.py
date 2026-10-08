#!/usr/bin/env python3
"""psxstack's PC-port inventory: what a game's C needs from the PS1, and the host-compile gate (docs/PORT.md
"Compiling the game C for the host"). A game's own tools/port_inventory.py configures it (configure()) and adds its
own commands; the game's CLI is:

  tools/port_inventory.py counts                 # the inventory's numbers on this tree
  tools/port_inventory.py counts --sites KIND    # file:line of every site of one kind
  tools/port_inventory.py probe [FILES...]       # host-compile gate at -m64 (exit 0 = clean)
  tools/port_inventory.py probe --warnings       # also count the non-gating -Wall warnings
  tools/port_inventory.py probe --m32            # the same at -m32 (compile only)
  tools/port_inventory.py probe --target windows # the same with llvm-mingw's clang (Windows x86_64)
  tools/port_inventory.py link                   # probe, then nm: duplicate / undefined globals
  tools/port_inventory.py decls [-v]             # the game's Psy-Q declarations vs the stack's (tools/psyq_decls.py)

counts reads only tracked files (the game's C and headers, its symbol files): comments and strings are stripped,
identifiers after `.`/`->`, prototypes and definitions are not calls. Site kinds for --sites (KIND or KIND:TAG, e.g.
`psyq:LIBGPU`, `psyq:DrawSync`, `addr:<slot name>`, `addr:cast`, `port:PTR_ADD`):
  psyq         calls of Psy-Q functions (names and libraries: the `// LIBxxx.LIB/OBJ.OBJ` comments of the symbol files)
  gpu-macro    uses of libgpu.h's function-like macros;  gpu-prim  mentions of its primitive types
  gte          uses of the gte_* macros of gtemac.h
  rodata, asm  INCLUDE_RODATA / INCLUDE_ASM lines
  size         object_new / heap_funcs.alloc* / heap_funcs.bzero calls whose size argument is an integer literal
  addr         constants in PS1 RAM or the scratchpad (0x1F800000-0x1F8003FF). Tags: the region (exe, the slots by
               name, heap, high, scratchpad) and one of wrapped (inside an ALL-CAPS macro call, also tagged with the
               macro's name), cast (a pointer cast and no macro), bare (neither)
  wait         loops with an empty body (busy-waits; tags: empty, or hooked + the macro when the body is one macro)
  late         uses of unprefixed `func_<addr>` names whose address is in an overlay slot (late-bound calls)
  port         uses of the macros the game's port.h defines (found at run time; tag: the macro)

probe compiles each file with the host gcc (PROBE_FLAGS + GATE below) into <out>/<width>/, with INCLUDE_ASM/
INCLUDE_RODATA empty and every gte_* macro a no-op (override headers generated there: include_asm.h, and the GTE
header at the path the game includes it by, `gtemac_include`, found as `gtemac` relative to the game's include
directories: the first game's include/psyq/gtemac.h is `psyq/gtemac.h`, a game's include/gte.h is `gte.h`). A game
whose symbol files carry no `// LIBxxx.LIB/OBJ.OBJ` comments gives the libraries with `psyq_libraries`, a callable
returning {name: "LIBGPU", ...}, consulted for every symbol the files leave untagged. A file fails on a gating
diagnostic (GATE), on any other compiler error, or on an assembler error (MIPS inline asm). GCC 14+ gets -fpermissive
so that its other default errors stay warnings, as on GCC 13: the gate is the same on every version.
--target windows compiles with llvm-mingw's clang (tools/llvm-mingw; the Windows cross build's compiler) with clang's
own default errors back to warnings (CLANG_GAME_FLAGS), so a unit that compiles for Linux but not for Windows shows.
Needs: gcc and nm on PATH. Nothing here touches the PS1 build.
"""
import argparse
import bisect
import os
import re
import shutil
import subprocess
import sys
import time
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

STACK_ROOT = Path(__file__).resolve().parent.parent


class Config:
    """What a game tells the inventory (its tools/port_inventory.py calls configure())."""

    def __init__(self, root, game_json, sources, headers, include_dirs, symbol_files, module_of=None, gtemac=None,
                 include_asm=None, port_h=None, psyq_dir=None, tool_dirs=(), defines=("NON_MATCHING",), out=None,
                 psyq_decl_headers=None, gtemac_include=None, psyq_libraries=None):
        import json
        self.root = Path(root)
        self.game_json = Path(game_json)
        self.sources = sources            # () -> [Path]: the game's C units
        self.headers = headers            # () -> [Path]: the game's headers (structs, the macro counts)
        self.include_dirs = [Path(d) for d in include_dirs]
        self.symbol_files = [Path(f) for f in symbol_files]
        self.module_of = module_of or (lambda rel: rel.split("/")[-2] if "/" in rel else rel)
        self.gtemac = Path(gtemac) if gtemac else None
        # The path the game's C includes the GTE header by (the override is written there, first on the include
        # path): given, else `gtemac` relative to the first include directory that holds it, else psyq/gtemac.h.
        self.gtemac_include = gtemac_include or gtemac_include_path(self.gtemac, [Path(d) for d in include_dirs])
        self.psyq_libraries = psyq_libraries    # () -> {name: library}, for symbol files without library comments
        self.include_asm = Path(include_asm) if include_asm else None
        self.port_h = Path(port_h) if port_h else None
        self.psyq_dir = Path(psyq_dir) if psyq_dir else self.root / "include" / "psyq"
        # The game headers that declare the Psy-Q functions its units call (`decls`: tools/psyq_decls.py). Default:
        # the recovered psyq_dir/*.h; a game that redeclares them in one header names it.
        self.psyq_decl_headers = ([Path(h) for h in psyq_decl_headers] if psyq_decl_headers
                                  else sorted(self.psyq_dir.glob("*.h")) if self.psyq_dir.is_dir() else [])
        self.tool_dirs = [Path(d) for d in tool_dirs]
        self.defines = list(defines)
        self.out = Path(out) if out else self.root / "build" / "port_inventory"
        g = json.loads(self.game_json.read_text())
        addr = lambda v: int(v, 0) if isinstance(v, str) else int(v)
        ram = addr(g["memory"]["ram"]["base"]), addr(g["memory"]["ram"]["size"])
        slots = [(addr(sl["base"]), sl["name"]) for sl in g["memory"]["slots"]]
        # Memory regions: the EXE (from the load area up to the first slot), the slots by name, the heap (when the
        # description has one; else "high" starts where the last slot ends), high.
        last = g["memory"]["slots"][-1]
        if "heap" in g["memory"]:
            heap = addr(g["memory"]["heap"]["start"]), addr(g["memory"]["heap"]["end"])
            tail = [(heap[0], "heap"), (heap[1] + 1, "high")]
        else:
            tail = [(addr(last["base"]) + addr(last["size"]), "high")]
        self.regions = [(ram[0] + 0x10000, "exe")] + slots + tail + [(ram[0] + ram[1], None)]
        self.slot_base = slots[0][0]      # late-bound addresses: anything from the first slot up


CFG = None


def gtemac_include_path(gtemac, include_dirs):
    """The include path of the game's GTE header: relative to the first include directory it lies under, else the
    first game's name, psyq/gtemac.h."""
    if gtemac:
        for d in include_dirs:
            try:
                return gtemac.resolve().relative_to(Path(d).resolve()).as_posix()
            except ValueError:
                continue
    return "psyq/gtemac.h"


def configure(**kwargs):
    global CFG
    CFG = Config(**kwargs)
    return CFG

PROBE_FLAGS = ["-std=gnu99", "-O0", "-fno-builtin", "-fsigned-char", "-fwrapv", "-fno-strict-aliasing", "-DPC_PORT"]
GATE = ["pointer-to-int-cast", "int-to-pointer-cast", "int-conversion", "implicit-function-declaration",
        "incompatible-pointer-types"]

NOT_A_TYPE = {"return", "else", "do", "case", "sizeof", "goto", "if", "while", "for", "switch"}
IDENT = re.compile(r"[A-Za-z_]\w*")
ADDR = re.compile(r"(?<![\w.])0[xX](80[0-9A-Fa-f]{6}|1[fF]80[0-9A-Fa-f]{4})[uUlL]*(?!\w)")
INT_LITERAL = re.compile(r"^\(*\s*(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*\s*\)*$")
ALLCAPS = re.compile(r"^[A-Z][A-Z0-9_]+$")


# ---------------------------------------------------------------------------------------------------------------------
# Source model

def strip_code(t):
    """Comments, string and character literals blanked; offsets and newlines kept."""
    def blank(m):
        s = m.group()
        if s[0] == "/":
            return re.sub(r"[^\n]", " ", s)
        return s[0] + re.sub(r"[^\n]", " ", s[1:-1]) + s[-1]
    return re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', blank, t, flags=re.S)


class Source:
    """One C file or header: stripped text, line table, brace depths, NON_MATCHING branches, #define lines."""

    def __init__(self, path):
        self.path = path
        self.rel = path.relative_to(CFG.root).as_posix()
        self.raw = path.read_text(errors="replace")
        self.text = strip_code(self.raw)
        self.line_starts = [0] + [m.end() for m in re.finditer(r"\n", self.text)]
        self._braces = [m.start() for m in re.finditer(r"[{}]", self.text)]
        depth, self._depths = 0, []
        for p in self._braces:
            depth += 1 if self.text[p] == "{" else -1
            self._depths.append(depth)
        # Per line: 'N' inside `#ifdef NON_MATCHING`, 'M' inside its #else (the matching build's side), '' elsewhere.
        # Per line: (macro name, body start offset) when the line belongs to a #define (continuation lines included).
        self.branch, self.define = [], []
        stack, cont = [], None
        lines = self.text.split("\n")
        for i, line in enumerate(lines):
            s = line.strip()
            if cont is None:
                m = re.match(r"\s*#\s*define\s+(\w+)(\([^)]*\))?", line)
                if m:
                    cont = (m.group(1), self.line_starts[i] + m.end())
            self.define.append(cont)
            if cont is not None and not s.endswith("\\"):
                cont = None
            m = re.match(r"#\s*(ifdef|ifndef|if|else|elif|endif)\b\s*(.*)", s)
            if m:
                kind, rest = m.groups()
                if kind in ("ifdef", "ifndef", "if"):
                    nm = re.search(r"\bNON_MATCHING\b", rest) is not None
                    neg = kind == "ifndef" or (kind == "if" and re.search(r"!\s*defined", rest) is not None)
                    stack.append(("M" if neg else "N") if nm else "")
                elif kind in ("else", "elif") and stack:
                    stack[-1] = {"N": "M", "M": "N", "": ""}[stack[-1]]
                elif kind == "endif" and stack:
                    stack.pop()
            self.branch.append(next((b for b in reversed(stack) if b), ""))

    def line(self, pos):
        return bisect.bisect_right(self.line_starts, pos)

    def depth(self, pos):
        i = bisect.bisect_right(self._braces, pos)
        return self._depths[i - 1] if i else 0

    def prev_token(self, pos, floor=0):
        """The token that ends just before pos: an identifier, or one or two punctuation characters."""
        j = pos
        while j > floor and self.text[j - 1].isspace():
            j -= 1
        if j <= floor:
            return ""
        if self.text[j - 1].isalnum() or self.text[j - 1] == "_":
            k = j
            while k > floor and (self.text[k - 1].isalnum() or self.text[k - 1] == "_"):
                k -= 1
            return self.text[k:j]
        return self.text[max(floor, j - 2):j] if self.text[j - 2:j] == "->" else self.text[j - 1]

    def is_use(self, m, call=True):
        """m: an identifier match. True when it is a use (a call when `call`): not a field, not a prototype or a
        definition, not the name a #define defines."""
        pos = m.start()
        if call and not re.match(r"\s*\(", self.text[m.end():m.end() + 80]):
            return False
        ln = self.line(pos) - 1
        d = self.define[ln]
        floor = 0
        if d is not None:
            if pos < d[1]:
                return False          # the macro's own name or a parameter
            floor = d[1]
        prev = self.prev_token(pos, floor)
        if prev in (".", "->"):
            return False
        if d is None:
            if self.depth(pos) == 0:
                return False          # file scope: a prototype or the definition
            if call and prev and (prev[0].isalpha() or prev[0] == "_") and prev not in NOT_A_TYPE:
                return False          # `type name(` inside a function: a local prototype
        return True

    def close_paren(self, i):
        """self.text[i] == '(': the index of its matching ')'."""
        depth = 0
        for j in range(i, len(self.text)):
            c = self.text[j]
            depth += (c == "(") - (c == ")")
            if depth == 0:
                return j
        return len(self.text) - 1

    def args(self, i):
        """self.text[i] == '(': the call's arguments, split at top-level commas."""
        end = self.close_paren(i)
        out, depth, start = [], 0, i + 1
        for j in range(i + 1, end):
            c = self.text[j]
            if c in "([{":
                depth += 1
            elif c in ")]}":
                depth -= 1
            elif c == "," and depth == 0:
                out.append(self.text[start:j].strip())
                start = j + 1
        out.append(self.text[start:end].strip())
        return out


def c_files():
    return list(CFG.sources())


def headers():
    return list(CFG.headers())


def module_of(rel):
    return CFG.module_of(rel)


# ---------------------------------------------------------------------------------------------------------------------
# Symbol files

def read_symbols():
    """-> (sdk: name -> (library, is_func), syms: name -> (addr, is_func)) from the game's symbol files."""
    sdk, syms = {}, {}
    files = [f for f in CFG.symbol_files if f.exists()]
    for f in files:
        lib = None
        for line in f.read_text(errors="replace").splitlines():
            s = line.strip()
            if s.startswith("//"):
                m = re.match(r"//\s*(LIB\w+)\.LIB/\w+\.OBJ", s)
                if m:
                    lib = m.group(1)
                elif not re.match(r"//\s*\w+\s*=\s*0x", s):     # a commented-out label keeps the section
                    lib = None
                continue
            m = re.match(r"(\w+)\s*=\s*0x([0-9A-Fa-f]+)\s*;\s*(?://(.*))?", s)
            if not m:
                if not s:
                    lib = None
                continue
            name, addr, attrs = m.group(1), int(m.group(2), 16), m.group(3) or ""
            is_func = "type:func" in attrs
            syms.setdefault(name, (addr, is_func))
            if lib:
                sdk.setdefault(name, (lib, is_func))
    if CFG.psyq_libraries is not None:
        # The game's own map (a symbol file without library comments): every name it knows and the files left untagged.
        for name, lib in CFG.psyq_libraries().items():
            if name in syms and name not in sdk:
                sdk[name] = (lib, syms[name][1])
    return sdk, syms


def region(addr):
    if 0x1F800000 <= addr < 0x1F800400:
        return "scratchpad"
    name = None
    for start, n in CFG.regions:
        if addr < start:
            return name
        name = n
    return None


# ---------------------------------------------------------------------------------------------------------------------
# counts

class Site:
    __slots__ = ("kind", "rel", "line", "name", "tags", "branch")

    def __init__(self, kind, src, pos, name, tags=()):
        self.kind, self.rel, self.line, self.name = kind, src.rel, src.line(pos), name
        self.tags = set(tags) | {name}
        self.branch = src.branch[self.line - 1]


def header_macros(path, pattern=r"\w+", function_like=True):
    if not path.exists():
        return []
    text = strip_code(path.read_text(errors="replace"))
    rx = r"^[ \t]*#[ \t]*define[ \t]+(%s)%s" % (pattern, r"\(" if function_like else r"\b")
    return list(dict.fromkeys(re.findall(rx, text, flags=re.M)))


def include_guard(path):
    m = re.search(r"#\s*ifndef\s+(\w+)\s*\n\s*#\s*define\s+\1\b", strip_code(path.read_text(errors="replace")))
    return m.group(1) if m else None


def enclosing_macro(src, pos, floor):
    """The nearest enclosing call `NAME(` around pos (not before floor); returns NAME or None."""
    depth = 0
    j = pos - 1
    while j >= floor:
        c = src.text[j]
        if c == ")":
            depth += 1
        elif c == "(":
            if depth:
                depth -= 1
            else:
                prev = src.prev_token(j, floor)
                if prev and (prev[0].isalpha() or prev[0] == "_") and prev not in NOT_A_TYPE:
                    return prev
        elif c in ";{}" and depth == 0:
            return None
        j -= 1
    return None


def pointer_cast_before(src, pos, floor):
    """True when a parenthesised type containing `*` directly precedes pos (opening parentheses skipped)."""
    j = pos
    while j > floor and (src.text[j - 1].isspace() or src.text[j - 1] == "("):
        j -= 1
    if j <= floor or src.text[j - 1] != ")":
        return False
    depth = 0
    for k in range(j - 1, floor - 1, -1):
        c = src.text[k]
        depth += (c == ")") - (c == "(")
        if depth == 0:
            inner = src.text[k + 1:j - 1]
            return "*" in inner and not re.search(r"[-+/%&|<>=!?]|(?<!\w)\d", inner)
    return False


def scan(sources):
    sdk, syms = read_symbols()
    sdk_funcs = {n for n, (lib, f) in sdk.items() if f}
    psyq = CFG.psyq_dir
    gpu_h = psyq / "libgpu.h"
    gpu_macros = set(header_macros(gpu_h))
    gpu_types = set()
    if gpu_h.exists():
        gpu_types = {n for n in re.findall(r"\}\s*(\w+)\s*;", strip_code(gpu_h.read_text()))
                     if re.match(r"((POLY|LINE|SPRT|TILE|DR)_\w+|SPRT|TILE|BLK_FILL)$", n)}
    gte_macros = set(header_macros(CFG.gtemac or psyq / "gtemac.h", r"gte_\w+"))
    port_h = CFG.port_h or CFG.root / "include" / "port.h"
    guard = include_guard(port_h) if port_h.exists() else None
    port_macros = {n for n in header_macros(port_h, function_like=False) if n != guard}
    sites = []
    totals = Counter()
    for src in sources:
        text = src.text
        in_psyq = psyq in src.path.parents
        base = src.rel.rsplit("/", 1)[-1]
        for m in IDENT.finditer(text):
            name = m.group()
            if name in sdk_funcs and src.is_use(m):
                sites.append(Site("psyq", src, m.start(), name, (sdk[name][0],)))
            elif name in gpu_macros and base != "libgpu.h" and src.is_use(m):
                sites.append(Site("gpu-macro", src, m.start(), name))
            elif name in gpu_types and not in_psyq:
                sites.append(Site("gpu-prim", src, m.start(), name))
            elif name in gte_macros and base != "gtemac.h" and src.is_use(m):
                sites.append(Site("gte", src, m.start(), name))
            elif name in port_macros and base != "port.h" and src.is_use(m, call=False):
                sites.append(Site("port", src, m.start(), name))
            elif name in ("INCLUDE_RODATA", "INCLUDE_ASM") and src.path.suffix == ".c":
                if src.define[src.line(m.start()) - 1] is None:
                    a = src.args(text.index("(", m.end()))
                    sites.append(Site("rodata" if name == "INCLUDE_RODATA" else "asm", src, m.start(), a[-1]))
            elif name in ("object_new", "object_create") and src.is_use(m):
                totals[name] += 1
                a = src.args(text.index("(", m.end()))
                if len(a) > 1 and INT_LITERAL.match(a[1]):
                    sites.append(Site("size", src, m.start(), name, (a[1],)))
            elif name == "heap_funcs":
                m2 = re.match(r"\s*\.\s*(alloc\w*|bzero)\s*\(", text[m.end():m.end() + 60])
                if m2:
                    which = "bzero" if m2.group(1) == "bzero" else "alloc"
                    totals[which] += 1
                    a = src.args(m.end() + m2.end() - 1)
                    size = a[1] if which == "bzero" and len(a) > 1 else a[0]
                    if INT_LITERAL.match(size):
                        sites.append(Site("size", src, m.start(), which, ("heap_funcs." + m2.group(1), size)))
            elif re.match(r"func_[0-9A-Fa-f]{8}$", name) and src.is_use(m):
                addr = int(name[5:], 16)
                if addr >= CFG.slot_base:
                    sites.append(Site("late", src, m.start(), name, (region(addr) or "?",)))
            elif name == "while":
                p = re.match(r"\s*\(", text[m.end():m.end() + 40])
                if not p:
                    continue
                close = src.close_paren(m.end() + p.end() - 1)
                cond = " ".join(text[m.end() + p.end():close].split())
                b = re.match(r"\s*(;|\{)", text[close + 1:close + 200])
                if not b:
                    continue
                if b.group(1) == ";":
                    if src.prev_token(m.start()) == "}":
                        continue       # do { ... } while (x);
                    body = ""
                else:
                    start = close + 1 + b.end()
                    end = text.find("}", start)
                    body = text[start:end].strip()
                    if "{" in body:
                        continue
                hook = re.match(r"([A-Z][A-Z0-9_]+)\s*(\([^;{}]*\))?\s*;?$", body)
                if body == "":
                    sites.append(Site("wait", src, m.start(), "empty", (cond,)))
                elif hook:
                    sites.append(Site("wait", src, m.start(), "hooked", (hook.group(1), cond)))
        for m in ADDR.finditer(text):
            addr = int(m.group(1), 16)
            reg = region(addr)
            if reg is None:
                continue
            ln = src.line(m.start()) - 1
            d = src.define[ln]
            floor = d[1] if d else 0
            macro = enclosing_macro(src, m.start(), floor)
            cast = pointer_cast_before(src, m.start(), floor)
            tags = [reg, "0x%08X" % addr]
            if macro and ALLCAPS.match(macro):
                tags += ["wrapped", macro]
            elif d and ALLCAPS.match(d[0]):
                tags += ["wrapped", "define", d[0]]
            else:
                tags.append("cast" if cast else "bare")
            if cast:
                tags.append("ptrcast")
            sites.append(Site("addr", src, m.start(), "0x%08X" % addr, tags))
    return sites, totals, {"sdk": sdk, "syms": syms, "gpu_macros": gpu_macros, "gpu_types": gpu_types,
                           "gte_macros": gte_macros, "port_macros": port_macros, "port_h": port_h.exists()}


def top(counter, n=None, sep=", "):
    return sep.join(f"{k} {v}" if v > 1 else k for k, v in counter.most_common(n))


def gcc_version(target="host"):
    try:
        return subprocess.run([compiler(target), "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    except (OSError, IndexError):
        return f"{compiler(target)}: not found"


def llvm_mingw_dir():
    """tools/llvm-mingw of the game's tool directories (scripts/setup.sh llvm-mingw), or None."""
    for d in CFG.tool_dirs:
        if (d / "llvm-mingw" / "bin" / "x86_64-w64-mingw32-clang").exists():
            return d / "llvm-mingw"
    return None


def compiler(target):
    """The probe's compiler: the host gcc, or llvm-mingw's clang for the Windows cross build (--target windows)."""
    return "gcc" if target == "host" else str(llvm_mingw_dir() / "bin" / "x86_64-w64-mingw32-clang")


def git_head():
    r = subprocess.run(["git", "-C", str(CFG.root), "rev-parse", "--short", "HEAD"], capture_output=True, text=True)
    return r.stdout.strip() or "?"


def cmd_counts(args):
    cs = [Source(p) for p in c_files()]
    hs = [Source(p) for p in headers()]
    sites, totals, info = scan(cs + hs)
    by = defaultdict(list)
    for s in sites:
        by[s.kind].append(s)

    if args.sites:
        kind, _, tag = args.sites.partition(":")
        if kind not in by and kind not in ("psyq", "gpu-macro", "gpu-prim", "gte", "rodata", "asm", "size", "addr",
                                           "wait", "late", "port"):
            sys.exit(f"unknown kind {kind!r} (see --help)")
        n = 0
        for s in by.get(kind, []):
            if tag and tag not in s.tags:
                continue
            extra = " ".join(sorted(t for t in s.tags if t != s.name))
            nm = {"N": "  [NON_MATCHING]", "M": "  [matching side]", "": ""}[s.branch]
            print(f"{s.rel}:{s.line}: {s.name}  ({extra}){nm}")
            n += 1
        print(f"{n} sites", file=sys.stderr)
        return 0

    print(f"port inventory: counts at {git_head()} (docs/PORT.md \"Compiling the game C for the host\")")
    lines = sum(c.raw.count("\n") for c in cs)
    print(f"\nC files: {len(cs)} ({lines:,} lines); headers: {len(hs)}")

    # Psy-Q calls
    p = by["psyq"]
    funcs = Counter(s.name for s in p)
    nm_only = sum(1 for s in p if s.branch == "N")
    print(f"\nPsy-Q functions called by game C: {len(funcs)} distinct at {len(p)} call sites "
          f"({nm_only} of them inside NON_MATCHING branches)")
    libs = defaultdict(list)
    for s in p:
        libs[next(t for t in s.tags if t.startswith("LIB"))].append(s)
    print(f"  {'library':<9} {'funcs':>5} {'sites':>5}  where (module sites)")
    for lib, ss in sorted(libs.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        mods = Counter(module_of(s.rel) for s in ss)
        print(f"  {lib:<9} {len(set(s.name for s in ss)):>5} {len(ss):>5}  {top(mods)}")
    for lib, ss in sorted(libs.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        print(f"  {lib}: {top(Counter(s.name for s in ss))}")

    g = by["gpu-macro"]
    print(f"\nlibgpu.h macros: {len(g)} uses of {len(set(s.name for s in g))} macros "
          f"(of {len(info['gpu_macros'])} defined): {top(Counter(s.name for s in g))}")
    t = by["gpu-prim"]
    print(f"libgpu.h primitive types: {len(set(s.name for s in t))} used, {len(t)} mentions: "
          f"{top(Counter(s.name for s in t))}")
    e = by["gte"]
    print(f"\ngte_* macros: {len(e)} uses of {len(set(s.name for s in e))} macros "
          f"(of {len(info['gte_macros'])} defined) in {top(Counter(s.rel.rsplit('/', 1)[-1] for s in e))}")
    print(f"  {top(Counter(s.name for s in e))}")

    r, a = by["rodata"], by["asm"]
    print(f"\nINCLUDE_RODATA: {len(r)} ({top(Counter(module_of(s.rel) for s in r))})")
    print(f"INCLUDE_ASM: {len(a)} ({top(Counter(module_of(s.rel) for s in a))})")

    def lit(name):
        ss = [s for s in by["size"] if s.name == name]
        dec = sum(1 for s in ss if not any(t.lower().lstrip("(").startswith("0x") for t in s.tags))
        return f"{len(ss)}" + (f" ({dec} decimal)" if dec else "") + f" of {totals[name]}"

    print(f"\nliteral sizes: object_new {lit('object_new')} calls, object_create {lit('object_create')}, "
          f"heap_funcs.alloc* {lit('alloc')}, heap_funcs.bzero {lit('bzero')}  (total {len(by['size'])})")

    ad = by["addr"]
    ptr = [s for s in ad if "wrapped" in s.tags or "cast" in s.tags]
    region_names = [n for _, n in CFG.regions if n] + ["scratchpad"]
    print(f"\nPS1 addresses in C (constants in RAM or the scratchpad, pointer-cast or inside an ALL-CAPS macro): "
          f"{len(ptr)}")
    print(f"  {'region':<11} {'total':>5} {'wrapped':>7} {'cast only':>9}")
    for reg in region_names:
        ss = [s for s in ptr if reg in s.tags]
        if not ss:
            continue
        w = [s for s in ss if "wrapped" in s.tags]
        wn = Counter(("#define " if "define" in s.tags else "")
                     + next(x for x in s.tags if ALLCAPS.match(x)) for s in w)
        cast = [s for s in ss if "cast" in s.tags]
        note = top(wn)
        if cast:
            note += ("; " if note else "") + "cast in " + top(Counter(module_of(s.rel) for s in cast), 4)
            common = Counter(s.name for s in cast).most_common(1)[0]
            if common[1] > 4:
                note += f" ({common[1]} are {common[0]})"
        print(f"  {reg:<11} {len(ss):>5} {len(w):>7} {len(cast):>9}  {note}")
    bare = [s for s in ad if "bare" in s.tags]
    print(f"  other integer constants in those ranges (no pointer cast, no macro: IDs and bit masks, or addresses "
          f"used as integers; `--sites addr:bare`): {len(bare)} ("
          + ", ".join(f"{r} {n}" for r, n in Counter(next(t for t in s.tags if t in region_names)
                                                     for s in bare).most_common()) + ")")
    lt = by["late"]
    print(f"late-bound functions (unprefixed func_<overlay address> used from C): {len(set(s.name for s in lt))} "
          f"at {len(lt)} sites: {top(Counter(s.name for s in lt))}")

    w = by["wait"]
    hooked = [s for s in w if s.name == "hooked"]
    print(f"\nempty-body loops (busy-waits): {len(w)}, {len(hooked)} of them hold only a macro")
    for s in w:
        cond = next((x for x in s.tags if x not in ("empty", "hooked") and not ALLCAPS.match(x)), "")
        macro = next((x for x in s.tags if ALLCAPS.match(x)), "")
        print(f"  {s.rel}:{s.line}: ({cond}) {macro}")

    if info["port_h"]:
        pc = Counter(s.name for s in by["port"])
        print(f"\nport.h macros ({len(info['port_macros'])} defined): {len(by['port'])} uses")
        for name in sorted(info["port_macros"]):
            files = Counter(s.rel for s in by["port"] if s.name == name)
            print(f"  {name:<24} {pc[name]:>4}  {top(files, 4)}")
    else:
        print("\nport.h: not present (no hook macros to count)")
    return 0


# ---------------------------------------------------------------------------------------------------------------------
# probe

DIAG = re.compile(r"^(?P<file>[^:\s][^:]*):(?P<line>\d+):(?:\d+:)? (?P<sev>fatal error|error|warning): (?P<msg>.*?)"
                  r"(?: \[(?P<flag>-W[^\]]+)\])?$")
NOTE = re.compile(r"^(?P<file>[^:\s][^:]*):(?P<line>\d+):(?:\d+:)? note: in expansion of macro '(?P<macro>\w+)'")
ASM_DIAG = re.compile(r"^(?P<file>[^:\s][^:]*):(?P<line>\d+): Error: (?P<msg>.*)$")


def gcc_major():
    r = subprocess.run(["gcc", "-dumpversion"], capture_output=True, text=True)
    try:
        return int(r.stdout.strip().split(".")[0])
    except ValueError:
        return 0


def write_overrides(out):
    """The override headers: INCLUDE_ASM/INCLUDE_RODATA empty, every gte_* macro a no-op. They carry the real headers'
    include guards, so the real ones are skipped wherever they are included from."""
    inc = out / "include"
    inc.mkdir(parents=True, exist_ok=True)
    gen = CFG.include_asm
    guard = (include_guard(gen) if gen and gen.exists() else None) or "INCLUDE_ASM_H"
    (inc / "include_asm.h").write_text(
        f"/* generated by tools/port_inventory.py */\n#ifndef {guard}\n#define {guard}\n"
        "#define INCLUDE_ASM(FOLDER, NAME)\n#define INCLUDE_RODATA(FOLDER, NAME)\n#endif\n")
    real = CFG.gtemac
    if real and real.exists():
        text = strip_code(real.read_text())
        guard = include_guard(real) or "PSYQ_GTEMAC_H"
        body = "".join(f"#define {n}{a} ((void)0)\n"
                       for n, a in re.findall(r"^[ \t]*#[ \t]*define[ \t]+(gte_\w+)(\([^)]*\))", text, flags=re.M))
        target = inc / CFG.gtemac_include
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(f"/* generated by tools/port_inventory.py */\n#ifndef {guard}\n#define {guard}\n{body}#endif\n")
    # psxstack_game_gen.h: the game's description, which psxstack's hooks.h includes (the game's port.h includes that
    # under PC_PORT): the same header the port's build generates (tools/game_gen.py).
    subprocess.run([sys.executable, str(STACK_ROOT / "tools" / "game_gen.py"), str(CFG.game_json),
                    "--out", str(inc / "psxstack_game_gen.h")], check=True)
    return inc


# Clang makes the old-C constructs GCC 14 does errors, each under its own flag; the ones that are not the gate go back
# to warnings (port/CMakeLists.txt gives the Windows build's units the same list, the gate's flags included).
CLANG_GAME_FLAGS = ["-Wno-error=implicit-int", "-Wno-error=incompatible-function-pointer-types",
                    "-Wno-error=return-type"]


def probe_command(width, inc, warnings, target="host"):
    if target == "host":
        cmd = ["gcc", f"-m{width}"] + PROBE_FLAGS + [f"-Werror={g}" for g in GATE]
        if gcc_major() >= 14:
            cmd.append("-fpermissive")
        diag = ["-fno-diagnostics-show-caret", "-fmax-errors=0"]
    else:
        # Windows x86_64 (cmake/windows-x86_64.cmake's compiler, no -m: the triple sets the width)
        cmd = [compiler(target)] + PROBE_FLAGS + [f"-Werror={g}" for g in GATE] + CLANG_GAME_FLAGS
        diag = ["-fno-caret-diagnostics", "-ferror-limit=0"]
    if warnings:
        cmd += ["-Wall", "-Wstrict-prototypes"]
    cmd += [f"-D{d}" for d in CFG.defines]
    cmd += ["-fdiagnostics-color=never"] + diag + ["-fmessage-length=0", f"-I{inc}"]
    cmd += [f"-I{d}" for d in CFG.include_dirs] + [f"-I{STACK_ROOT / 'include'}", f"-I{STACK_ROOT / 'include/psxstack'}"]
    return cmd


def check_width(width, out, target="host"):
    """None when the compiler can compile at this width (the host gcc; the Windows clang at its own), else its
    message."""
    t = out / f"width{width}{'' if target == 'host' else '-' + target}.c"
    t.write_text("int probe_width(int a) { return a + 1; }\n")
    cmd = [compiler(target)] + ([f"-m{width}"] if target == "host" else [])
    r = subprocess.run(cmd + ["-c", str(t), "-o", str(t.with_suffix(".o"))], capture_output=True, text=True)
    return None if r.returncode == 0 else (r.stderr.strip().splitlines() or [f"{cmd[0]} failed"])[0]


def run_probe(files, width=64, warnings=False, jobs=None, target="host"):
    """Compiles files (repo-relative posix paths). -> {rel: (ok, errors Counter, warnings Counter, messages, obj)}"""
    out = CFG.out / (f"m{width}" if target == "host" else target)
    out.mkdir(parents=True, exist_ok=True)
    inc = write_overrides(CFG.out)
    base = probe_command(width, inc, warnings, target)
    env = dict(os.environ, LC_ALL="C")

    def one(rel):
        obj = out / "obj" / (rel[:-2] + ".o")
        obj.parent.mkdir(parents=True, exist_ok=True)
        if obj.exists():
            obj.unlink()
        r = subprocess.run(base + ["-c", rel, "-o", str(obj)], cwd=CFG.root, capture_output=True, text=True, env=env,
                           errors="replace")
        errs, warns, msgs, pending = Counter(), Counter(), [], None
        for line in r.stderr.replace(str(CFG.root) + "/", "").splitlines():
            m = NOTE.match(line)
            if m and pending is not None and m.group("file") == rel:
                # an error inside a header's macro: the last expansion note in the C file is the game site
                msgs[pending] = msgs[pending].split("  <- ")[0] + f"  <- {rel}:{m.group('line')} ({m.group('macro')})"
                continue
            m = DIAG.match(line)
            if m:
                pending = None
                flag = (m.group("flag") or "").replace("-Werror=", "").replace("-W", "", 1).rstrip("=")
                if m.group("sev") == "warning":
                    warns[(flag or "(no flag)", line.split(": warning:")[0])] += 1
                    if warnings:
                        msgs.append(line)
                else:
                    cat = flag if flag in GATE else ("fatal" if m.group("sev") == "fatal error" else
                                                     "other-error" + (f" ({flag})" if flag else ""))
                    errs[cat] += 1
                    if m.group("file") != rel:
                        errs[(cat, m.group("file"))] += 1      # located in a header (a macro's body)
                        pending = len(msgs)
                    msgs.append(line)
                continue
            m = ASM_DIAG.match(line)
            if m:
                errs["asm"] += 1
                msgs.append(line)
        ok = r.returncode == 0 and obj.exists()
        if not ok and not errs:
            errs["other"] += 1
            msgs += r.stderr.splitlines()[-5:]
        if not ok and obj.exists():
            obj.unlink()
        return rel, (ok, errs, warns, msgs, obj)

    with ThreadPoolExecutor(max_workers=jobs or os.cpu_count() or 1) as ex:
        return dict(ex.map(one, files))


def resolve_files(names):
    if not names:
        return [p.relative_to(CFG.root).as_posix() for p in c_files()]
    out = []
    for n in names:
        p = Path(n)
        p = p if p.is_absolute() else (Path.cwd() / p)
        if p.is_dir():
            out += [q.resolve().relative_to(CFG.root).as_posix() for q in sorted(p.rglob("*.c"))]
        elif p.exists():
            out.append(p.resolve().relative_to(CFG.root).as_posix())
        else:
            sys.exit(f"no such file: {n}")
    return out


def cmd_probe(args):
    width = 32 if args.m32 else 64
    target = args.target
    if target != "host" and args.m32:
        sys.exit("probe: --m32 is the host's; the Windows build is x86_64 only")
    CFG.out.mkdir(parents=True, exist_ok=True)
    why = check_width(width, CFG.out, target)
    if why:
        print(f"probe: {compiler(target)} cannot compile" + (f" at -m{width}" if target == "host" else "") + f": {why}")
        return 2
    files = resolve_files(args.files)
    t0 = time.time()
    res = run_probe(files, width, args.warnings, args.jobs, target)
    what = f"-m{width}" if target == "host" else f"{target} x86_64 (llvm-mingw)"
    print(f"port inventory: {'host' if target == 'host' else target}-compile probe at {git_head()}, {what}, "
          f"{gcc_version(target)}")
    print("flags: " + " ".join(probe_command(width, CFG.out / "include", args.warnings, target)[1:]))
    failing = {f: r for f, r in res.items() if not r[0]}
    total = Counter()

    def describe(errs, order):
        out = []
        for c in order:
            hdr = [f"{n} in {k[1]}" for k, n in sorted(errs.items(), key=str) if isinstance(k, tuple) and k[0] == c]
            out.append(f"{errs[c]} {c}" + (f" ({', '.join(hdr)})" if hdr else ""))
        return ", ".join(out)

    for f in files:
        ok, errs, warns, msgs, _ = res[f]
        total.update(errs)
        if ok and not (args.warnings and args.verbose and msgs):
            continue
        if not ok:
            print(f"FAIL {f}: " + describe(errs, sorted(k for k in errs if isinstance(k, str))))
        if args.verbose:
            for line in msgs:
                print("    " + line)
    cats = sorted((k for k in total if isinstance(k, str)), key=lambda k: -total[k])
    n_err = sum(total[k] for k in cats)
    print(f"\n{len(files) - len(failing)} of {len(files)} files compile; {len(failing)} fail with {n_err} errors"
          + (": " + describe(total, cats) if total else "") + f"  ({time.time() - t0:.1f} s)")
    if args.warnings:
        # A warning inside a header shows in every file that includes it: count each location once.
        seen = {}
        for f in files:
            for (flag, loc), n in res[f][2].items():
                seen.setdefault((flag, loc), n)
        wt, wf, wh = Counter(), defaultdict(set), Counter()
        for (flag, loc), n in seen.items():
            wt[flag] += n
            wf[flag].add(loc.split(":")[0])
            wh[flag] += n if loc.startswith("include/") else 0
        print(f"non-gating warnings (-Wall -Wstrict-prototypes{', -fpermissive' if gcc_major() >= 14 else ''}; "
              f"each source location once): {sum(wt.values())}")
        for k, v in wt.most_common():
            print(f"  {v:>6}  {'-W' if k != '(no flag)' else ''}{k}  ({len(wf[k])} files"
                  + (f"; {wh[k]} in include/" if wh[k] else "") + ")")
    print("probe: " + ("clean" if not failing else "NOT clean"))
    return 0 if not failing else 1


# ---------------------------------------------------------------------------------------------------------------------
# link

def nm_symbols(obj):
    r = subprocess.run(["nm", str(obj)], capture_output=True, text=True)
    defined, undefined = set(), set()
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0] == "U":
            undefined.add(parts[1])
        elif len(parts) == 3 and parts[1].isupper() and parts[1] not in "UWV":
            defined.add(parts[2])
    return defined, undefined


def file_scope_definitions(src):
    """Names a C file defines at file scope (functions with a body, data without `extern`): for the files that are
    missing from the link probe."""
    names = set()
    text = src.text
    start = 0
    for m in re.finditer(r"[;{]", text):
        if src.depth(m.start() - 1) != 0 if m.start() else False:
            continue
        stmt = text[start:m.start()]
        if text[m.start()] == "{":
            end = m.start()
            # skip to the matching close: the next position at depth 0
            i = bisect.bisect_right(src._braces, end)
            while i < len(src._braces) and src._depths[i] != 0:
                i += 1
            start = (src._braces[i] + 1) if i < len(src._braces) else len(text)
        else:
            start = m.end()
        stmt = re.sub(r"^\s*#.*$", "", stmt, flags=re.M)
        if re.search(r"\b(extern|typedef)\b", stmt) or not stmt.strip():
            continue
        if text[m.start()] == "{":
            f = re.search(r"(\w+)\s*\([^{}]*\)\s*$", stmt)
            if f:
                names.add(f.group(1))
                continue
        if "(" in stmt.split("=")[0] and "(*" not in stmt.split("=")[0].replace(" ", ""):
            continue                   # a prototype
        d = re.search(r"(\w+)\s*(\[[^=;]*\])*\s*(=|$)", stmt.split("=")[0].rstrip() + ("=" if "=" in stmt else ""))
        if d:
            names.add(d.group(1))
        fp = re.search(r"\(\s*\*\s*(\w+)\s*(\[[^\]]*\])*\s*\)", stmt.split("=")[0])
        if fp:
            names.add(fp.group(1))
    return names


def cmd_link(args):
    CFG.out.mkdir(parents=True, exist_ok=True)
    why = check_width(64, CFG.out)
    if why:
        print(f"link: the host gcc cannot compile at -m64: {why}")
        return 2
    files = resolve_files([])
    t0 = time.time()
    res = run_probe(files, 64, False, args.jobs)
    missing = [f for f in files if not res[f][0]]
    print(f"port inventory: link probe at {git_head()}, -m64, {gcc_version()}")
    print(f"objects: {len(files) - len(missing)} of {len(files)} ({len(missing)} files fail the probe and are "
          f"missing: run `probe` for why)")
    where = defaultdict(list)
    undefined = defaultdict(list)
    with ThreadPoolExecutor(max_workers=args.jobs or os.cpu_count() or 1) as ex:
        ok = [f for f in files if res[f][0]]
        for f, (d, u) in zip(ok, ex.map(lambda f: nm_symbols(res[f][4]), ok)):
            for n in d:
                where[n].append(f)
            for n in u:
                undefined[n].append(f)
    dups = {n: fs for n, fs in where.items() if len(fs) > 1}
    print(f"\nglobals defined: {len(where)}; defined twice: {len(dups)}")
    for n, fs in sorted(dups.items()):
        print(f"  {n}: {', '.join(fs)}")

    sdk, syms = read_symbols()
    rodata = {}
    for f in files:
        raw = (CFG.root / f).read_text(errors="replace")
        for m in re.finditer(r"^\s*INCLUDE_RODATA\([^,]+,\s*(\w+)\s*\)", raw, flags=re.M):
            rodata[m.group(1)] = f
    in_missing = {}
    for f in missing:
        for n in file_scope_definitions(Source(CFG.root / f)):
            in_missing.setdefault(n, f)
    cats = defaultdict(list)
    for n in sorted(set(undefined) - set(where)):
        m = re.match(r"func_([0-9A-Fa-f]{8})$", n)
        if n in sdk:
            cats["Psy-Q function" if sdk[n][1] else "Psy-Q data"].append(n)
        elif m and int(m.group(1), 16) >= CFG.slot_base:
            cats["late-bound overlay address"].append(n)
        elif n in in_missing:
            cats["defined in a file missing from the probe"].append(n)
        elif n in rodata:
            cats["asm-only data (INCLUDE_RODATA)"].append(n)
        elif re.match(r"(D|jtbl|jpt)_", n) or (n in syms and not syms[n][1]):
            cats["asm-only data (other)"].append(n)
        else:
            cats["other"].append(n)
    total = sum(len(v) for v in cats.values())
    print(f"\nundefined everywhere: {total}")
    order = ["Psy-Q function", "Psy-Q data", "asm-only data (INCLUDE_RODATA)", "asm-only data (other)",
             "late-bound overlay address",
             "defined in a file missing from the probe", "other"]
    for c in order:
        names = cats.get(c, [])
        if not names:
            continue
        print(f"  {c}: {len(names)}")
        if c == "Psy-Q function":
            libs = Counter(sdk[n][0] for n in names)
            print("    " + ", ".join(f"{k} {v}" for k, v in sorted(libs.items(), key=lambda kv: -kv[1])))
        if c == "asm-only data (INCLUDE_RODATA)":
            print("    " + top(Counter(rodata[n].split("/")[1] for n in names)))
        if args.verbose or c not in ("Psy-Q function", "asm-only data (INCLUDE_RODATA)",
                                     "defined in a file missing from the probe"):
            limit = None if args.verbose else 40
            for n in names[:limit]:
                note = ""
                if n in rodata:
                    note = f"  INCLUDE_RODATA in {rodata[n]}"
                elif c == "defined in a file missing from the probe":
                    note = f"  {in_missing[n]}"
                elif n in syms:
                    note = f"  0x{syms[n][0]:08X}"
                users = undefined[n]
                print(f"    {n}{note}  <- {users[0]}" + (f" (+{len(users) - 1})" if len(users) > 1 else ""))
            if limit and len(names) > limit:
                print(f"    ... {len(names) - limit} more (--verbose)")
    print(f"\nlink probe: {'no duplicate globals' if not dups else f'{len(dups)} DUPLICATE GLOBALS'}"
          f"  ({time.time() - t0:.1f} s)")
    return 1 if dups else 0


def cmd_decls(args):
    """The game's Psy-Q declarations against the stack's (tools/psyq_decls.py), with the probe's flags."""
    sys.path.insert(0, str(STACK_ROOT / "tools"))
    import psyq_decls
    if not CFG.psyq_decl_headers:
        print("decls: no game header declares Psy-Q functions (configure(psyq_decl_headers=[...]))")
        return 2
    missing = [str(h) for h in CFG.psyq_decl_headers if not h.exists()]
    if missing:
        sys.exit(f"decls: no such header: {' '.join(missing)}")
    CFG.out.mkdir(parents=True, exist_ok=True)
    inc = write_overrides(CFG.out)
    cflags = [f for f in PROBE_FLAGS if f != "-O0"] + [f"-D{d}" for d in CFG.defines] + [f"-I{inc}"]
    cflags += [f"-I{d}" for d in CFG.include_dirs] + [f"-I{STACK_ROOT / 'include'}", f"-I{STACK_ROOT / 'include/psxstack'}"]
    counts, lines = psyq_decls.run(STACK_ROOT, CFG.out / "decls", inc, CFG.psyq_decl_headers, cflags, "gcc",
                                   args.verbose)
    what = ", ".join(h.relative_to(CFG.root).as_posix() if h.is_relative_to(CFG.root) else str(h)
                     for h in CFG.psyq_decl_headers)
    return psyq_decls.report(counts, lines, f"{what} against psxstack's include/psxstack/psyq/ ({git_head()})")


def build_parser():
    """The parser with the stack's commands; a game adds its own (structs, ...) to the returned subparsers."""
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("counts", help="the inventory's numbers on the current tree")
    p.add_argument("--sites", metavar="KIND[:TAG]", help="list file:line of one kind of site")
    p.set_defaults(func=cmd_counts)
    p = sub.add_parser("probe", help="host-compile gate (exit 0 = every file compiles without a gating error)")
    p.add_argument("files", nargs="*", help="C files or directories (default: every src/**/*.c)")
    p.add_argument("-j", "--jobs", type=int, help="parallel compiles (default: all cores)")
    p.add_argument("--m32", action="store_true", help="compile at -m32 instead of -m64")
    p.add_argument("--target", choices=["host", "windows"], default="host",
                   help="windows: compile with llvm-mingw's clang for Windows x86_64 instead (tools/llvm-mingw)")
    p.add_argument("--warnings", action="store_true", help="add -Wall and count the non-gating warnings by flag")
    p.add_argument("-v", "--verbose", action="store_true", help="print the compiler's messages")
    p.set_defaults(func=cmd_probe)
    p = sub.add_parser("link", help="nm over the probe's objects: duplicate and undefined globals")
    p.add_argument("-j", "--jobs", type=int)
    p.add_argument("-v", "--verbose", action="store_true", help="list every undefined symbol")
    p.set_defaults(func=cmd_link)
    p = sub.add_parser("decls", help="the game's Psy-Q declarations against the stack's: the register ABI must agree")
    p.add_argument("-v", "--verbose", action="store_true", help="list every function, the agreeing ones too")
    p.set_defaults(func=cmd_decls)
    return ap, sub


NO_GCC = {"counts", "object-sizes"}   # commands that run without a compiler


def main(parser=None, argv=None):
    if CFG is None:
        sys.exit("port_inventory: not configured: run the game's tools/port_inventory.py, which calls configure()")
    ap = parser or build_parser()[0]
    args = ap.parse_args(argv)
    if shutil.which("gcc") is None and args.cmd not in NO_GCC:
        sys.exit("gcc not found on PATH")
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
