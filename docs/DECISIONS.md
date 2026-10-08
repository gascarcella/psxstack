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
