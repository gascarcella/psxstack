# Contributing to psxstack

psxstack is a reusable stack for PC ports of decompiled PlayStation games. A change here reaches every game that
builds on it, so the rules below keep the stack generic, legal and reviewable. Read `README.md` first, then
`docs/GAME_CONTRACT.md`: what a game provides and what the stack provides.

## Ground rules

### No game code, no game data, no Psy-Q SDK
Nothing from a game's `src/`, its disc or its extracted files comes here. The Psy-Q declarations the shim compiles
against are the stack's own (`include/psxstack/psyq/`), written from the games' use of the API under Sony's names,
with no SDK file (docs/DECISIONS.md "Psy-Q declarations: the stack's"). The shim never decompiles Sony's code; it
reimplements the libraries' behavior over the hardware models. Anything borrowed is recorded in
`docs/THIRD_PARTY.md`.

### No fact about a game in the stack
No address, file ID, overlay name, disc hash, serial, title or directory name. They come from the game's `game.json`
(generated into `psxstack_game_gen.h` at configure time by `tools/game_gen.py`) or from the game's adapter behind
`include/psxstack/game.h`.

### The contract first
A change to what a game must supply, or to a hook macro's meaning, is a change to `docs/GAME_CONTRACT.md` first. An
incompatible one bumps `PSXSTACK_API` (`include/psxstack/game.h`). The hook macro names (`PLATFORM_WAIT`,
`OVERLAY_COPY`, `SLOT_FUNC`, …) are stable API: games have hundreds of sites.

### Reproducible
Setup steps go in `scripts/` or `docs/`, never only in shell history. A new pinned tool is a `scripts/setup.sh` step
into `tools/<dir>`, a `.gitignore` line (without a trailing slash) and a line in `docs/THIRD_PARTY.md`. Nothing
installs system-wide or needs sudo.

## Getting set up
```sh
git clone https://github.com/gascarcella/psxstack && cd psxstack
scripts/setup.sh                                  # SDL3 and Dear ImGui into tools/, no sudo
python3 tests/hello_test.py                       # the smoke test
```
`README.md` "Setup" lists the other tools (DXC for the hardware renderer, llvm-mingw for Windows) and "Tests" every
test the stack runs on its own.

## Making a change

### Branch and pull request
1. Open or pick an [issue](https://github.com/gascarcella/psxstack/issues): problems and plans live there.
2. Branch from `main`; a large change goes in phases, each one reviewable on its own.
3. Open a pull request to `main`. Its description says what was done and lists the gates that ran.
4. CI must be green (`.github/workflows/ci.yml`; a change to Markdown or `LICENSE` alone runs nothing). A code owner
   (`.github/CODEOWNERS`) reviews and merges.

### Gates
- **Always:** the tests in `README.md` "Tests" that touch what changed; `python3 -m py_compile tools/*.py
  tools/mcp/*.py tools/replay/*.py tests/*.py` for Python.
- **`runtime/`, `psyq/`, `cmake/` or the contract:** also a real game. `examples/hello` exercises little; the gate
  is the first game: in a [dw2003recomp](https://github.com/gascarcella/dw2003recomp) checkout,
  `cmake -S port -B build/port -G Ninja -DPSXSTACK_DIR=/path/to/psxstack` and its `scripts/test.sh` (it needs the
  game's disc; its README says how). A contributor without one says so in the pull request, and the owner runs it.
- **The launcher:** its self-test (`launcher/README.md`).
- **Windows:** the cross-build and the tests under Wine when the change touches platform code
  (`runtime/platform.c`, `cmake/windows-x86_64.cmake`, `windows/`).

Don't claim something works until it has run; when unsure, say so in the pull request.

### Docs
- Update the docs that describe how things are (`docs/`, `README.md`) when the state changes.
- Record a decision in `docs/DECISIONS.md` only when it changes how the stack works: a few lines, what and why.
- Docs cite each other as `docs/X.md "Heading"`, so keep headings stable. They are lean: how things are and why, no
  logs. Leftover work becomes an issue, not a doc.

### Code style
Write code that reads like the code around it. The naming:
- the runtime's API the game's C calls is `port_*` (`include/psxstack/hooks.h`);
- the adapter functions a game implements are `game_*` (`include/psxstack/game.h`);
- CMake is `psxstack_*` functions and `PSXSTACK_*` options;
- the generated description macros are `PSXSTACK_GAME_*` and `PORT_SLOT<n>_*`;
- environment variables derive from the game's `env_prefix` (`<PREFIX>_PORT_*`, `<PREFIX>_CONFIG_DIR`);
- Psy-Q names stay Sony's.

### Commits
Commit messages say what and why in plain words. One topic per commit.

## Releases
Releases are semver tags (`vMAJOR.MINOR.PATCH`), cut by the owner. A game pins a tag, so a change here reaches it as
a reviewable pin bump (docs/GAME_CONTRACT.md "Compatibility and versions").

## License
By contributing you agree that your contribution is licensed under the MIT license (`LICENSE`).
