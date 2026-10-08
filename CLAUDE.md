# psxstack

**Goal:** a reusable stack for PC ports of decompiled PlayStation games. A game repository (a matching decompilation
whose C also compiles for a 64-bit host) adds a small adapter and gets a Linux and Windows port, a launcher and a
release. The stack contains no game code and no game data; `docs/GAME_CONTRACT.md` is the line between the two.
The first consumer is [dw2003recomp](https://github.com/gascarcella/dw2003recomp) (the `psxstack` submodule there).

## Session protocol
1. **Start:** read `README.md` (what is here) and `docs/GAME_CONTRACT.md` (what a game provides, what the stack
   provides). Problems and plans are GitHub issues (`gh issue list`).
2. **Tools:** `scripts/setup.sh` installs everything into `tools/<name>` (gitignored, no sudo, idempotent). With the
   first game's checkout beside this repo, `scripts/dev_link_tools.sh ../dw2003recomp/tools` links its built tools
   instead of building them again. In a git worktree, `scripts/setup.sh link` links the main checkout's.
3. **Work in phases;** stop at the end of each to summarize and wait for the go-ahead. Branch, push, open a pull
   request to `main` (`gh pr create`); CI must be green; the owner reviews and merges.
4. **End:** update the docs that describe how things are (`docs/`, `README.md`) when the state changed; record a
   decision in `docs/DECISIONS.md` only when it changes how the stack works (a few lines: what and why); leftover
   work becomes issues, not docs; commit with a clear message. The pull request says what was done.

## Rules
- **No game code, no game data, no Psy-Q SDK.** Nothing from a game's `src/`, its disc or its extracted files comes
  here; the Psy-Q declarations the shim compiles against are the stack's own, `include/psxstack/psyq/` (written from
  the games' use of the API, Sony's names, no SDK file: DECISIONS "Psy-Q declarations: the stack's"), and a game's
  own declarations are checked against them (`tools/psyq_decls.py`). Record anything borrowed in `docs/THIRD_PARTY.md`.
- **The contract is the source of truth.** A change to what a game must supply or to a hook macro's meaning is a
  change to `docs/GAME_CONTRACT.md` first, with `PSXSTACK_API` (`include/psxstack/game.h`) bumped when it is
  incompatible. Releases are semver tags; a game pins a tag, so a change here reaches it as a reviewable pin bump.
- **No fact about a game in the stack:** no address, file ID, overlay name, disc hash, serial, title or directory
  name. They come from `game.json` (generated into `psxstack_game_gen.h` at configure time by `tools/game_gen.py`;
  nothing reads the file at run time) or from the game's adapter behind `include/psxstack/game.h`.
- **Test against a consumer when touching `runtime/`, `psyq/`, `cmake/` or the contract.** `examples/hello` is the
  disc-free smoke test and CI runs it, but it exercises little. The real gate is the first game: in a dw2003recomp
  checkout, `cmake -S port -B build/port -G Ninja -DPSXSTACK_DIR=/path/to/this/checkout` and its `scripts/test.sh`
  (its README says how; it needs the owner's disc). Say in the pull request which gates ran.
- Don't claim something works until it has been run. When unsure, say so and test.
- Everything reproducible: setup steps go in `scripts/` or `docs/`, never only in shell history. A new pinned tool is
  a `scripts/setup.sh` step into `tools/<dir>` plus a `.gitignore` line without a trailing slash, and a line in
  `docs/THIRD_PARTY.md`.
- Ask before anything that needs sudo or installs system-wide.

## Layout
| Path | Contents |
|---|---|
| `runtime/` | The host runtime (C): arena, overlay manager, pump, video, the hardware renderer, SPU, audio, input, memory cards, settings, the mods engine, json, sha1, crash report, debug channel, frame log, platform, reset, save states, `game_defaults.c` (the adapter's weak defaults) |
| `include/psxstack/` | `hooks.h` (the hook macros' host side and the `port_*` the game C calls), `game.h` (the adapter interface, `PSXSTACK_API`), `mods.h`, `types.h`, `psyq/` (the Psy-Q declarations the shim implements: the stack's own, DECISIONS "Psy-Q declarations: the stack's"), the runtime's headers |
| `psyq/` | The Psy-Q shim, one file per library, plus the hardware models `gpu.c`, `gte.c`, `mdec.c`, `xa.c`; `check.sh` |
| `shaders/` | The hardware renderer's HLSL (DXC to SPIR-V, for Windows also DXIL, at build time) |
| `mods/fast_forward/` | The one mod every game has; a game's own mods live in the game |
| `launcher/` | The launcher (C++, SDL3 + Dear ImGui): its own CMake project; every string from the game description (`launcher/src/brand.h`) |
| `cmake/` | `psxstack.cmake` (`psxstack_add_game()`, documented inline), `version.cmake`, `embed.cmake`, `windows-x86_64.cmake` (llvm-mingw) |
| `windows/` | The game's Windows resource and manifest templates |
| `tools/` | `game_gen.py` (`game.json` → header), `port_gen.py` (the build's generators), `port_inventory.py` (the host-compile gate a game configures), `replay/` (the emulator and port replay runners a game configures: `emulator.py`, `run.lua`, `boot_check.lua`, `port_test.py`, `redux.sh`; docs/RUNTIME.md "The replay runners"), `mcp/` (the debug channel's MCP server, client, symbols; `tools/mcp/README.md`); built tools under `tools/<name>` (gitignored) |
| `scripts/` | `setup.sh`, `dev_link_tools.sh` |
| `schema/` | `game.schema.json`, the game description |
| `examples/` | `dw2003.game.json` (the first game's description, a copy; the game's own is the source), `hello/` (a disc-free Psy-Q program built through the stack) |
| `tests/` | `hello_test.py`, `game_gen_test.py`, `fixtures/` (the launcher self-test's mod manifests) |
| `docs/` | See below |
| `.github/workflows/ci.yml` | Jobs: `game-gen`, `hello`, `mcp`, `launcher` (build + self-test), `windows` (cross-build + self-test under Wine); no disc, no game |

## Commands
```sh
scripts/setup.sh [sdl3 imgui llvm-mingw sdl3-windows dxc sdl3-desktop cmake link]   # project-local tools (default: sdl3 imgui link); --pins = the cache key
scripts/dev_link_tools.sh ../dw2003recomp/tools   # reuse another checkout's built tools
python3 tests/game_gen_test.py                    # the description generator; python3 tools/game_gen.py FILE --check validates one
python3 tests/hello_test.py [--record]            # the smoke test: examples/hello built through psxstack_add_game(), run headless, its picture hashed
python3 tests/hello_test.py --exe build/hello-win/hello.exe --wine --gpu   # its Windows build (-DPSXSTACK_SDL=ON, cross-built) under Wine; --gpu: the hardware renderer's picture too (D3D12)
cmake -S launcher -B build/launcher -G Ninja [-DPSXSTACK_GAME_JSON=path/to/game.json] && cmake --build build/launcher
SDL_VIDEO_DRIVER=offscreen build/launcher/<id>-launcher --self-test build/selftest   # <id> from game.json (dw2003 by default)
cmake -S launcher -B build/launcher-win -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/windows-x86_64.cmake" -DCMAKE_BUILD_TYPE=Release   # the toolchain path must be absolute
python3 tools/mcp/selftest.py                     # the MCP server offline, against fake_game.py
psyq/check.sh --compile [-I DIR]...               # the shim alone with -Werror, against the stack's declarations (no game); --game-root DIR: its coverage of a game's Psy-Q needs
python3 tools/psyq_decls.py --gen-include DIR --out DIR --game-header H... [-I DIR]... [-D X]...   # a game's Psy-Q declarations vs the stack's (a game runs it as its inventory's `decls`)
python3 tests/replay_test.py                      # the replay runners' driver and record logic against a fake emulator (no game)
python3 -m py_compile tools/*.py tools/mcp/*.py tools/replay/*.py tests/*.py
```
The game-side commands (the port's build, the probe, the replays, the MCP server with a real game) are in the game's
own `CLAUDE.md`; dw2003recomp's `port/CMakeLists.txt` is the reference consumer.

## Conventions
- Names: the runtime's API the game C calls is `port_*` (`hooks.h`); the adapter functions a game implements are
  `game_*` (`game.h`); CMake is `psxstack_*` functions and `PSXSTACK_*` options; the generated description macros are
  `PSXSTACK_GAME_*` and `PORT_SLOT<n>_*`; environment variables derive from the game's `env_prefix`
  (`<PREFIX>_PORT_*`, `<PREFIX>_CONFIG_DIR`). The hook macro names (`PLATFORM_WAIT`, `OVERLAY_COPY`, `SLOT_FUNC`, …)
  are stable API: games have hundreds of sites.
- Psy-Q names stay Sony's; the shim never decompiles Sony's code, it reimplements behavior over the hardware models.
- Docs cite each other as `docs/X.md "Heading"`; keep headings stable. Docs are lean: how things are and why, no
  logs; plans and problems are issues.
- Commit messages say what and why in plain words; a pull request's description lists the gates that ran.

## Docs
`docs/GAME_CONTRACT.md` what a game provides and what the stack provides · `docs/PORT.md` the runtime's design (hook
macros, arena, overlays, pump, shim, rendering, sound, crash report) · `docs/RUNTIME.md` building, running, options and
formats · `docs/LAUNCHER.md` the launcher/game contract, the settings file, mod manifests · `docs/DECISIONS.md` the
decisions · `docs/THIRD_PARTY.md` borrowings and pinned tools · `tools/mcp/README.md` the MCP server ·
`launcher/README.md` building the launcher and its self-test
