# psxstack

A reusable stack for PC ports of decompiled PlayStation games: the host runtime (memory arena, overlay manager,
interrupt pump, software and hardware rendering, SPU, input, memory cards, settings, crash reports, debug channel),
a Psy-Q shim, a launcher (SDL3, Dear ImGui) with mod manifests, and the build and packaging scripts. A game repo
(a matching decompilation) adds a small adapter and gets a Linux and Windows port, a launcher and a release.

It grew out of the [Digimon World 2003 decompilation](https://github.com/gascarcella/dw2003recomp)'s `port/` and
`launcher/`, which it replaces once version 0.1 is tagged.

**Status: phase 3 landed; phases 1 and 2 are in progress in dw2003recomp.** The contract (`docs/GAME_CONTRACT.md`), the
game description (`schema/game.schema.json`, `tools/game_gen.py`) and the launcher (`launcher/`, branded from the
description; `docs/LAUNCHER.md`) are here. The runtime and the Psy-Q shim arrive with phase 2; `runtime/` holds only the
two files the launcher shares with them (`json.c`, `sha1.c`).

```sh
scripts/setup.sh sdl3 imgui              # or scripts/dev_link_tools.sh ../dw2003recomp/tools to reuse built tools
cmake -S launcher -B build/launcher -G Ninja [-DPSXSTACK_GAME_JSON=path/to/game.json] && cmake --build build/launcher
SDL_VIDEO_DRIVER=offscreen build/launcher/dw2003-launcher --self-test build/selftest
python3 tests/game_gen_test.py           # the description generator's test
```

## Planned layout

| Path | Contents |
|---|---|
| `runtime/` | The host runtime (C): arena, overlay manager, pump, video, the hardware renderer, SPU, audio, input, memory cards, settings, mods engine, json, sha1, crash report, debug channel, frame log, platform, reset |
| `include/psxstack/` | The hook macros' host side (`hooks.h`), the adapter interface (`game.h`, `mods.h`), the runtime's headers |
| `psyq/` | The Psy-Q shim: one file per library, plus the hardware models `gpu.c`, `gte.c`, `mdec.c`, `xa.c`; `check.sh` |
| `shaders/` | The hardware renderer's HLSL (DXC to SPIR-V at build time) |
| `mods/fast_forward/` | The one mod every game has |
| `launcher/` | The launcher (C++): settings, disc check, mods, input bindings, self-test; branding from the game description |
| `cmake/` | `psxstack.cmake` (`psxstack_add_game()`), `version.cmake`, `embed.cmake`, `windows-x86_64.cmake` (the llvm-mingw toolchain) |
| `windows/` | The game's Windows resource and manifest templates |
| `tools/` | `game_gen.py` (the description to a header), `port_gen.py` (the build's generators), `port_inventory.py` (the host-compile gate, configured by a game), `mcp/` (the debug channel's MCP server, client and symbols) |
| `scripts/` | `setup.sh` (SDL3, ImGui, DXC, llvm-mingw, SDL3 for Windows, cmake), `dev_link_tools.sh` |
| `schema/` | `game.schema.json`: the game description |
| `examples/` | `dw2003.game.json` (the first game's description); `hello/`, a disc-free Psy-Q program built through the stack: its smoke test |
| `docs/` | `GAME_CONTRACT.md`, `PORT.md`, `RUNTIME.md`, `LAUNCHER.md`, `DECISIONS.md`, `THIRD_PARTY.md` |
| `tests/` | The stack's own tests: `hello_test.py`, `game_gen_test.py`, the launcher's fixtures (`launcher --self-test`, `tools/mcp/selftest.py`) |

## Using it from a game
```cmake
include(psxstack/cmake/psxstack.cmake)          # the submodule (or a sibling clone linked at psxstack/)
psxstack_add_game(mygame GAME_JSON port/game/game.json UNITS build/gen/units.txt MAIN_UNIT src/main.c
                  OVERLAYS build/gen/overlays.txt INCLUDE_DIRS include . ADAPTER port/game/state.c ...)
```
`cmake/psxstack.cmake` documents every argument; `docs/GAME_CONTRACT.md` what the game provides; `examples/hello` is
the smallest consumer, dw2003recomp's `port/CMakeLists.txt` the first real one. The launcher is its own project:
`cmake -S psxstack/launcher -B build/launcher -DPSXSTACK_GAME_JSON=... -DPSXSTACK_VERSION_ROOT=...`.

## Phases

0. **Contract** (done): `GAME_CONTRACT.md`, `game.schema.json`, decisions.
1. **Split in place** (done) in dw2003recomp: `port/runtime` vs `port/game`, `port.h` split, N-slot overlay manager.
2. **Bootstrap** (done): the runtime, the shim, the tools and scripts moved here; `examples/hello`; this repo's CI. A
   scratch copy of dw2003recomp consumes it by local path and passes its full test suite.
3. **Launcher** (done): branding and disc identity from the game description, self-test driven by it.
4. **Converge**: the first game switches to the submodule; its release, CI and setup scripts adapted.
5. **Public and switch**: tag `v0.1.0`; dw2003recomp replaces `port/` and `launcher/` with the pin.

## License

MIT (`LICENSE`). The stack contains no game code and no game data.
