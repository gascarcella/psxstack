# Changelog

Every release of psxstack, newest first. Releases are semver tags (`vMAJOR.MINOR.PATCH`); a game pins one, and
`PSXSTACK_API` (`include/psxstack/game.h`) counts incompatible contract changes (docs/GAME_CONTRACT.md
"Compatibility and versions"). Issue and pull request numbers are this repository's.

## v0.3.19 (2026-10-28)
- **Launcher and in-game menu:** Settings is four screens, each its own entry in the navigation: Video (window,
  fullscreen, scaling, the rate, the menu's size), Graphics (the renderer, its resolution, 3D vertices and the filter
  set or sets, in tabs named by a game's scene labels, greyed out on the Software renderer), Audio and System (memory
  cards, updates, Steam, the settings file; the hook `set_settings_extra` is `set_system_extra`) (#114).
  `<PREFIX>_PORT_MENU_SCREEN` takes the page names; `settings` still opens Video. The settings file and the contract
  are unchanged: `PSXSTACK_API` stays 1.
- **Save states:** a run with no game (a `disc_required` game's `--input-test` without its disc, #64) no longer arms
  them. Since v0.3.7 every window run did, and the first vsync, on main's stack, stopped the run (`state: the vsync is
  not on the game stack or a fiber's`, status 1): dcb_decomp's disc-free input self-test failed. Now the input
  self-test skips the quick slot's round trip (`no game in this run`) and goes on to the menu's; the hotkeys and the
  debug channel give the same reason; `--save-state` and `--load-state` without the game are refused (status 64).
- **Tests:** `tests/hello_test.py --menu` also builds hello as a `disc_required` game (`-DHELLO_DISC_REQUIRED=ON`):
  without a disc it refuses to start, and its input self-test passes without the game.

## v0.3.18 (2026-10-26)
- **Video:** separate present filters for a game's 2D and 3D scenes (dw2003recomp #122, #111, #110). A game opts in
  with game.json's `video.scenes` (the rows' labels and the 2D set's default filter) and the adapter's
  `game_state_scene()`, read once per vsync; each set is a filter and its crt values, kept in `video.scene_2d` beside
  `video.filter`/`video.crt` (the 3D set), with `--filter-2d` on the command line. Both sets' pipelines stay built,
  so a scene change builds nothing. The launcher's Video screen and the in-game menu show a Filter row per set, the
  menu marking the one in use (`port_video_scene()`). Additive: `PSXSTACK_API` stays 1, and a game without scenes
  sees no change.
- **Existing players** of a game that opts in: the filter they already chose becomes the 3D scenes' filter, and the
  2D scenes take the game's 2D default (smooth for DW2003) until they change it.
- **VRAM:** `port_video_vram_extend`, a 1024x1024 VRAM for textures and CLUTs in Psy-Q's 2 MB layout, on both
  renderers and in save states; `port_video_widescreen_pad`, how far past the display's edges the wide canvas shows
  (dw2003recomp #78, #109).

## v0.3.17 (2026-10-24)
- **Docs:** the README leads with the features, then using the stack from a game and developing it (#107);
  `CONTRIBUTING.md`, the ground rules, the gates and the pull request steps; this changelog, out of the README.
- **Repository:** `.github/CODEOWNERS`; CI skips a change to it alone, as it does for Markdown and `LICENSE`.

## v0.3.16 (2026-10-24)
- **Video:** the picture fits the screen: `video.scaling`, `fit` by default, `integer` or `stretch`, the Scaling row
  in Settings and the in-game menu (#106).
- **Widescreen:** the canvas is at the window's aspect up to 16:9 (16:10 on a Steam Deck), in whole columns rounded
  down (426 at 16:9).

## v0.3.15 (2026-10-24)
- **Input:** a second pad on port 1: `input.player2`, the Controls screen's Player 2, `pad2` and `port` for scripts,
  the debug channel and the MCP server (#101, #104).
- **In-game menu:** its picture on the hardware renderer; unmuting a game started muted opens the audio device (#74,
  #105).

## v0.3.14 (2026-10-21)
- **Launcher:** the self-test finds its own process under the name the system lists (`AppRun` inside an AppImage)
  (#100).

## v0.3.13 (2026-10-21)
- **Launcher:** Add to Steam, Remove from Steam and Update shortcut in Settings (`launcher.steam`), with the optional
  Steam artwork from game.json's `launcher.steam_art` (#93).

## v0.3.12 (2026-10-21)
- **In-game menu:** fitted to the launcher's canvas, its Menu size (`video.menu_size`), a Mods page that fits
  640x480.

## v0.3.11 (2026-10-21)
- **Launcher:** the update check against the game's GitHub releases and the self-update (`launcher.updates` in
  game.json, curl as a process, the AppImage and Windows swap), Update now, `--no-update-check`.

## v0.3.10 (2026-10-21)
- **Mods:** the `stop` callback and `applies` in force, for the in-game menu's live switch.

## v0.3.9 (2026-10-21)
- **In-game menu:** the launcher's Settings, Controls and Mods inside the game's window on Escape or Back+Start,
  applied live (`PSXSTACK_MENU`).

## v0.3.8 (2026-10-21)
- **Mods:** `fast_loading` ("Faster loading"), a second stack mod beside `fast_forward`: 2x, 4x (the default), 8x or
  instant for the CD's data reads (ReadN/ReadS and CdRead without the XA bit), their seeks divided as much.
- **LIBCD:** the movie stream and the reads with the XA bit keep the realistic drive, under `--cd-speed instant` too,
  so a movie's video and XA audio play at the drive's rate (before, instant ran the video at the decoder's speed and
  overflowed the XA FIFO).
- The shim's `psyq_cd_set_data_speed`, the runtime's `port_disc_fast_loading`. `PSXSTACK_API` stays 1.

## v0.3.7 (2026-10-21)
- **Save states:** quick save and load hotkeys: `input.hotkeys.save_state` (F5) writes the whole game to the quick
  slot at the vsync's end, `load_state` (F9) resumes it (a paused game is paused again there); the slot is
  `--quick-state FILE`, default `quick.state` beside the settings file; `--input-test` ends with the slot's round
  trips (#72).

## v0.3.6 (2026-10-09)
- **Scripts:** a `write_mem` step on both runners (a PS1 address, 1, 2 or 4 bytes with an optional mask, or up to 256
  bytes), resolved through the adapter's state map first, then the arena, as `peek_ps1`/`poke_ps1` now are too (#67,
  #70).
- **Fibers:** a slot's stack is unpoisoned for ASan before its next fiber starts there; a destroyed fiber's redzones
  were still poisoned (#68, #69).

## v0.3.5 (2026-10-09)
- **Game description:** the optional `disc_required`: without a disc the port says so and exits 64 instead of running
  the game on reads that find no data; `--input-test` still runs, without `game_main` (#64, #65).

## v0.3.4 (2026-10-08)
- **Launcher:** the self-test compares the refresh fallback with the game's rate, not 50; `--fps`'s help names the
  game's rate (#57, #58).
- **Shim:** `psyq/README.md` lists the two GPU differences against PCSX-Redux the second game found as known emulator
  categories; `gpu.c` is unchanged (#54, #56).

## v0.3.3 (2026-10-08)
- **LIBC2:** `rand`/`srand` are the shim's (the PS1's generator, state 0 at start, renamed through `psyq_names.h`),
  held in save states and cleared by the reset; `port_rand_seed()` for a game's random-index probe (#40, #52).
- **Timing:** the CD and XA rates follow the description's `video.rate`; `--refresh` overrides (#40).
- **Replay runners:** `port_test.py --sanitize` sets `detect_stack_use_after_return=0` (#51); every run gets its own
  `--crash-dir` (#47).

## v0.3.2 (2026-10-08)
- **Scripts:** a checkpoint may say `"image": false`: neither runner dumps or hashes the checkpoint image there, the
  record carries the flag, and a script/record disagreement is reported (#49).

## v0.3.1 (2026-10-08)
The second game's shim and build fixes.
- **Build:** the units' include order is the probe's and `include/psxstack` is off their path (#35, #36).
- **LIBGTE and LIBGPU:** 51 functions the second game calls (#37) and eight function forms (#45).
- **LIBAPI, LIBCARD, LIBCD, LIBSND, LIBSPU:** events and root counters, the BIOS file calls and LIBCARD over the card
  images, LIBCD's file search and reads, LIBSND's SEQ API, LIBSPU (#38); every game unit is compiled with
  `psyq_names.h` forced in, renaming the Psy-Q names that are also the host's (#44).
- **LIBGS:** 20 functions and the `GsTMDfast*` handlers; a mapped TMD keeps offsets on the host (#41).
- **Tests:** `tests/psyq_test.py` and `tests/shim_test.py`; `examples/tasks` preempts through the vblank event.

## v0.3.0 (2026-10-08)
The contract grows for a second game, `PSXSTACK_API` unchanged.
- **Memory:** `memory.heap` is optional: a game may keep its heap as its own data (#26, #29); the GTE override is
  written at the game's header's path, the inventory takes a Psy-Q library map, `psyq/check.sh` takes its paths
  (#28).
- **Psy-Q declarations:** the stack owns them (`include/psxstack/psyq/`) and the shim compiles against them; the
  `decls` command checks a game's prototypes against them; the runtime and the shim no longer see a game's include
  directories (#7, #31).
- **Fibers:** a game's own tasks on host stacks of the runtime's, deterministic preemption at the tick's end, save
  states holding every fiber (#25, #32; `examples/tasks`).
- **Replay runners:** they live in the stack (`tools/replay/`: the emulator driver, the step engine, the boot check,
  the port's replay test, the emulator installer); a game keeps its probes and its pins (#27, #30).

## v0.2.1 (2026-10-08)
- **Fixed:** `runtime/mods_data.c` bounds a mod's id in the settings paths (`%.63s`), so Ubuntu 24.04's gcc 13
  Release build no longer fails on `-Werror=format-truncation` (#24).

## v0.2.0 (2026-10-08)
The hardware renderer's follow-ups.
- **Rendering:** Direct3D 12 on Windows (DXIL shaders), sub-pixel precision (`video.subpixel`, the perspective
  option), the present filters (`video.filter`: sharp, scanlines, smooth; `video.crt`), the wide canvas
  (widescreen).
- **Textures:** replacement: the dump mode, texture packs, the launcher's mods.
- **Save states:** `--save-state`, `--load-state`.

## v0.1.0 (2026-10-07)
The first release the Digimon World 2003 port builds on: the host runtime, the Psy-Q shim, the launcher, the game
description and its generator, `psxstack_add_game()`, the hello example, the tools (`port_gen`, `port_inventory`, the
MCP server) and the docs, consumed by dw2003recomp as a pinned submodule.

## v0.1.0-rc.1 (2026-10-07)
The runtime, the Psy-Q shim, the launcher and the build, as extracted from dw2003recomp (phases 0 to 3).
