# psxstack

A reusable stack for PC ports of decompiled PlayStation games: the host runtime (memory arena, overlay manager,
interrupt pump, software and hardware rendering, SPU, input, memory cards, settings, crash reports, debug channel,
save states), a Psy-Q shim, a launcher (SDL3, Dear ImGui) with mod manifests, and the build and packaging scripts. A
game repo (a matching decompilation) adds a small adapter and gets a Linux and Windows port, a launcher and a release.

The stack contains no game code and no game data; `docs/GAME_CONTRACT.md` is the line between the two. The first
consumer is [dw2003recomp](https://github.com/gascarcella/dw2003recomp), the Digimon World 2003 decompilation.

## Features

### Runtime
- **Memory and overlays:** an arena that keeps the PS1's distances between the game's memory slots, an N-slot overlay
  manager with data reset and address tables, an optional heap (docs/PORT.md "Memory arena", "Overlays").
- **Timing:** the interrupt pump at the game's nominal rate (50 PAL, 60 NTSC), root counters, vblank handlers
  (docs/PORT.md "Interrupts, the pump and timing").
- **Fibers:** a game's own tasks on host stacks of the runtime's, switched where its scheduler says and preempted at
  the end of a vsync tick (docs/PORT.md "Fibers").
- **Disc, memory cards and movies:** the disc image with a SHA-1 check against the game's known discs, memory cards
  as files, MDEC movies and XA audio at the drive's rate (docs/PORT.md "Disc, memory cards and movies").
- **Save states:** save and load anywhere, F5/F9 in the window, `--save-state`/`--load-state` on the command line
  (docs/RUNTIME.md "Save states").
- **Crash reports:** the build, the overlays, the game's state, the log's last lines and the stack, a minidump on
  Windows (docs/PORT.md "Crash report").
- **Debug channel** and an MCP server over it, for driving and inspecting a running game (`tools/mcp/README.md`).

### Rendering
- **The software GPU:** the PS1's picture pixel for pixel, headless or in a window.
- **The hardware renderer** on SDL_GPU (Vulkan; Direct3D 12 on Windows): internal resolution up to 8x, sub-pixel
  precision from the GTE's fractions, optional perspective-correct texturing (docs/PORT.md "Rendering",
  "Sub-pixel precision").
- **Present filters:** sharp bilinear, scanlines, CRT, xBR (docs/RUNTIME.md "The window"); a game with 2D and 3D scenes
  may give each its own (game.json `video.scenes`).
- **Screen fit:** fit, integer or stretch scaling; a wide 16:9 canvas a game's mod can turn on for the scenes that
  widen.
- **Texture dumps and texture packs**, keyed by content (docs/RUNTIME.md "Texture dump", "Texture packs").

### Sound and input
- **The SPU core** and the audio device over SDL3, a WAV dump for tests (docs/RUNTIME.md "The SPU core", "Audio").
- **Input:** keyboard and gamepads with rebindable buttons and hotkeys, a second player on a second gamepad
  (docs/LAUNCHER.md "Input bindings").
- **Input scripts:** replays a script drives frame by frame, the same grammar in the emulator and on the port
  (docs/RUNTIME.md "The replay runners").

### The Psy-Q shim
- Sony's libraries reimplemented over hardware models (`gpu.c`, `gte.c`, `mdec.c`, `xa.c`), never decompiled:
  LIBAPI, LIBC2, LIBCARD, LIBCD, LIBETC, LIBGPU, LIBGS, LIBGTE, LIBMCRD, LIBPAD, LIBPRESS, LIBSND, LIBSPU (docs/PORT.md "The Psy-Q shim").
- The stack's own Psy-Q declarations (`include/psxstack/psyq/`), checked against a game's (`tools/psyq_decls.py`).

### Launcher and in-game menu
- **The launcher:** disc check, video, audio, memory card, controls and mods settings, the game's crash report; every
  string from the game's description (docs/LAUNCHER.md, `launcher/README.md`).
- **Updates:** a check against the game's GitHub releases, Update now and the launcher's own update
  (docs/LAUNCHER.md "Updates").
- **Add to Steam:** the game as a non-Steam shortcut with its artwork, for Steam's library, Game Mode on the Steam
  Deck and its controller configuration (docs/LAUNCHER.md "Steam").
- **The in-game menu:** the launcher's Settings, Controls and Mods inside the game's window on Escape or Back+Start,
  applied live (docs/LAUNCHER.md "In-game menu").

### Mods
- Mod manifests with generated settings pages, code mods in the game, data mods from a mods directory, a load order
  (docs/LAUNCHER.md "Mod manifest", "Data mods", "Mod runtime").
- The stack's own mods, which every game has: `fast_forward` (hold Tab) and `fast_loading`.

### Build, platforms and tools
- **One CMake call:** `psxstack_add_game()` (`cmake/psxstack.cmake`) builds the game's C for a 64-bit host, with
  section renaming, post-link checks and a version stamp from the game's tags.
- **Platforms:** Linux, and Windows cross-built from Linux with llvm-mingw (tested under Wine); release packages an
  AppImage and a Windows zip.
- **Gates:** the host-compile inventory (`tools/port_inventory.py`), sanitizer and `-m32` builds, the replay runners
  against PCSX-Redux (`tools/replay/`), the launcher's self-test.
- **Reproducible tools:** `scripts/setup.sh` pins SDL3, Dear ImGui, DXC, llvm-mingw and CMake into `tools/`, no sudo.

## Contents
- [Features](#features): [Runtime](#runtime) · [Rendering](#rendering) · [Sound and input](#sound-and-input) ·
  [The Psy-Q shim](#the-psy-q-shim) · [Launcher and in-game menu](#launcher-and-in-game-menu) · [Mods](#mods) ·
  [Build, platforms and tools](#build-platforms-and-tools)
- [Using it from a game](#using-it-from-a-game): [What a game provides](#what-a-game-provides) ·
  [Adding the stack](#adding-the-stack) · [Building the port](#building-the-port) ·
  [Building the launcher](#building-the-launcher) · [Versions and releases](#versions-and-releases)
- [Developing](#developing): [Setup](#setup) · [Tests](#tests) · [Layout](#layout) · [Docs](#docs) ·
  [Contributing](#contributing)
- [Background](#background): [Status](#status) · [How it got here](#how-it-got-here)
- [License](#license)

## Using it from a game

### What a game provides
A matching decompilation whose C also compiles for a 64-bit host, plus a small adapter (`docs/GAME_CONTRACT.md`
"What the game provides"):
1. **`game.json`**, the game description (`schema/game.schema.json`): its id, title, discs, rate, memory slots,
   optional updates repository and Steam artwork. It is generated into a header at configure time; nothing reads it
   at run time.
2. **A hook header** holding the PS1 side of every hook macro and including `<psxstack/hooks.h>` under `PC_PORT`, so
   the matching build never depends on the stack.
3. **The hooks in its C** (`PLATFORM_WAIT`, `OVERLAY_COPY`, `SLOT_FUNC`, …).
4. **Adapter units** implementing `<psxstack/game.h>`, and its own mods.
5. **The build inputs:** the unit list, the overlay list, its symbol files.
6. **Its tests:** the goldens, the replay scripts and the emulator's probes.

### Adding the stack
The stack is a git submodule pinned at a release tag:
```sh
git submodule add https://github.com/gascarcella/psxstack psxstack
git -C psxstack checkout v0.3.17
```

### Building the port
```cmake
include(psxstack/cmake/psxstack.cmake)          # the submodule, pinned at a tag
psxstack_add_game(mygame GAME_JSON port/game/game.json UNITS build/gen/units.txt MAIN_UNIT src/main.c
                  OVERLAYS build/gen/overlays.txt INCLUDE_DIRS include . ADAPTER port/game/state.c ...)
```
`cmake/psxstack.cmake` documents every argument and docs/RUNTIME.md "Build and run" the options and the command line.
The consumers to copy from:
- `examples/hello`, the smallest: one unit, no overlays, no disc;
- `examples/tasks`, the same with its work in tasks on the stack's fibers;
- dw2003recomp's `port/CMakeLists.txt`, the first real one.

### Building the launcher
The launcher is its own CMake project, branded from the game's description:
```sh
cmake -S psxstack/launcher -B build/launcher -G Ninja -DPSXSTACK_GAME_JSON=... -DPSXSTACK_VERSION_ROOT=...
cmake --build build/launcher
```
`launcher/README.md` has the rest, the Windows build and the self-test among it.

### Versions and releases
Releases are semver tags; a game pins one, so a change here reaches it as a reviewable pin bump. `PSXSTACK_API`
(`include/psxstack/game.h`) counts incompatible contract changes. A game's release packages are named so the
launcher's update check finds them (docs/GAME_CONTRACT.md "Compatibility and versions", "Releases").
[`CHANGELOG.md`](CHANGELOG.md) lists what each release changed.

## Developing

### Setup
`scripts/setup.sh` builds the pinned tools into `tools/<name>` (gitignored, no sudo, idempotent):
```sh
scripts/setup.sh                                  # sdl3 imgui link: enough for the launcher and the tests
scripts/setup.sh dxc llvm-mingw sdl3-windows      # the hardware renderer's shaders, the Windows cross-build
scripts/dev_link_tools.sh ../dw2003recomp/tools   # or reuse a game checkout's built tools
scripts/setup.sh link                             # in a git worktree: link the main checkout's
```
A desktop window needs SDL3 built with the X11/Wayland `-dev` headers present (docs/RUNTIME.md "The window").

### Tests
None needs a disc or a game:
```sh
python3 tests/hello_test.py [--menu] [--gpu]      # the smoke test: examples/hello built through psxstack_add_game()
python3 tests/tasks_test.py [--sanitize|--m32]    # the fibers, with save states
python3 tests/shim_test.py [--sanitize|--m32]     # the shim's deterministic pieces on their own
python3 tests/psyq_test.py [--sanitize]           # the shim's LIBGTE and LIBGPU
python3 tests/game_gen_test.py                    # the description generator
python3 tests/sha256_test.py                      # runtime/sha256.c
python3 tests/replay_test.py                      # the replay runners against a fake emulator
python3 tools/mcp/selftest.py                     # the MCP server against a fake game
psyq/check.sh --compile                           # the shim with -Werror against the stack's declarations
cmake -S launcher -B build/launcher -G Ninja && cmake --build build/launcher
SDL_VIDEO_DRIVER=offscreen build/launcher/dw2003-launcher --self-test build/selftest
```
CI (`.github/workflows/ci.yml`) runs these on Linux and the Windows cross-build under Wine. A change to `runtime/`,
`psyq/`, `cmake/` or the contract is also tested against a real game: in a dw2003recomp checkout,
`cmake -S port -B build/port -G Ninja -DPSXSTACK_DIR=/path/to/psxstack` and its `scripts/test.sh` (it needs the
game's disc).

### Layout

| Path | Contents |
|---|---|
| `runtime/` | The host runtime (C): arena, overlay manager, pump, video, the hardware renderer, SPU, audio, input, memory cards, settings, mods engine, json, sha1, sha256, crash report, debug channel, frame log, platform, reset, save states, fibers (a game's own tasks) |
| `include/psxstack/` | The hook macros' host side (`hooks.h`), the adapter interface (`game.h`, `mods.h`), the Psy-Q declarations the shim implements (`psyq/`), the runtime's headers |
| `psyq/` | The Psy-Q shim: one file per library, plus the hardware models `gpu.c`, `gte.c`, `mdec.c`, `xa.c`; `check.sh`. Its declarations are `include/psxstack/psyq/*.h` |
| `shaders/` | The hardware renderer's HLSL (DXC to SPIR-V, for Windows also DXIL, at build time) |
| `mods/fast_forward/`, `mods/fast_loading/` | The stack's own mods, which every game has: fast-forward, faster loading |
| `launcher/` | The launcher (C++): settings, disc check, mods, input bindings, the update check, Add to Steam, self-test; branding from the game description; `ui/` the screens the game's in-game menu shares (`PSXSTACK_MENU`) |
| `cmake/` | `psxstack.cmake` (`psxstack_add_game()`), `imgui.cmake` (Dear ImGui for the launcher and the menu), `version.cmake`, `embed.cmake`, `windows-x86_64.cmake` (the llvm-mingw toolchain) |
| `windows/` | The game's Windows resource and manifest templates |
| `tools/` | `game_gen.py` (the description to a header), `port_gen.py` (the build's generators), `port_inventory.py` (the host-compile gate, configured by a game), `psyq_decls.py` (a game's Psy-Q declarations against the stack's), `replay/` (the emulator and port replay runners a game configures: `emulator.py`, `run.lua`, `boot_check.lua`, `port_test.py`, `redux.sh`), `mcp/` (the debug channel's MCP server, client and symbols) |
| `scripts/` | `setup.sh` (SDL3, ImGui, DXC, llvm-mingw, SDL3 for Windows, cmake), `dev_link_tools.sh` |
| `schema/` | `game.schema.json`: the game description |
| `examples/` | `dw2003.game.json` (the first game's description); `hello/`, a disc-free Psy-Q program built through the stack: its smoke test; `tasks/`, the same with its work in tasks on the stack's fibers |
| `docs/` | `GAME_CONTRACT.md`, `PORT.md`, `RUNTIME.md`, `LAUNCHER.md`, `DECISIONS.md`, `THIRD_PARTY.md` |
| `tests/` | The stack's own tests: `hello_test.py`, `tasks_test.py` (the fibers, with save states), `game_gen_test.py`, `psyq_test.py` (the shim's LIBGTE and LIBGPU, its C in `tests/psyq/`), `shim_test.py` (the shim's deterministic pieces on their own: events, the memory card's BIOS calls, `CdSearchFile`; its C in `tests/shim/`), `sha256_test.py`, `replay_test.py`, the launcher's fixtures (`launcher --self-test`, `tools/mcp/selftest.py`) |

### Docs
| Doc | What it covers |
|---|---|
| `CHANGELOG.md` | Every release and what it changed |
| `docs/GAME_CONTRACT.md` | What a game provides and what the stack provides |
| `docs/PORT.md` | The runtime's design: hook macros, arena, overlays, pump, shim, rendering, sound, crash report |
| `docs/RUNTIME.md` | Building, running, options and formats |
| `docs/LAUNCHER.md` | The launcher/game contract, the settings file, mod manifests, updates, Steam, the in-game menu |
| `docs/DECISIONS.md` | The decisions and why |
| `docs/THIRD_PARTY.md` | Borrowings and pinned tools |
| `tools/mcp/README.md` | The MCP server |
| `launcher/README.md` | Building the launcher and its self-test |

### Contributing
`CONTRIBUTING.md` has the ground rules (no game code, no game data, no Psy-Q SDK; no fact about a game in the stack;
the contract first), the gates a change runs and how a pull request is made. Problems and plans are
[GitHub issues](https://github.com/gascarcella/psxstack/issues); `.github/CODEOWNERS` names who reviews.

## Background

### Status
**`v0.3.17`.** One game builds on it so far; the second will widen the shim and the contract. Every release and what
it brought is in [`CHANGELOG.md`](CHANGELOG.md).

### How it got here
It grew out of the [Digimon World 2003 decompilation](https://github.com/gascarcella/dw2003recomp)'s `port/` and
`launcher/`, which consume it as a git submodule since `v0.1.0`. It was built in five phases out of dw2003recomp's
port (its `docs/DECISIONS.md` "The port stack lives in psxstack"): the contract first, the port split in place behind
it, the runtime and the launcher moved here against a scratch copy of the game, the game converged on the submodule,
then `v0.1.0`. `docs/DECISIONS.md` has the decisions.

## License

MIT (`LICENSE`). The stack contains no game code and no game data; `docs/THIRD_PARTY.md` lists what it borrows and the
tools it pins.
