# Decisions

A few lines each: what and why. Only decisions that change how the stack works.

## The name: psxstack (2026-10-07)
Short, free on GitHub, and a clean prefix everywhere the code said `dw3`: CMake `PSXSTACK_*`, the C++ namespace, the
library target. Nothing with "Psy" in it, since Psy-Q is SN Systems' name.

## One stack repo, consumed as a pinned git submodule (2026-10-07)
A separate repo that releases like a library, not a monorepo of games: the game repos have disc-derived gitignored
data and their own CI, and two of the candidate decomps are other people's projects. A submodule rather than
FetchContent (needs network in CI and worktrees) or subtree (muddles history). A game's `worktree_init.sh` links the
sibling clone so worktrees build offline; CI checks the submodule out at the pin.

## The game description is compiled in, not read at run time (2026-10-07)
`game.json` is read once at configure time and generated into `psxstack_game_gen.h`; the port, the launcher and the
self-test get their constants from it. Single binaries, no file lookup at start, and the self-test fixtures carry no
real hash of their own.

## Psy-Q headers: the game's, for now (2026-10-07)
The shim compiles against the game's recovered `include/psyq/*.h` through the include path the game passes
(`INCLUDE_DIRS`). The stack owns its own Psy-Q declarations only when the second game arrives and two sets have to
agree; doing it first would mean a third copy with no way to test it.

## The hooks in the game's C are the game's (2026-10-07)
`PLATFORM_WAIT`, the pointer macros, `OVERLAY_COPY`, the slot macros and the `#ifdef PC_PORT` blocks are the per-game
cost of a port, about a hundred lines for dw2003. The stack documents the patterns and gates them; it does not try to
hide them. A third-party decomp is forked and carries them as a small rebased patch set.

## N slots, data-driven (2026-10-07)
The overlay manager and the arena take the slot table from the description instead of two hard-wired tiers. dw2003
has two slots plus the stage files, Digimon World 2 has one; the macro `tier` argument becomes the slot's 1-based
index, so the game C does not change.

## Versioning (2026-10-07)
Semver tags; `PSXSTACK_API` counts incompatible contract changes and replaces `PORT_MODS_API` (manifests keep
`requires_port`). The settings file's `schema` stays 1; changing it is a MAJOR.

## The game's hook header keeps the PS1 side; the stack's hooks.h is the host side only (2026-10-07)
Phase 0 had the game's `port.h` include `<psxstack/hooks.h>` for both sides. The matching build must not depend on
the stack at all (a clone without the submodule still rebuilds the executable byte for byte), so the PS1 expansions
stay in the game's header and `<psxstack/hooks.h>` is included only under `#ifdef PC_PORT`. The game's constants and
the description's are tied by `_Static_assert`s in the adapter.

## The launcher's self-test carries its fixtures (2026-10-07)
The four test manifests live as files under `tests/fixtures/mods/` (readable, diffable), and the launcher's CMake
compiles them into the binary as string constants. The self-test keeps needing no file beside the executable, which
is what lets it run inside a release package and under Wine.

## One generator for the game description: tools/game_gen.py (2026-10-08)
Phase 1 had the first game generate the same header with its own names (`PortGameDisc`, `PSXSTACK_GAME_LAUNCHER_ABOUT`).
One header, one generator, the stack's names: `PsxstackGameDisc`, `PSXSTACK_GAME_ABOUT`; the arena's derived numbers
(`PORT_SLOT<n>_OFS`, `port_slot<n>`, `PORT_HEAP_OFS`, `PORT_ARENA_SIZE`) and the id's token form
(`PSXSTACK_GAME_ID_IDENT`, for pasted symbol names) come from it too, so nothing in the runtime computes a game fact.

## Sections named by the game's id on ELF, fixed on PE (2026-10-08)
The overlay sections are `<id>_data_<ovl>`/`<id>_bss_<ovl>` on ELF (the bracket symbols `__start_<id>_...`). On PE
they are the chunk groups of two fixed output sections, `.psxdata`/`.psxbss`: a PE image section name is eight
characters, which an id-prefixed name would not fit; the symbols still carry the id.

## Environment variables derive from the game's prefix (2026-10-08)
`<PREFIX>_PORT_TRACE`, `_FAST_FORWARD`, `_CHECKPOINT_DIR`, `_RESET_CHECK`, `_CRASH_AT`, `_PRESENT_READBACK`,
`_GPU_VRAM_CHECK`, `_PRIM_DUMP`, `_MCP_GAME`: `PSXSTACK_GAME_ENV_PREFIX` pasted at compile time, so the first game's
names (`DW3_PORT_TRACE`) and tests stay as they were, and two games on one machine never share a variable.

## The build's inputs are files the game writes (2026-10-08)
`tools/port_gen.py` reads no game C: the units, the overlays (slot, file ID, symbol file), the tag sites whose overlay
the game knows and the volatile ranges come as tab-separated files from the game's own tool
(dw2003recomp: `tools/port_inputs.py`), named in `psxstack_add_game()`. The stack checks what it can on its own
(`SLOT_FUNC`/`LATE_FUNC` sites against every table of their tier); a game adds precision, never knowledge the stack
would have to carry.

## The MCP server and the inventory are configured, not forked (2026-10-08)
`tools/mcp/server.py` takes the game on its command line (the game's `.mcp.json`: root, description, binaries,
symbol files, disc) and its self-test runs on fixture symbol files; `tools/port_inventory.py` is a module a game's
wrapper configures (`configure()`: sources, headers, include directories, symbol files, regions from the
description) and extends with its own commands (the first game: `structs`, `object-sizes`).

## examples/hello is the stack's smoke test (2026-10-08)
A disc-free Psy-Q program (two polygons, a pad read, the sound library on) built through `psxstack_add_game()` with
the first game's recovered Psy-Q headers copied beside it (MIT; `docs/THIRD_PARTY.md`), run headless with a
screenshot whose hash CI checks. It proves the generators, the sections, the shim and the runtime without any game,
so the stack's CI needs no disc and no private data. `discs` may be empty for it (the port then takes no `--disc`).
