# The game contract

What a game repository provides to psxstack and what psxstack provides to it. The game is a matching decompilation
whose C also compiles for a 64-bit host; psxstack turns that C into a PC port with a launcher. Code cites this file as
`GAME_CONTRACT.md "<heading>"`; keep the headings stable.

Terms: **the stack** is this repository, pinned by the game as a git submodule at a tag. **The game** is the decomp
repo. **The adapter** is the game's small port-specific part: its description (`game.json`), its hook header, its
adapter units and its mods. **A unit** is one C file of the game. **An overlay** is a code file the PS1 loads into a
fixed address range (**a slot**); **a tier** names a slot (tier 1 = slot 1, ...).

## Layering

```
 game C units  ──includes──▶  game/include/port.h  ──includes──▶  <psxstack/hooks.h>      (the macros, both sides)
      │                                                                   │
      └──calls port_* ────────────────────────────────────────────▶  runtime/ (the stack)
                                                                          │ calls
 game adapter units (port/game/*.c)  ◀─── implements <psxstack/game.h> ───┘
 game adapter also provides: game.json, mods, symbols files, the unit list
```

Rules:
1. **The stack never includes a game header** except through the two include paths the game passes it (below). It
   uses `<stdint.h>` types in its own headers. The macros in `hooks.h` expand inside game code and may use the game's
   `s32`/`u32`/`u8` names there.
2. **The game's C never includes a stack header directly.** Its `include/port.h` (whatever it is called) includes
   `<psxstack/hooks.h>` and adds the game-specific hooks (its own slot-function typedef macros, its mod hooks).
3. **The runtime reaches the game only through `<psxstack/game.h>`**, the adapter interface the game implements, and
   through `game_main`. Anything the runtime needs from the game's objects (a gamestate field, a map ID, the player's
   position) is a function in that interface, implemented in the adapter, never a direct reference from `runtime/`.
4. **No fact about a game lives in the stack**: no address, file ID, overlay name, disc hash, serial, title or
   directory name. They come from `game.json` (generated into a header at configure time) or from the adapter.
5. **The PS1 matching build sees none of this.** Every macro in `hooks.h` expands to the original code without
   `PC_PORT`; the game's byte-identical build does not depend on the stack at all (the submodule may be absent).

## What the game provides

### 1. `game.json`, the game description
`schema/game.schema.json`; `examples/dw2003.game.json`. Static facts about the game, read once at configure time by
`psxstack_add_game()` and generated into `psxstack_game_gen.h` (C and C++): nothing reads the file at run time, so
the port and the launcher stay single binaries. Contents:
- **Identity:** `id` (the exe's name, the settings and cache directory names, the section prefix), `title`,
  `env_prefix` (`<PREFIX>_CONFIG_DIR`, `<PREFIX>_GAME`, …), `launcher.about`, `launcher.disc_hint`.
- **Discs:** the accepted images: label, serial, SHA-1 of the whole BIN, BIN size, region. Both the port's check and
  the launcher's check use this list; the error text names the label.
- **Timing:** `video.rate` (the nominal vsyncs per second: 50 PAL, 60 NTSC), the rates the launcher offers and the
  text that explains the alternative.
- **Memory:** `memory.ram` (base, size), `memory.slots` (name, base, size, in address order, contiguous), `memory.heap`
  (PS1 start and end, the host heap size), `memory.bios_standin` (addresses the game reads from the BIOS ROM and what
  the stand-in holds).
- **Sections:** nothing. The section prefix is `id` (`dw2003_data_<ovl>`); the overlay names come from the unit list.

### 2. The hook header
The game's `include/port.h` (included by its `common.h`, as today) becomes:
```c
#include <psxstack/hooks.h>   /* PLATFORM_WAIT, OVERLAY_COPY, SLOT_FUNC, OVERLAY_FN, LATE_FUNC/LATE_CALL, SLOT_PTR,
                                 HEAP_*, PTR_*, BIOS_PTR, PORT_SCRATCHPAD_STACK_*; the port_* they call */
#define WSTAG_ENTRY(addr) SLOT_FUNC(void *(*)(), addr)          /* the game's own typed slot-function macros */
#define OVERLAY_ENTRY(addr) SLOT_FUNC(s32 (*)(void), addr)
#ifdef PC_PORT
extern int port_mod_skip_dialogues;  /* the game's mod hooks, read only inside #ifdef PC_PORT blocks */
...
#endif
```
The macro names and meanings of `hooks.h` are stable API (table in `docs/PORT.md "Hook macros"` once it moves here).
`hooks.h` gets the slot constants from `psxstack_game_gen.h`: `PORT_SLOT<n>_BASE`, `PORT_SLOT<n>_SIZE`, `port_slot<n>`
for every slot in `memory.slots`, `PORT_HEAP_START_ADDR`, `PORT_HEAP_END_ADDR`. The `tier` argument of `OVERLAY_COPY`,
`OVERLAY_FN`, `LATE_FUNC` and `SLOT_PTR` is the slot's 1-based index.

### 3. The hooks in the game's C
The game's units carry the `PLATFORM_WAIT()` at every busy-wait an interrupt ends, the pointer macros where a pointer
meets an integer, `OVERLAY_COPY` at the overlay loads, the slot macros at every fixed address, and `#ifdef PC_PORT`
blocks for the mod hooks and the 64-bit `sizeof` fixes. This is the per-game cost of a port; the stack documents the
patterns (`docs/PORT.md "Compiling the game C for the host"`) and `tools/port_inventory.py` counts and gates them, but
nothing in the stack removes the need. A fork of a third-party decomp carries them as a small rebased patch set.

### 4. The adapter units (`<psxstack/game.h>`)
C the game compiles into the port alongside the runtime. It implements:
- `int game_main(void);` the game's `main`, renamed by the build (`-Dmain=game_main` on that one unit).
- `void game_apply_rate(long rate);` called after the settings are read and before `game_main`: the game's own
  response to a non-default rate (dw2003: `records_60hz = 1` for 60). May be empty.
- **State probes** (the replay scripts, the debug channel, the checkpoint hash): `game_state_stage()`,
  `game_state_file()`, `game_state_map()`, `game_state_random_index()`, `game_state_player_pos(s32 *x, s32 *y)`,
  `game_state_image(u8 *out)` with `GAME_STATE_IMAGE_SIZE` (the PS1 bytes a checkpoint hashes, pointers as PS1
  addresses), `game_state_volatile[]` (the ranges the stable hash zeroes), `game_state_host(addr, size)` for the
  game's own fixed-address objects outside the arena (memcard state, an overlay's bss object). The stack maps the
  arena, the EXE's symbols and the overlays' sections itself from the generated tables.
- **The mods:** `const PortMod *const game_mods[]` and `game_mod_count`: each mod's id, version, options and
  `start`/`frame` callbacks, in the stack's `PortMod` shape. Their logic and hooks are the game's; the option types,
  the manifest reader, the hotkeys, `--print-mods` and the settings section are the stack's. The manifests
  (`mods/<id>/mod.json`) sit in the game repo and the build copies them next to the binary.
- **Weak stand-ins** for data the game's residual asm owned (`asmdata.c`), placed in the right overlay section.
Every function has a weak default in the stack that returns 0 or does nothing, so a new game starts with an empty
adapter and adds probes as its tests need them.

### 5. The build inputs
Passed to `psxstack_add_game()` (`cmake/psxstack.cmake`, phase 2):
- `GAME_JSON`: the description.
- `UNITS`: the game's C units, each tagged with its overlay (`<path>;<overlay or MAIN>`), from the game's own
  generator (dw2003: the tier list, `config/wstag_c.txt` and the stage tables, which stay in the game's tooling).
- `MAIN_UNIT`: the unit whose `main` becomes `game_main`.
- `SYMBOLS`: the `type:func` symbol files per overlay (`config/<ovl>.symbols.txt`, …) and the EXE's
  (`config/symbol_addrs.txt`), for the overlay address tables and the state tables.
- `OVERLAY_FILES`: overlay name → file ID (the game's cdload IDs), one line each; the game generates it.
- `INCLUDE_DIRS`: the game's `include/` and root (its `common.h`, its recovered `psyq/*.h`: DECISIONS "Psy-Q headers").
- `ADAPTER`: the adapter units; `MODS_DIR`: the manifests.
- `DEFINES`, `COMMON_FLAGS`, per-unit overrides as today.
Everything comes from tracked files: a port configures from a fresh clone with no disc.

### 6. Tests
The game owns its oracles: goldens, replays, the emulator harness, the M1 test. The stack provides the formats and the
runners it can without a game: the layer-2 script grammar and the per-frame log and record shapes (`script.c`,
`framelog.c`), the debug-channel gate, the renderer comparison, the settings round trip, the launcher self-test. A
game's test that needs the stack's internals talks to the debug channel, not to the C.

## What the stack provides

- **The runtime:** the arena at the PS1's distances from `memory.slots`, the N-slot overlay manager with data reset
  and address tables, the interrupt pump and timing at `video.rate`, the reset, the disc source with the SHA-1 check
  over `discs[]`, the memory cards, settings (schema 1, `--config`), the per-frame log and record, the input script,
  the window and input over SDL3, the software GPU, the hardware renderer, the SPU and audio, the crash report, the
  debug channel.
- **The Psy-Q shim:** the libraries' behavior over the hardware models. It implements what its games call; a new
  game's missing function stops with `port_unimplemented(name)` and is added to the shim (the shim grows per game and
  stays generic).
- **The launcher:** settings directory and file, disc check, video/audio/memcard/input/mods screens, crash reports,
  the self-test; every string, name and hash from `psxstack_game_gen.h`.
- **The build:** `psxstack_add_game()`, the section renaming without a linker script, the post-link checks, the
  Windows cross-build, the host-compile gate (`port_inventory.py probe/link/structs`), the version header.
- **Packaging:** the AppImage and the Windows zip with PDBs, driven by `id` and `title`.
- **Tools:** the MCP server over the debug channel, the symbols from the game's symbol files.

## Compatibility and versions
- The stack is tagged semver `vMAJOR.MINOR.PATCH`. The game pins a tag. `PSXSTACK_API` (an integer in
  `<psxstack/game.h>`) counts incompatible changes to this contract; it replaces `PORT_MODS_API`, and a manifest's
  `requires_port` is checked against it.
- The settings file keeps `schema` 1; a change there is a stack MAJOR.
- A game's adapter compiles against exactly one stack version; the game's CI builds the pin, nothing else.
