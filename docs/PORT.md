# The PC port runtime

How a decompiled PS1 game runs as a native program on psxstack: the architecture, the PS1 assumptions the runtime
handles, and what it does not do. Building a game with it is `cmake/psxstack.cmake` (`psxstack_add_game()`;
GAME_CONTRACT.md "5. The build inputs"); the command-line options, the per-frame log and the record formats are in
`docs/RUNTIME.md`; the shim's per-function behaviour in `psyq/README.md`; the launcher and the settings file in
`docs/LAUNCHER.md`. The first game's own port documentation (its hooks, its adapter, its tests) is dw2003recomp's
`docs/PORT.md`. The choices behind this design are in `docs/DECISIONS.md`.

## Overview
- A game's C is compiled for the host with `-DPC_PORT` (and the game's own defines) and linked with the runtime
  (`runtime/`), the game's adapter and our own Psy-Q shim (`psyq/`) into one 64-bit executable named after the game.
- No emulator is linked and no Sony code is used: the game code runs natively, and the PS1 hardware the game reaches
  through Sony's libraries (GPU, GTE, SPU, MDEC, CD drive, pads, memory cards) is reimplemented in C behind the shim.
  Emulators serve only as external test oracles.
- The game's PS1 matching build is untouched: every port change in its C is either a hook macro from its own
  `port.h` that expands to the original code without `PC_PORT`, or an `#ifdef PC_PORT` block (GAME_CONTRACT.md "3").
- The default build is headless (what the tests run); `-DPSXSTACK_SDL=ON` adds the SDL3 window, input and audio device.
- The game reads the user's own disc image (BIN/CUE, SHA-1 checked against the game's description); the stack holds
  no game data and no game code.

## Source layout
| Path | Role |
|---|---|
| `include/psxstack/hooks.h` | The host side of the hook macros and the `port_*` interface the game's C calls; the game's `port.h` includes it under `PC_PORT` (GAME_CONTRACT.md "2") |
| `include/psxstack/game.h`, `mods.h` | The adapter interface the game implements (`game_main`, `game_apply_rate`, the `game_state_*` probes, `game_mods`) and a mod's shape; `runtime/game_defaults.c` has a weak default for each |
| `include/psxstack/types.h` | The PS1-style type names for the stack's own C (the game's `common.h` defines the same under the same guard) |
| `include/psxstack/port_runtime.h`, `port_harness.h`, `settings.h`, `spu.h`, `platform.h`, `json.h`, `sha1.h` | The runtime's internal interfaces |
| `runtime/` | The runtime: `main.c` (options, setup), `arena.c`, `overlay.c`, `pump.c`, `reset.c`, `disc.c`, `memcard.c`, `video.c`, `render_gpu.c`, `input.c`, `audio.c` and `spu*.c`, `script.c`, `framelog.c`, `settings.c`, `mods.c` (the engine and the fast_forward mod), `crash.c`, `debug.c`, `platform.c`, `json.c`, `sha1.c` |
| `psyq/` | The Psy-Q shim: one file per library, plus the hardware models `gpu.c`, `gte.c`, `mdec.c`, `xa.c`; `check.sh` compiles it alone and checks its coverage of a game |
| `shaders/` | The hardware renderer's HLSL, compiled to SPIR-V by DXC at build time and embedded (`cmake/embed.cmake`) |
| `mods/fast_forward/` | The one mod every game has (its manifest; the code is in `runtime/mods.c`) |
| `cmake/psxstack.cmake` | `psxstack_add_game()`: the whole build (below, "What the build generates") |
| `tools/game_gen.py` | The game description (`game.json`) to `psxstack_game_gen.h` and CMake variables |
| `tools/port_gen.py` | The generators the build runs: the override headers, the units' compile launcher (the overlay sections), the overlay address tables, the state tables, the post-link check |
| `tools/port_inventory.py` | The host-compile gate (`probe`, `link`) and the inventory of what a game's C needs (`counts`); a game configures it |
| `tools/mcp/` | The MCP server over the debug channel, the client, the symbols (configured by the game's `.mcp.json`) |
| `examples/hello/` | A disc-free Psy-Q program built through `psxstack_add_game()`: the stack's smoke test |

## Compiling the game C for the host
- **Flags:** C99 with GNU extensions (`gnu99`: unprototyped `f()` declarations are common in the game C and C23 would
  read them as `(void)`), `-fsigned-char` (the code relies on signed `char`), `-fwrapv`, `-fno-strict-aliasing`.
  On ELF `-fno-pie`/`-no-pie`, for the debug channel alone: it, the MCP server and a game's tests over them resolve host
  symbols with `nm`'s link-time addresses of the binary, which a PIE would relocate at load (Ubuntu's GCC
  links PIE by default). Nothing else needs it: the arena needs no link address (see "Memory arena") and the
  overlay sections are orphan sections the linker places after `.data`/`.bss`, outside GNU_RELRO (see "Overlays").
- **`INCLUDE_ASM`** is empty on the host (the generated `include_asm.h`): a function a game still keeps in asm is
  absent from the port, and a call to it is an unresolved symbol at link time, unless the game provides a stand-in.
- **`gte_*` macros** (the game's `gtemac.h`, MIPS `cop2` sequences; `GTEMAC` in `psxstack_add_game()`) are replaced at build time by a generated header
  that turns each sequence into the same register accesses on the software GTE (`tools/port_gen.py overrides`;
  see "GTE").
- **The host-compile gate:** `tools/port_inventory.py probe` (configured by the game's own wrapper) compiles every
  unit at `-m64` with `-Werror` on pointer/integer casts, `int-conversion`, implicit declarations and incompatible
  pointer types; `link` checks the objects for duplicate globals. A game's remaining host warnings (missing returns,
  `-Wmissing-braces`, ...) are in its matching C and left as they are.
- **Undefined behaviour the PS1 tolerated** is fixed in the C under `PC_PORT` or in a form that keeps the PS1 bytes,
  as the sanitizer runs find it (the first game's `tests/host/FINDINGS.md` lists its cases: for example a `% 0`,
  which does not trap on the R3000A but raises SIGFPE on x86). In-struct overruns a game relies on go in its UBSan
  suppressions and nowhere else.

## Hook macros (`include/psxstack/hooks.h`)
A game's `common.h` includes its `port.h`; no unit includes it directly. Each macro is the original code without
`PC_PORT`, in the game's header; with it, `port.h` includes `include/psxstack/hooks.h`, where the host side lives (the
PS1 build never needs that path). `tier` is a slot's 1-based index in the game description's `memory.slots`:

| Macro | The game's use | Host meaning |
|---|---|---|
| `PLATFORM_WAIT()` | The body of a busy-wait that only an interrupt can end | `port_wait()`: runs the pending vsync/CD/GPU "interrupts" |
| `PLATFORM_HALT()` | A deliberate endless loop | `port_halt()`: reports the place and exits |
| `PORT_SCRATCHPAD_STACK_ENTER/LEAVE` | A switch to a stack in the scratchpad (raw `$sp` asm) | Nothing: the normal stack |
| `OVERLAY_COPY(tier, file, dst, src, size)` | An overlay `memcpy` into a slot | `port_overlay_load()` (see "Overlays") |
| `SLOT_FUNC(type, addr)` | A function at a fixed address in a slot (the game wraps it in its own typed macros) | A *tag*: the PS1 address kept as an integer in a function pointer (valid in static initializers) |
| `OVERLAY_FN(tier, fn)` | A call through a pointer that may hold a tag | `port_overlay_resolve()`: a tag becomes the current overlay's host function; a real pointer passes unchanged |
| `LATE_FUNC` / `LATE_CALL` | A call by name into whatever overlay is loaded | Resolved by address through the current overlay's table |
| `SLOT_PTR(tier, type, addr)` | Data in a slot | The same offset into the arena's slot buffer |
| `HEAP_START/END/SIZE_FROM/ADDR` | The heap's bounds and constants inside it | The arena's heap region |
| `PTR_ADD(type, ofs, base)` | An offset-table resolve, written on the PS1 as `ofs + (s32)base` | Pointer arithmetic |
| `PTR_TO_S32` / `S32_TO_PTR` | A pointer kept in an `s32` field or argument | The pointer's PS1-style address in the arena, and back (fatal outside the arena) |
| `PTR_TO_U32(p)` | The 24-bit ordering-table tags (`setaddr`) | The pointer's word offset in the tag window (the game's data and the arena; fatal outside it) |
| `BIOS_PTR(type, addr)` | A read of the BIOS ROM | A stand-in region holding the description's `bios_standin` texts (`arena.c`) |

`port_file_wait_read(id)` is the one function hook: a game that frees a file the CD is still reading into calls it
and then waits for the read. A game's own hooks (its typed slot-function macros, its mods' flags) live in its
`port.h` under `PC_PORT`, after the include.

**The runtime and the game adapter** (GAME_CONTRACT.md "4"): the runtime reaches the game only through
`include/psxstack/game.h`, the adapter interface (`game_main`, `game_apply_rate`, the `game_state_*` probes and
checkpoint image, `game_state_read`/`game_state_host` for PS1 addresses outside the arena, `game_mods`), implemented
in the game's adapter with a weak default for each in `runtime/game_defaults.c`. The runtime's game facts come from
`psxstack_game_gen.h`, generated at configure time from the game's `game.json` (`tools/game_gen.py`):
`PSXSTACK_GAME_ID`/`_TITLE`/`_ENV_PREFIX`/`_RATE`, `PORT_SLOT_COUNT` and `PORT_SLOT<n>_BASE/SIZE/NAME/OFS`, the heap,
the accepted discs (`PSXSTACK_GAME_DISCS`) and the BIOS stand-ins. The brand strings (`--version`, the window title,
the crash report's header, the cache directory `<id>-port`, the disc error) and the environment variables
(`<PREFIX>_PORT_TRACE`, `_FAST_FORWARD`, `_CHECKPOINT_DIR`, `_RESET_CHECK`, `_CRASH_AT`, `_PRESENT_READBACK`,
`_GPU_VRAM_CHECK`, `_PRIM_DUMP`) read them.

## Memory arena
One static block, `port_arena`, stands for the PS1 RAM from the first slot up (`runtime/arena.c`; the sizes are the
description's, generated into `psxstack_game_gen.h`: the slots in address order, contiguous, then the heap,
`memory.heap.host_size` large). The first game's layout, as an example:

| Region | PS1 address | Size | Macro (on `port_arena`) |
|---|---|---|---|
| Slot 1 (the stage overlays) | `0x80082CB0` | `0x23130` | `port_slot1` |
| Slot 2 (the map files) | `0x800A5DE0` | `0x5A20` | `port_slot2` |
| Heap | `0x800AB800` | 4 MB (the PS1's is 1.3 MB; 64-bit runtime structs are larger) | `port_heap_start`, `port_heap_end` |

- The regions are macros on `port_arena + offset`, so their addresses stay constant expressions (a game's static initializers
  use them). A pointer's PS1-style address is `PORT_SLOT1_BASE + offset`
  (`port_ptr_to_s32`); an ordering-table tag is an offset in the tag window (`port_ptr_to_u32`, below).
- **No alignment or link address is assumed:** the block is 4 KB-aligned, the arena would work in a PIE (the ELF
  link stays non-PIE for the ld script's sake, "Compiling the game C for the host"), and nothing
  relies on the arena lying below 4 GB. This is what a PE (Windows) build needs: COFF allows no section alignment past 8 KB and ASLR moves
  the image.
- Everything a game's heap hands out (objects, packet buffers, ordering tables, the file cache) lives in the heap
  region, so most primitives and ordering tables are in the arena; a few may be static (the first game's battle
  cursor OT, in an overlay's `.bss`), which is why tags are offsets in the **tag window**, not in the arena.
- **The tag window** (`port_tag_base`, `port_tag_span`; `port_overlay_init` measures it, `arena.c` keeps it): the
  lowest to the highest address of the units' `.data`/`.bss` regions and the arena, i.e. one image's writable memory,
  a few MB (more in a sanitizer build, with ASan's redzone after each of the game's globals). A tag is a word
  offset in it, so it may span up to 64 MB (a startup check).
- The slot buffers hold data files loaded into a slot (files without code) and the targets of
  `SLOT_PTR`; code overlays are linked in, not copied (see "Overlays").

## Ordering tables on 64-bit
- PS1 primitives start with a tag whose address field is 24 bits (`P_TAG.addr`). A game sets it through
  `setaddr`/`addPrim` and may compare `ptr & 0xFFFFFF` with tags; `ClearOTagR` terminates with `0xFFFFFF`.
- On the host `PTR_TO_U32(p)` is the pointer's **word offset** in the tag window (`port_ptr_to_u32`; a fatal error
  for a pointer outside it or not word-aligned), so a tag's low 24 bits are that offset and this code works unchanged:
  the game only stores tags, passes them on (`addPrim`: `setaddr(p, getaddr(ot))`, where the macro keeps an integer
  argument as it is, chosen at compile time by the argument's type) and compares them; only the shim resolves one. `DrawOTag`/`ContinueDraw` follow a link as `port_tag_base + 4 * (tag & 0xFFFFFF)`, inside the tag
  window. A test harness that keeps PS1 lists as they are can give the shim a byte-offset window instead
  (`psyq_set_arena`), with its own `port_ptr_to_u32`.
- Primitive layouts therefore stay PS1-sized; no primitive type is widened.

## 64-bit layout
- **Runtime structs grow** on 64-bit (any struct holding a pointer). Code that hard-coded a PS1 size now uses
  `sizeof`: object allocations, heap allocations and `bzero`s. The literal sizes left are true byte counts, each
  marked `PC_PORT:` (`tools/port_inventory.py counts` lists them; `object-sizes` keeps the object data blocks from
  regressing).
- **Disc formats are unaffected** when the structs that describe disc data are pointer-free or are compiled from C,
  so they re-lay themselves out.
- **Save data is pointer-free** in a game the port is meant for (the first game's save slot is the pointer-free
  prefix of its state): cards written by the port and by the PS1 (or an emulator) are then byte-compatible.
- **The `-m32` build is the layout oracle:** pointers are 4 bytes there, as on the PS1, and the per-frame log holds no
  host address, so a run whose log differs between `-m32` and `-m64` has a pointer-size bug on one side.
  The first game's `tools/port_inventory.py structs` compares struct sizes at both widths with the documented PS1 sizes.

## Overlays
Every overlay is **linked statically** into the binary (a game's naming convention must leave no duplicate global
across overlays: `tools/port_inventory.py link` checks). The overlay manager (`runtime/overlay.c`) stands in for the
PS1's copy into the slot:

- **Data reset:** on the PS1 every load also resets the overlay's `.data`/`.bss`, because the file contains them.
  Each unit is compiled through `port_gen.py rename` (CMake's compiler launcher), which puts the object's
  `.data`/`.bss` sections into its overlay's (ELF: GNU objcopy renames them after the compile; PE: the overlay's
  `#pragma clang section` forced into the compile, since objcopy breaks COFF COMDATs and with them the unwind
  tables, `docs/RUNTIME.md`): on ELF `<id>_data_<ovl>`/`<id>_bss_<ovl>`, orphan
  output sections whose `__start_`/`__stop_` symbols GNU ld makes by itself; on PE the chunk groups
  `.psxdata$<ovl>_1`/`.psxbss$<ovl>_1` of the `.psxdata`/`.psxbss` sections, which lld sorts by their `$` suffix
  between generated empty marker chunks (`_0`, `_2`) carrying the same symbols (`port_gen.py markers`). No linker
  script (DECISIONS "Overlay sections by renaming, no linker script"). The manager snapshots the ranges at startup
  and restores them when `OVERLAY_COPY` loads a file, under the same "a different stage/file" condition the game
  checks. A post-link check (`port_gen.py sections`) fails the build if a writable section of a game object was
  not renamed.
- **Current overlay per tier:** a code file becomes its tier's current overlay; a data file (no table) is copied into
  the slot buffer, exactly as the PS1's `memcpy` would.
- **Address tables:** generated after compilation from each overlay's symbol file (`overlays.txt` names them),
  keeping each function that `nm` finds as a global. The generator checks every tag site in the C (`SLOT_FUNC` and
  `LATE_FUNC` sites it finds itself, against every table of their tier; the sites whose overlay the game knows, from
  its `tag_sites.txt`, against that overlay) and fails the build on one that does not resolve. At run time `port_overlay_resolve` looks a tag up in its tier's current overlay; a tag the current
  overlay does not define is fatal.
- **Copy time:** the PS1's byte-loop `memcpy` takes measurable time (0x19000 bytes are about 1.8 frames),
  and the emulator's checkpoints can observe the state in between. The manager runs `size * 12 / 677376` vsync
  ticks after a copy to keep that order.
- A file listed as an overlay without a symbol file is data only; it is loaded raw into the slot.

## Interrupts, the pump and timing
- The port is **single-threaded and deterministic**. A frame is one vsync tick, run from the game's `VSync()`, from
  `PLATFORM_WAIT()` (`port_wait`), or from LIBCD's `StGetNext` once per 5000 empty polls (the movie player spins with
  no wait hook).
- Each tick renders the vsync's audio, runs the game's vsync callback (frame counters, play time, the display flip,
  LIBSND's sequencer tick), then `port_frame` (`runtime/pump.c`): the CD tick, the frame log, the window's input or
  the script's step, screenshots and the window's present.
- **CD timing** is modelled in vsync ticks (`--cd-speed realistic`: 3 sectors per tick at double speed after a seek;
  `instant`: no seek, up to 75 per tick).
- **Real time** applies only with a window: vsyncs are paced to the nominal rate against `CLOCK_MONOTONIC`. Pacing
  changes only the time between vsyncs, so a window run's log and record equal the headless run's.
- **Frame rate:** the description's `video.rate` (50 PAL, 60 NTSC). `--refresh N` (one of the description's `rates`)
  sets the pace, audio and CD rates to match and calls the adapter's `game_apply_rate(N)` for the game's own response
  (the first game's NTSC-patch mode).
- **Watchdog:** `--watchdog SEC` exits when no `port_wait()` ran for that long (a loop no hook reaches).
- **Pause:** the pump can hold the game between two vsyncs (the window's pause key; the debug channel's pause, step
  and wait). Nothing of it reaches the game, the log or the record.
- **Debug channel** (`--debug SOCKET`): a tool drives the running game between two vsyncs (below).
- **Console reset** (`runtime/reset.c`): a script's `reset` step longjmps from the vsync tick back to `main()`,
  restores every game section from the startup snapshot, zeroes the arena and resets the shim, then runs the game's
  `main()` again. `<PREFIX>_PORT_RESET_CHECK=1` verifies the restore.

## Debug channel and the MCP server
`--debug SOCKET` (`runtime/debug.c`, whose header comment is the protocol, v1) opens a Unix stream socket of
newline-delimited JSON requests (`{"id", "op", ...}`), answered in order, on which a tool drives and inspects the
running game, headless or in a window. Without the option nothing of it exists (`port_frame` pays one branch): the
bare binary, the replays and the goldens are unchanged. The ops:
- `status`: frame, stage, file, map, paused, pace, pad owner. `pause` / `resume`: hold the game at the next vsync
  boundary (the pump's pause, shared with the window's pause key). `step frames`: exactly N vsyncs, then pause.
- `wait`: run until a host or PS1 read, the stage or the map equals a value, or a timeout in frames; leaves the game
  paused. `pad buttons frames release [sync]`: the channel owns pad 1 (`psyq_pad_set` each vsync) until `pad_free`;
  a script keeps precedence.
- `peek` / `poke`: raw host memory, any range `/proc/self/maps` says is mapped (a bad address never faults the game).
  `peek_ps1` / `poke_ps1`: a PS1 address, the arena (from the first slot's base) directly at any length, anything
  else through the adapter's state map (`game_state_read`, 1/2/4 bytes).
- `screenshot path` (the display image as a binary PPM), `hash` (the game-state image, as a checkpoint hashes it), `pace fps`, `reset` (the console reset, after the answer), `quit status`.

The game thread polls the socket itself: once per vsync from `port_frame` (after the script's step, before the video)
and 50 times a second while the pump holds it paused. So every command runs between two vsyncs, reads and writes are
frame-consistent, and a driven run is as deterministic as a scripted one (the same presses at the same frames give the
same log and record). A deferred op (`step`, `wait`, `pad` with sync) is answered when it completes, and nothing else is
read meanwhile. `--debug` turns the watchdog and the default frame cap off; a client that disconnects frees the pad and
resumes the game. `--debug-hold` holds the game paused at its first vsync until the client resumes it, so a run is
reproducible from frame 1 (without it an unthrottled headless game is past the boot by the time the client connects).

`tools/mcp/` (`tools/mcp/README.md`) is the MCP server on top of it, registered for Claude Code by the game's
`.mcp.json`, which passes the configuration on the command line (the root, the game description, the binaries, the
symbol files, the disc): `game.py` is the plain client (no MCP dependency; `Game.spawn`, one method per op),
`symbols.py` adds names (host addresses from `nm` on the ELF, PS1 addresses from the game's symbol files:
`mem_read("ps1:gamestate_data+8")`), `server.py` the tools (`game_start`, `pad_press`, `wait_stage`, `mem_read`,
`screenshot` as a PNG image, `state_hash`, ...). `fake_game.py` is the protocol double for `selftest.py`, which runs
with fixture symbol files and no game.

## The Psy-Q shim
`psyq/` implements the Psy-Q functions its games call (the first game: 123; `tools/port_inventory.py counts` lists
a game's; `psyq/check.sh --game-root DIR` checks that each, and the SDK data symbols the C uses, is defined) against
the prototypes in the game's recovered `psyq/*.h` headers (DECISIONS "Psy-Q headers: the game's, for now"). It is
our own code, MIT, written from the games' use of the API and public hardware documentation (psx-spx). The
libraries' internals (`_spu_*`, `_card_*`, ...) are never needed: only what a game calls. A function a new game needs
and the shim lacks stops the run with `port_unimplemented(name)` (status 3) and is added to the shim.

| Library | In the shim |
|---|---|
| LIBGPU | Real; drives the software GPU (`libgpu.c`, `gpu.c`) |
| LIBGTE | Real, on the software GTE (`libgte.c`, `gte.c`) |
| LIBGS | TIM info, the GTE set-up, the world-screen and light matrices (`libgs.c`); `GsInitGraph`/`GsInit3D` do the PS1's GTE and matrix set-up and skip its draw environments |
| LIBETC | `VSync`, `VSyncCallback`, `SetVideoMode`, `ResetCallback`: real, on the pump |
| LIBCD | A real command model over the disc image, timed in ticks; interrupt-driven sector reads, streaming (`St*`, `CdRead2`); XA sectors to the XA decoder (`libcd.c`, `xa.c`) |
| LIBPRESS | MDEC movie decoding (`libpress.c`, `mdec.c`) |
| LIBSND | The sequencer and voices over the SPU core (`libsnd*.c`; `docs/SOUND.md` in the first game) |
| LIBPAD | A digital pad on port 0; actuator calls accepted and ignored |
| LIBMCRD | Real, over `.mcd` images (`libmcrd.c`) |
| LIBC2, LIBAPI | Not defined: they resolve to the host libc |

`<PREFIX>_PORT_TRACE=1` traces every shim call. The behaviours each library still assumes (rather than checked
against the PS1 or an emulator) are listed in `psyq/README.md` "Behaviour assumed".

## Rendering
- **Software GPU** (`psyq/gpu.c`): a 1024x512 VRAM of 16-bit pixels and every GP0 drawing command the primitive types produce: flat/Gouraud/textured polygons with 4/8/15-bit textures and CLUTs, texture windows,
  the four blend modes, dithering, mask bits, lines, rectangles, fills, VRAM copies and transfers, the draw area and
  offset. Exact VRAM semantics matter: games reuse VRAM (`MoveImage`, `DR_MOVE` cursors with `BreakDraw`/`ContinueDraw`). It is built at `-O3` in every build.
- **Video output** (`runtime/video.c`): every vsync the display area set by `PutDispEnv` is read from the VRAM in 15-
  or 24-bit mode (movies and the title are 24-bit; 320x480 interlaced frames are shown whole) and converted to 32-bit
  pixels. `--screenshot` writes it as a PPM in any build.
- **Window** (SDL3, static, pinned in `scripts/setup.sh`): the image at 4:3, nearest-neighbour, integer-scaled;
  fullscreen toggle. SDL selects X11/Wayland and the audio backend at run time.
- **Hardware renderer** (`runtime/render_gpu.c`; the first game's DECISIONS "The hardware renderer"): SDL_GPU on Vulkan,
  only in the SDL build, chosen by `--renderer gpu` or `video.renderer` (default `software`). Its shaders
  (`shaders/*.hlsl`) are compiled to SPIR-V at build time by the pinned DXC (`scripts/setup.sh dxc`) and embedded
  in the binary. **Phase 1, now:** it opens the device and the window's swapchain, and presents the same software image
  pixel for pixel (an integer nearest mapping into video.c's 4:3 rectangle, equal to SDL_Renderer's output); when no
  device can present (no Vulkan driver; NVIDIA on SDL's offscreen driver) the run logs why and uses SDL_Renderer.
  `--gpu-screenshot FRAME[@WxH]:PATH` writes its picture (headless too). **Phase 2, now:** its rasteriser draws the
  software GPU's decoded command stream (gpu.c's listener: every triangle, rectangle, line segment, fill, copy and
  transfer) into a 1024x512 VRAM target of its own, and a 15-bit display is presented from it. Its pixel shader is
  gpu.c's pixel pipeline in integers (attributes from gpu.c's plane equations, coverage by the GPU with vertices shifted
  by half a pixel); blending, dithering and the mask test read a copy of the target refreshed per triangle or segment
  where something was drawn since (no fixed-function blending); texels come from a copy of the software VRAM uploaded in
  stream order from gpu.c's write stamps; an overlapping copy that smears takes its result from the VRAM. At internal
  scale 1 the target equals the software VRAM (the first game's gpu golden family and its replays, whole VRAM, NVIDIA
  and lavapipe). **Next:** internal resolutions up to 8x. The software GPU stays the reference and the default; every existing test uses it.

## GTE
`psyq/gte.c` is the geometry coprocessor in software: the 64 registers with their read/write rules, every
command (RTPS/RTPT with the UNR division, NCLIP, MVMVA, the lighting and depth-cue commands, AVSZ3/4, GPF/GPL, ...),
FLAG and the saturations, in the PS1's fixed point. A game's GTE code reaches it through the generated `gtemac.h`; LIBGTE's functions call it too.

## Input
- **Window input** (`runtime/input.c`): the keyboard and every gamepad SDL sees are ORed into pad 1, a digital pad,
  once per vsync. Bindings are by key position and SDL's positional (PlayStation-layout) gamepad buttons, rebindable
  in the settings file; hotkeys (fullscreen, pause, the mods') never reach the pad. The mapping and defaults are in
  `docs/RUNTIME.md` "The window".
- **Scripted input** (`runtime/script.c`): `--script` replays a pad script (the first game's layer-2 format, `docs/RUNTIME.md`)
  with the same step engine as the emulator's runner; the script then owns the pad.
- `--input-test` injects every binding through SDL (keys, a virtual gamepad, chords, hotkeys) and checks what reaches
  the pad.

## Sound
The SPU core (`runtime/spu.c`, `spu_dsp.c`), LIBSND (`psyq/libsnd*.c`), the XA path and the audio output
(`runtime/audio.c`: rendered once per vsync, `--wav`, the SDL3 audio stream with drift correction) are described in
the first game's `docs/SOUND.md`. The audio is a function of the SPU writes and the vsync count, so it is deterministic and identical
with or without an output device.

## Disc, memory cards and movies
- **Disc** (`runtime/disc.c`): the user's BIN/CUE, its SHA-1 checked against the description's `discs` (cached with a
  stamp; `--no-disc-check` skips it; a game with no discs takes no `--disc`). LIBCD reads it at sector level, so a
  game's file table, its 2340-byte read mode and movie streaming work unchanged.
- **Memory cards** (`runtime/memcard.c`, `psyq/libmcrd.c`): raw 128 KB `.mcd` images (the usual emulator
  format), one per slot; every change is written back to the file. A new card is formatted as PCSX-Redux formats
  one. Saves move both ways between the port and the emulator.
- **Movies:** STR files play through the movie stream (`libcd.c`), MDEC decoding (`mdec.c`) and the XA-ADPCM
  decoder (`xa.c`); no FFmpeg.

## Settings and mods
`<game> --config FILE` reads the settings file the launcher writes (disc, window, audio, memory cards, input bindings,
mods); without `--config` the binary depends on nothing on the machine. The mods' engine is `runtime/mods.c`: the
stack's fast_forward and the game's mods (`game_mods`), their manifests (`mods/<id>/mod.json`, the stack's and the
game's directories) copied beside the binary; they are off under `--script` unless `--script-mods`. Formats and
behaviour: `docs/LAUNCHER.md`, `docs/RUNTIME.md` "The settings file" and "The mods".

## Testing
The stack's own tests need no game and no disc (`.github/workflows/ci.yml`):

| Check | What it proves | Where |
|---|---|---|
| hello | A Psy-Q program builds through `psxstack_add_game()` (the generators, the sections, the shim, the runtime) and draws the same picture everywhere | `tests/hello_test.py` |
| The game description | `game_gen.py` validates and generates; the examples validate | `tests/game_gen_test.py` |
| The launcher | Its headless self-test on Linux, and under Wine from the Windows cross-build | `launcher/README.md` |
| The debug tools | The client, the symbols and the server's tools against the protocol double | `tools/mcp/selftest.py` |

A game brings the oracles: an emulator's goldens replayed through the shim's C, its pad scripts replayed by the port
against the emulator's records, its launcher self-test with the real disc and game (GAME_CONTRACT.md "6"). The first
game's table is in dw2003recomp's `docs/PORT.md` "Testing".

## Crash report

When the game dies of a signal (SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT) or stops on a fatal error, a halt, an
unimplemented part or the watchdog, `runtime/crash.c` writes one text file, `crash-<YYYYMMDD-HHMMSS>.txt` (UTC), into
`--crash-dir DIR` (the launcher passes `<settings dir>/crashes/`; default: the current directory), and names it on its
last stderr line: `port: crash report: PATH`. The report holds the kind and status, the build (`port_version` and the
commit: `<game> --version`; `cmake/version.cmake` stamps every build from `git describe` over the game's repository), the platform, the vsync
count, the overlays in both tiers, the stage, file and map, the pad, whether a script runs, the last 64 lines of the
port's log, and, for a signal, the signal, the fault address, the pc and sp and the stack; for a fatal stop the reason
and the stack. Code addresses are relative to the executable (`exe+0x...`): the first game's `scripts/symbolize.py REPORT --binary
FILE` names them with `addr2line` against the unstripped build or the release's `.debug` file. The
signal handler is async-signal-safe (a static buffer, `write`), runs on its own stack and is one-shot: after the
report the signal's default action ends the process, so the exit status stays the signal's. The report never enters
the frame log or the record. Test hook: `<PREFIX>_PORT_CRASH_AT=VSYNC` writes through a NULL pointer at that vsync
(`tests/port/crash.py`). A game's CI can keep the reports as an artifact.

**On Windows** the report is the same file. A crash is an unhandled SEH exception (`SetUnhandledExceptionFilter`):
the filter writes the text with `exception:` (the code's name and value, `EXCEPTION_ACCESS_VIOLATION (0xc0000005)`),
`fault address:` and `access:` (read, write or execute, for an access violation), `pc:` and `sp:` from the exception's
context, and the stack walked from that context with `RtlVirtualUnwind` over the image's unwind tables (frame 0 is the
faulting instruction, the rest return addresses, through the game's frames to `main`: the units keep their `.pdata`,
"Overlays"). Then a helper thread writes `crash-<stamp>.dmp` beside it with `MiniDumpWriteDump`
(dbghelp.dll, loaded only then; `MiniDumpNormal | MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory`: the
threads' stacks and contexts, the module list, the exception record, the image's writable data and what the stacks
point at; a few MB on Windows, 22 KB under Wine, whose dbghelp writes the stacks and the records but ignores the data
flags), the text gets a `minidump:` line, stderr `port: minidump: PATH`, and the process ends with the exception code
as its exit status (what Windows reports for an unhandled exception; the launcher names the codes; `wine` itself
exits with the low byte, 5). `abort()` (and UCRT's invalid-parameter and pure-call handlers, which abort after setting
the reason) writes a report of kind `abort` and exits 3, UCRT's status for abort. The watchdog's thread suspends the
main thread and reports its registers and stack. The `.dmp` opens in WinDbg or Visual Studio with the build's PDB
(`<game>.pdb` beside the exe); llvm-symbolizer (llvm-mingw's) resolves a report's `exe+0x...` addresses with it. A
stack overflow gets the text (the filter runs on what the guard page leaves) and perhaps no dump.

## Known limitations
- **Pads:** one digital pad on port 0; no analog mode, no rumble (`PadSetAct` is accepted and ignored), no second
  port or multitap.
- **No reset key** in the window (the console reset exists only as a script step and a debug-channel op).
- **Timing stand-ins:** the CD seek times (3 ticks + 1 per 8192 sectors, at most 40) and `StGetNext`'s 5000 polls
  per vsync are estimates, not measurements.
- **State probes:** `game_state_read` (a script's `wait_mem`) maps what the game's adapter maps; other pointer-bearing
  objects and overlay data read as unmapped.
- **BIOS:** a stand-in holding the description's texts, not the user's BIOS.
- **Windows** (cross-built from Linux with llvm-mingw, `cmake/windows-x86_64.cmake`; tested under Wine): the runtime's
  operating-system calls are in one file with a POSIX and a Windows half, `runtime/platform.c` (`platform.h`: paths
  with drive letters and `\`, a replacing rename, the per-user cache directory, positional reads of the disc image, a
  monotonic clock and a high-resolution sleep for the pace, the watchdog as a thread); the frame log, the record and
  the SPU trace are written in binary mode (the same bytes on both); stderr is unbuffered on Windows (UCRT has no
  line buffering); the console reset's `setjmp` takes no SEH frame on mingw (`port_setjmp`); `--debug` is refused
  there (the channel is a Unix socket). The executable is a GUI-subsystem program (no console window behind it when
  the launcher starts it; stderr still reaches the launcher's pipe) with a manifest (`windows/`: the UTF-8 code page,
  long paths, per-monitor DPI). A crash writes the same report as on Linux plus a minidump ("Crash report"). The SDL
  window is 64-bit only; macOS is not planned.
- **`long` is 32-bit on Windows (LLP64):** the runtime's `long`s are counters and option values (`port_frames`,
  `port_max_frames`, the pace, the step counts), none of which holds a pointer or a byte count over 2 GB; the shim's
  `PSYQ_PTR` and `VSyncCallback` use `uintptr_t`. A game's own `long`s are its to audit; the `-m32` build, where `long`
  is 32-bit too, replaying with the same log as the 64-bit build brackets LLP64 between the two.
- **Sanitizer builds** see other section sizes (ASan's redzones), so logs compare only between builds of the same
  kind.
