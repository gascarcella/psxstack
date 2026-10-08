# psxstack

A reusable stack for PC ports of decompiled PlayStation games: the host runtime (memory arena, overlay manager,
interrupt pump, software and hardware rendering, SPU, input, memory cards, settings, crash reports, debug channel),
a Psy-Q shim, a launcher (SDL3, Dear ImGui) with mod manifests, and the build and packaging scripts. A game repo
(a matching decompilation) adds a small adapter and gets a Linux and Windows port, a launcher and a release.

It grew out of the [Digimon World 2003 decompilation](https://github.com/gascarcella/dw2003recomp)'s `port/` and
`launcher/`, which it replaces once version 0.1 is tagged.

**Status: phase 0.** The contract is written, no code has moved yet. `docs/GAME_CONTRACT.md` says what a game provides
and what the stack provides; `schema/game.schema.json` is the game description; `docs/DECISIONS.md` the decisions.

## Planned layout

| Path | Contents |
|---|---|
| `runtime/` | The host runtime (C): arena, overlay, pump, video, render_gpu, spu, audio, input, memcard, settings, json, sha1, crash, debug, framelog, platform, reset |
| `include/psxstack/` | The hook macros the game C uses (`hooks.h`), the adapter interface (`game.h`), the runtime's headers |
| `psyq/` | The Psy-Q shim: one file per library, plus the hardware models `gpu.c`, `gte.c`, `mdec.c`, `xa.c` |
| `launcher/` | The launcher (C++): settings, disc check, mods, input bindings, self-test; branding from the game description |
| `tools/` | `port_gen.py` (the generic generators), `mcp/` (the debug channel's MCP server), `port_inventory.py` (the host-compile gate) |
| `cmake/`, `scripts/` | `psxstack_add_game()`, the Windows toolchain, setup steps (SDL3, ImGui, DXC, llvm-mingw, AppImage), packaging |
| `schema/` | `game.schema.json`: the game description; `settings.schema.json`, `mod.schema.json` later |
| `examples/` | `dw2003.game.json` (the first game's description); `hello/`, a disc-free host-only Psy-Q program, the stack's smoke test |
| `docs/` | `GAME_CONTRACT.md`, `DECISIONS.md`; `PORT.md`, `LAUNCHER.md`, `RELEASE.md` once their generic parts move here |
| `tests/` | The stack's own tests: the launcher self-test, the debug-channel gate, the renderer comparison, the hello example |

## Phases

0. **Contract** (this): `GAME_CONTRACT.md`, `game.schema.json`, decisions. Both tracks code against it.
1. **Split in place** in dw2003recomp: `port/runtime` vs `port/game`, `port.h` split, N-slot overlay manager. No behavior change.
2. **Bootstrap**: the runtime, the shim, the tools and scripts move here; `examples/hello`; this repo's CI. A scratch copy of
   dw2003recomp consumes it by local path and passes its full test suite.
3. **Launcher** (parallel with 2): moves here, branding and disc identity from the game description, self-test driven by it.
4. **Converge**: the scratch copy switches to the submodule; release, CI and setup scripts adapted.
5. **Public and switch**: docs, THIRD_PARTY, tag `v0.1.0`; dw2003recomp replaces `port/` and `launcher/` with the pin.

## License

MIT (`LICENSE`). The stack contains no game code and no game data.
