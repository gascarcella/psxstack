# port/psyq: the Psy-Q shim

Our own implementation of the Psy-Q library functions the game calls (docs/PORT.md "The Psy-Q shim";
DECISIONS "PC port architecture"): one C file per library, against the stack's own prototypes in
`include/psxstack/psyq/*.h` (DECISIONS "Psy-Q declarations: the stack's"; a game's declarations must agree in ABI:
`tools/psyq_decls.py`).
MIT, like the repo. Written from the game's own use of the API and public hardware documentation; no SDK file, no
emulator code (PsyCross, MIT, was consulted for signatures only).

**M1 skeleton: headless, records** (but a real GTE: `gte.c`, since M2 a real GPU: `gpu.c`, and since M3 a real LIBSND: `libsnd*.c`). Every stub returns what lets the game go on; nothing
allocates host memory the game sees, and nothing stores a game pointer in a 32-bit field.

## Interface to the port runtime: `psyq.h`
`psyq_vsync_tick()` and `psyq_cd_tick()` are the "interrupts": the port's pump (`port_wait()`) calls them from the
game's busy-waits. `psyq_set_trace()` turns tracing on. `port_unimplemented()` is declared here and defined by the
runtime (the cases that call it are listed below the table). The optional extras (`psyq_set_arena`, `psyq_cd_set_reader`,
`psyq_cd_set_timing`, `psyq_pad_set`, `psyq_gpu_take_hash`) are what the M1 test ("boot to STDWTITL's menu with scripted input, a primitive-stream hash
per frame") plugs into; the runtime may ignore them.

## What is real, what is a stub
| Library | Real (computes the right answer) | Fixed answer / recorded only |
|---|---|---|
| LIBGPU `libgpu.c` | `SetDefDispEnv`, `SetDefDrawEnv` (`dfe = h < 289`), `SetDrawEnv` (the PS1's packet: E3, E4 clamped to the VRAM, E5, E1, E2, E6, and with `isbg` a TILE over the clip area), `PutDrawEnv` (the same packet, but a fill when the clip's x and width are multiples of 64; terminated and drawn at once, part of the frame's primitive stream), `SetDrawTPage`, `SetDrawMove`, `SetDrawArea` (E3/E4 clamped), `SetTexWindow`, `SetDrawStp`, `SetDrawMode` (E1 + E2, a 0 word for no window), `GetTPage`, `GetClut`, `SetSemiTrans`, `SetShadeTex`, the function forms `SetSprt`, `SetPolyF3/FT3/G3/GT3/F4/FT4/G4/GT4`, `SetLineF2/G2/G3`, `SetTile` (the macros' length and command bytes), `AddPrim` (the tag mechanism: `addPrim`), `MargePrim` (-1 past 16 words), `ClearOTag`, `ClearOTagR`; `DrawOTag`/`ContinueDraw` walk the list, hash the primitives and draw them; `LoadImage`/`LoadImage2`, `MoveImage`/`MoveImage2` (-1 and nothing for an empty rectangle), `ClearImage`/`ClearImage2` (a fill, or E1 + a TILE when x or w is not a multiple of 64, as LIBGPU does) into the VRAM, `StoreImage`/`StoreImage2` from it (wrapping at its edges); `OpenTIM`/`ReadTIM` (the CLUT and image blocks of TIMs back to back, NULL at the end); `GetDispEnv` (the last `PutDispEnv`'s, all 0xFF after `ResetGraph` 0/3 or `SetDispMask(0)`); `ResetGraph` 0/3 resets the drawing state; `BreakDraw` NULL (idle, as on the PS1); `psyq_gpu_vram`/`psyq_gpu_display` (the video output, `psyq.h`) | `DrawSync` 0, `IsIdleGPU` 0 (drawing completes when queued: the `2` forms are the same calls), `SetGraphDebug` 0, `SetDispMask`, `PutDispEnv` (returns env, stores the display) |
| GPU `gpu.c` | The GPU in software (session 16, M2): a 1024x512 VRAM of 16-bit pixels and every GP0 drawing command (polygons flat/Gouraud/textured with 4/8/15-bit textures and CLUTs, the texture window, raw and modulated texels, the four blend modes, dithering, mask set/check, lines and polylines, rectangles, fill, VRAM copy, CPU-to-VRAM transfer; E1..E6), the draw area and offset; the emulator's rasterisation rules taken over where documentation is silent (`gpu.c`'s header) | GP1 beyond the drawing state; timing, the texture cache, VRAM-to-CPU |
| GTE `gte.c` | The geometry coprocessor in software (session 16): the 64 registers with their read/write rules, every command (RTPS/RTPT with the UNR division, NCLIP, OP, DPCS/DPCT, INTPL, MVMVA, NCDS/NCDT, CDP, NCCS/NCCT, CC, NCS/NCT, SQR, DCPL, AVSZ3/4, GPF, GPL), FLAG, the saturations; the game's `gte_*` macros call it (`tools/port_gen.py overrides` translates each MIPS sequence of `include/psyq/gtemac.h` into the same register accesses: `psyq_gte_mtc2/mfc2/ctc2/cfc2/cmd`, declared in `psyq_internal.h`; a `swc2` is `psyq_gte_swc2_`). Beside SXY, `gte_shadow.c` keeps the fractions RTPS cuts away for the hardware renderer (`docs/PORT.md` "Sub-pixel precision") | — |
| LIBGTE `libgte.c` | `rsin`, `rcos` (computed 4096-entry table), `RotMatrixYXZ_gte`, `RotMatrixZYX_gte` (LIBGTE's GPF sequence), `ScaleMatrix`, `ApplyMatrixSV` (MVMVA); the register setters write the GTE: `InitGeom` (ZSF3/4, H, DQA/DQB, OFX/OFY), `SetGeomOffset`, `SetBackColor`, `SetGeomScreen` (LIBGS's `GsSetProjection`), `SetRotMatrix`, `SetLightMatrix`, `SetTransMatrix`, and for LIBGS (not called yet: see below) `SetFarColor`, `SetColorMatrix`; for LIBGS's view: `MulMatrix`, `MulMatrix2`, `ApplyMatrixLV` (MVMVA), `TransposeMatrix`, `SquareRoot0` (LZCS/LZCR and the table). For the second game (each issues the commands of LIBGTE's own objects, in their order, with their stores and FLAG): `RotTransPers`/`3`/`4`, `RotTrans`, `RotAverage4`, `RotNclip3`/`4`, `RotAverageNclip3`/`4` (RTPS/RTPT, NCLIP, AVSZ3/4), `NormalColorCol`/`3` (NCCS/NCCT), `MulMatrix0`, `CompMatrix` (MVMVA, the translation through MAC), `PushMatrix`/`PopMatrix` (RT and TR, 20 deep), `TransMatrix`, `VectorNormal`, `MatrixNormal` (SQR, OP, GPF, the leading-zero count and the 1/sqrt table), `RotMatrix`, `RotMatrixYXZ` (on the CPU, every product floored), `csqrt` (the hyperbolic CORDIC), `ratan2` (the atan table), `catan` (the circular CORDIC: see below) | — |
| LIBGS `libgs.c`, `libgs_sort.c` | `GsGetTimInfo` (parses the TIM header); `GsSetProjection` (H); `GsSetRefView2` (the world-screen matrix from viewpoint, reference point, twist and `super`), `GsGetLw` (the coordinate hierarchy and its `flg`/PSDCNT cache) and `GsSetFlatLight` (the light and light colour matrices, LCM), checked against the PS1 by the layer-1 family `libgs_view` (issues #7, #19: the battle camera and lights; `tests/host/libgs_replay.py`, with LIBGTE's `MulMatrix`/`ApplyMatrixLV`/`TransposeMatrix`/`SquareRoot0`); `GsInitGraph`'s GTE set-up and matrices (the view base with the aspect in `m[1][1]`, `GsIDMATRIX`, the zero light matrices, PSDCNT 1); owns `GsWSMATRIX`, `GsLIGHTWSMATRIX`, `GsIDMATRIX`, `GsFCALL4`, `GsOUT_PACKET_P`. For the second game (each with the calls and order of the PS1's LIBGS 4.x objects; `tests/psyq_test.py` checks hand-worked views, a three-level hierarchy, a synthetic TMD sorted and drawn): `GsGetLs`/`GsGetLws` (GsGetLw's walk, then `GsWSMATRIX`), `GsInitCoordinate2`, `GsMulCoord3`, `GsSetLsMatrix` (RT, TR), `GsSetLightMatrix` (LLM = `GsLIGHTWSMATRIX` x m, the GTE's rotation kept), `GsSetAmbient` (BK = c >> 4), `GsSetLightMode` (0..3), `GsSetWorkBase`/`GsGetWorkBase`, `GsInit3D` (the screen's centre to the GTE, or with GsOFSGPU to the drawing environment), `GsSwapDispBuff` (puts LIBGS's display and drawing environments, counts PSDCNT, alternates the buffers), `GsMapModelingData` (the host's form: offsets from each object's entry, `libgs.h`), `GsLinkObject4` (the entry; the primitive runs' lengths), `GsSortObject4` (the attribute's GsDOFF/GsALON/GsDIV/GsLOFF/GsLLMOD, the run through its `GsFCALL4` entry) and the 18 fast handlers `GsTMDfast{F3L,G3L,TF3L,TF3NL,TG3L,TG3NL,TNF3,TNG3,F4L,F4NL,NF4,G4L,TF4L,TF4NL,TG4L,TG4NL,TNF4,TNG4}` (RTPT, NCLIP, RTPS, AVSZ3/4, NCCS/NCCT and the packet words in the PS1's order) | `GsInitGraph` records the rest (no GPU reset, no PutDrawEnv/PutDispEnv: the games draw with their own); the 9 subdividing handlers `GsTMDdiv*` call `port_unimplemented` (none is reached by a game yet); a `GsFCALL4` entry left 0 calls `port_unimplemented` (the PS1 jumps to 0) |
| LIBETC `libetc.c` | `VSyncCallback` (stores, returns the previous), `SetVideoMode` (returns the previous), `VSync` (ticks the vsync inline for modes 0 and > 1, returns the count). The vsync tick (`psyq_vsync_tick`) runs, in order: the pad, the runtime's audio, the `VSyncCallback` handler, LIBSND's own tick (`SsStart`), the memory card's command (LIBCARD), the root counter 3's events (LIBAPI), the runtime's frame | `ResetCallback` 0 |
| LIBCD `libcd.c` | `CdIntToPos`, `CdPosToInt`; the command model: `CdControl`/`CdControlB` apply at once and return 1, `CdControlF` completes on the next tick with `CdlComplete` to the sync handler; a read delivers its sectors from the sector source (the BIN, `runtime/disc.c`), one `CdlDataReady` call per sector, at the drive's rate in vsync ticks (`psyq_cd_set_timing`: "realistic" = 3 sectors per tick at double speed, 1.5 at single, after a seek of 3..40 ticks; "instant" = up to 75 per tick, no seek), stops at a command a handler issues, ends with `CdlDataEnd` past the source's end; `CdGetSector` reads through the delivered sector in the size the mode byte selects; `CdReadyCallback`/`CdSyncCallback` store and return the previous. The high-level calls: `CdSearchFile` looks a path (`\\P.DRV;1`) up in the disc's ISO 9660 directories through the sector source (fp, NULL when missing, -1 without a volume); `CdRead` reads whole sectors into a buffer at the drive's rate (no ready handler), `CdReadSync` reports the sectors left (0 done, -1 the source ended), `CdSync` the CdControlF command's completion (both wait in vsync ticks in mode 0); `CdMix` sets the drive's volume matrix, applied to the XA audio (unity: untouched). The movie stream: `CdRead2` streams from the Setloc position at the drive's rate, video sectors (StHEADER magic `0x80010160`) are assembled into whole frames in the game's `StSetRing` buffer (a slot = the 32-byte StHEADER + the frame's data); XA audio sectors (Mode 2, submode audio + real time, with Setmode bit `0x40`; `CdlSetfilter`'s file/channel with bit `0x08`) go to the XA decoder in a read as in the stream, never to the CPU, and reach the SPU's CD input (`spu_cd_input`) as 882 frames per tick after a one-tick wait (M5, docs/SOUND.md "CD audio"); `StGetNext` hands out the oldest complete frame (and runs a vsync tick every 5000 empty polls, as the PS1's interrupts run while the player spins), `StFreeRing` releases it, `StSetStream` keeps the frame range, `StUnSetRing` ends the stream (and, as `Pause`, a new read, `CdInit` and the reset, flushes the XA audio), `StClearRing` empties the ring, `StRingStatus` gives its free and dropped sectors, `StGetBackloc` the frame after the newest complete one and its sector (where a stalled stream resumes). Owns `StCdIntrFlag` (always 0) | `CdInit` 1, `CdSetDebug` 0, `StCdInterrupt` (nothing is deferred); `StSetStream`'s callbacks are not called (the games pass none); Mute/Demute are not modelled (no game uses them) |
| XA `xa.c` | The drive's XA-ADPCM decoder (psx-spx "CDROM XA Audio ADPCM Compression": 18 sound groups, 4- and 8-bit units, the four filters, ranges 0..12 and 13..15 as 9, mono/stereo, 37,800/18,900 Hz) and its 37,800 -> 44,100 Hz "zigzag" resampler (the seven 29-point tables); libcd.c drives it. Checked against an independent Python model (`tests/xa/`: 1317 unit checks; MOVIEOPN.STR's 2261 audio sectors decode to identical PCM) and against the emulator's audio (the movie's soundtrack, correlation 1.0000 per 0.1 s window) | emphasis (coding bit 6: its formula is unknown, no game uses it) is not applied; the 18,900 Hz path (each sample played twice) is our reading; the six-step counter starts at 6 |
| LIBPAD `libpad.c` | A digital pad on port 0 (none on port 1): `PadInitDirect`/`PadInitMtap` fill the buffers (status 0, id 0x41, buttons active low), `PadChkVsync` 1 once per vsync tick, `PadGetState` 6 (stable) / 0, `PadInfoMode(…, 2, …)` 4 (digital) | `PadStartCom` 0, `PadStopCom`, `PadInfoAct` 0, `PadSetAct`, `PadSetActAlign` 0, `PadSetMainMode` 0 |
| LIBMCRD `libmcrd.c` | The cards over raw 128 KB `.mcd` images (`runtime/memcard.c` inserts them with `psyq_mcrd_set_card`; the layout: docs/FORMATS.md "The card image"): `MemCardExist`/`Accept`/`ReadFile`/`WriteFile` register an asynchronous command (1; 0 while one is pending) that `MemCardSync` reports done with its number (1 Exist, 2 Accept, 3 ReadFile, 4 WriteFile) and LIBMCRD's result (0 none, 1 no card, 2 invalid, 3 new card, 4 not formatted, 5 no such file); `MemCardCreateFile` (first free blocks; 6 exists, 7 full), `Format` (the directory a new PCSX-Redux card has), `Unformat` (frame 0 cleared), `GetDirentry` (`*`/`?` patterns; name, size, head) answer at once. The first access after insertion or `psyq_mcrd_reset` answers 3 (new card) and clears the flag, as in the emulator. Every change of an image goes back to its file. The card store (images, written-back callbacks, the new-card flags: `psyq_card_*`, `psyq_internal.h`) is shared with LIBCARD and the BIOS's file calls | `MemCardInit`, `MemCardStart`; the timing: a command completes at the 2nd (Exist), 4th (Accept) or (1 + bytes/128)th (a transfer) `MemCardSync` poll, `MemCardSync(0, …)` at once (the emulator's card is slower; the scripts wait on `memcard_state`) |
| LIBSND `libsnd*.c` | The 24 functions the first game calls, over the SPU core (`port/include/spu.h`; M3, session 16, docs/SOUND.md section 7): VAB headers and bodies (DMA to the PS1's SPU addresses; `SsVabTransCompleted` 0 until the DMA's completion), SEPs and the score table (with its aliasing), the sequencer at `SsSetTickMode`'s rate (notes, program change, pitch bend, CC 0/6/7/10, NRPN loops and reverb attributes, tempo, end of track, play counts, decrescendo), the voice manager (allocation, volumes, pitches, the per-tick flush), `SsUtKeyOn/KeyOff/AllKeyOff`, the reverb presets and depth, main/CD volume, `SsInit` with LIBSPU's start-up stores (`libsnd_spu.c`). Exact against the emulator's SPU write traces when replayed on its timeline (`tests/port/sound.py`). `CdInit` (`libcd.c`) makes LIBCD's five SPU stores. For the second game: SEQ files (`SsSeqOpen`/`Close`/`Play`/`Stop`/`SetVol`/`GetVol`: an access number's sequence 0 on the same sequencer), `SsStart`/`SsStart2` with a display tick mode (`SS_TICK60`, `SS_TICK50`, `SS_TICKVSYNC`: one `SsSeqCalledTbyT` per vsync, `psyq_snd_vsync`; `SS_NOTICK`: none, the game ticks), `SsSetMono`/`SsSetStereo`, `SsUtKeyOnV`/`SsUtKeyOffV` (a given voice), `SsUtReverbOff` | noise voices, RPN/VAG attributes and NRPN attributes other than 15/16, CC 11/64/91/100/101/121, the faster tick modes (`SS_TICK120`/`240`, a rate per second: `port_unimplemented`) |
| LIBSPU `libspu.c` | What a game calls itself (the second game; LIBSND's own LIBSPU work is `libsnd_spu.c`): `SpuSetVoiceAttr` (direct volumes, pitch, start and loop addresses, ADSR1/2 and their fields by mask, voice by voice; the release rate the game sets on all 24), `SpuSetCommonAttr` (main volume, CD and external volumes, SPUCNT's CD/external enable and reverb bits), `SpuClearReverbWorkArea` (the type's work area zeroed) | volume sweeps and a pitch from a note (`port_unimplemented`); the order of LIBSPU's own stores |
| LIBAPI `libapi.c` | Events: `OpenEvent`/`CloseEvent`/`EnableEvent`/`DisableEvent`/`TestEvent`/`WaitEvent` over 32 event blocks (handles `0xF1000000 + n`); a delivery runs an `EvMdINTR` handler at once or marks an `EvMdNOINTR` event for `TestEvent`. Root counter 3 (the vblank): `SetRCnt`/`StartRCnt`/`StopRCnt`/`GetRCnt`/`ResetRCnt`; started with `RCntMdINTR`, it delivers `(RCntCNT3, EvSpINT)` once per vsync tick, after the `VSyncCallback` handler (a handler's `port_fiber_preempt` is honoured at the tick's end, docs/PORT.md "Fibers"). `EnterCriticalSection` (1 when interrupts were on) / `ExitCriticalSection` count the nesting for the trace | root counters 0-2 (`port_unimplemented`); `ChangeClearPad` (recorded) |
| LIBCARD `libcard.c` | The memory card through the BIOS, on LIBMCRD's card store: `InitCARD`, `StartCARD`/`StopCARD`, `_bu_init`; `_card_info`/`_card_load` (SwCARD events) and `_card_clear` (HwCARD) complete at the next vsync tick with `EvSpIOE`, `EvSpTIMOUT` (no card), `EvSpNEW` (`_card_info`: the new-card flag, which `_card_clear` or a write clears; `_card_load`: not formatted) or `EvSpERROR`; `_card_format` (1/0), `_card_status`, `_card_wait`, `_card_chan`. The BIOS's file calls on `bu00:`/`bu10:`: `open` (`FREAD`, `FWRITE`, `FCREAT` with the block count in bits 16-31, `FASYNC`; descriptors 2..15), `read`/`write` (whole 128-byte frames inside the file; with `FASYNC` they complete at the next tick with a SwCARD event), `lseek` (SEEK_SET, SEEK_CUR), `close`, `firstfile`/`nextfile` (the BIOS's global search, `?`/`*`), `erase`, `format`. Another device fails (-1: the first game's `sim:`), `cdrom:` stops the run (`port_unimplemented`) | the timing (one tick per command) |
| LIBPRESS `libpress.c`, MDEC `mdec.c` | The movie decoder (session 16, M5; our own, from psx-spx: no FFmpeg): `DecDCTvlc2` expands a .STR version 2 frame (the disc's movies are all v2, 320x416) into the MDEC's run-level words with MPEG-1's AC code table, word for word as LIBPRESS does; `DecDCTin` writes the mode into the command word (bit 0: 24-bit, clears bit 27; bit 1: sets bit 25) and starts the MDEC; `DecDCTout` writes `size` words of pixels: per 16x16 macroblock (Cr, Cb, Y1..Y4) the run-level decode (quantiser 1..63 with LIBPRESS's table, quantiser 0, 11-bit saturation, zig-zag), the IDCT on the scale table, YCbCr to 24-bit or 15-bit RGB; then the `DecDCToutCallback` handler runs, and a `DecDCTout` it makes runs after it returns (a loop, so the handler's `LoadImage` of the column just decoded happens before the next column overwrites its buffer); `DecDCTReset(0)` loads the default tables | `DecDCTvlcBuild` (the decoder has its own table; the game's 0x11000 bytes are left as they are); no DMA timing: a frame decodes inside `DecDCTout` |
| LIBC2 | the host libc (see below) | — |

`port_unimplemented` stops the run where a game reaches what the shim does not model: root counters 0-2, the BIOS's
`cdrom:` device, a volume sweep or a pitch from a note in LIBSPU, a LIBSND tick faster than the vsync, LIBGS's
`GsTMDdiv*` handlers (subdivision) and a `GsFCALL4` entry the program left empty.

### LIBC2, and LIBAPI's names the host has too
`strlen`, `strcpy`, `strncpy`, `memcpy`, `strcspn`, `atoi`, `sprintf`, `memset`, `bzero`, `bcopy`, `toupper`, `rand`
and the other LIBC2 functions are **not** defined by the shim: they resolve to the host libc, whose string functions
are the same functions. The prototype differences in `include/psxstack/psyq/libc2.h` (`s32` returns and lengths where
libc has `size_t`) are harmless on the LP64 ABIs (x86-64, AArch64): the low 32 bits of the return register and a
32-bit length register are what both sides use. **`rand` is the host's**: glibc's (or the Windows CRT's) sequence, not
the PS1's LIBC2 `rand` (a linear congruential generator); a game whose replays depend on `rand` (the second game: 87
calls) needs the PS1's.

LIBAPI's `open`, `read`, `write`, `lseek` and `close` (the BIOS's file calls) and `EnterCriticalSection`/
`ExitCriticalSection` are also names of the host (libc; Win32's kernel32). A definition named `open` in the executable
would replace libc's for every shared library in the process (SDL included) and for the runtime's own files; one named
`EnterCriticalSection` clashes with kernel32's at the Windows link. So the game's units are compiled with
`include/psxstack/psyq_names.h` forced in (`cmake/psxstack.cmake`), whose object-like macros rename those names to
the shim's `psyq_api_*` throughout the unit (calls, prototypes, and also a struct member or variable of the same name,
consistently: the first game's `cdload_reader.read`); `libapi.c` and `libcard.c` include it too, so the stack's
prototypes and their definitions carry Sony's names in the source and the shim's at link time. The first game's
`open("sim:C:\...")` fails (-1) as before: the BIOS has no `sim:` device.

## Tracing
`<PREFIX>_PORT_TRACE=1` (`DW3_PORT_TRACE` for the first game) in the environment (decided at the first stub call), or `psyq_set_trace(1, stream)`, logs one line
per stub call (`psyq: CdControlF 02 param ...`) to stderr or the stream given. Off, each call costs one predictable
branch. The hot paths that run every frame are not traced: `SsSeqCalledTbyT`, `TestEvent` and `StGetNext`/`PadChkVsync`
polls. `DrawOTag` logs the primitive count and the running hash per call. LIBCD logs commands (with only the
parameter bytes each takes), each read's or stream's start (sector, head, seek ticks, speed), the sector range a tick
delivered, and per movie frame its arrival, `StGetNext` and `StFreeRing`; a ring position is an offset in the ring.

## The primitive stream
`DrawOTag`/`ContinueDraw` follow the 24-bit tags (docs/PORT.md "Ordering tables on 64-bit": `(ot & ~0xFFFFFF) + (tag & 0xFFFFFF)`),
only inside the window `psyq_set_arena` gave (by default the runtime's tag window; a link outside
it stops the walk with a trace line). Every primitive's `len` words after its tag go into an FNV-1a hash that
`psyq_gpu_take_hash` returns and resets: the M1 test's "hash of the primitive stream per frame", then to `gpu.c` as
GP0 words.

## Behaviour assumed, to verify against the emulator later
Each file's header comment lists its own; the ones a later milestone must check first:
- **GTE and LIBGTE:** checked against the PS1 (session 16): the layer-1 family `gte` (`tests/golden/families/gte.py`)
  runs MIPS routines in the emulator that load all 64 registers, issue one command and store them back (every
  command with sf/lm 0/1, MVMVA's 64 mx/v/cv combinations, the game's nine command words, the registers' write/read
  rules, MAC 44-bit overflow, saturations, NCLIP/AVSZ limits, RTPS sweeps over every UNR table entry and H >= 2 * SZ3)
  and calls LIBGTE's own `rsin`/`rcos` (a whole turn and outside it), `RotMatrix*_gte`, `ScaleMatrix` and
  `ApplyMatrixSV`, with the GTE state each leaves; `tests/host/gte_replay.py` (run by `tests/host/replay.py`) replays
  all 865 cases through `gte.c`/`libgte.c`: every register and word equal. What the goldens changed: the sine table was
  right; `RotMatrix*_gte` floors each product on its own (it was one exact product, off by one in 80 of 96 cases);
  `ScaleMatrix` writes the pad halfword after `m[2][2]` and multiplies in 32 bits; the MAC accumulator wraps at 44 bits.
  Not covered: the GTE's timing (a command is instant here) and the power-on register values (zero here).
- **LIBGTE for the second game:** not yet checked against the PS1 (its emulator goldens are to come); each function
  issues the commands of the second game's own LIBGTE objects in their order (RTPT/RTPS, NCLIP, AVSZ, MVMVA, NCCS/NCCT,
  SQR, OP, GPF, LZCS/LZCR) with their stores, the commands being `gte.c`'s (checked), and `tests/psyq_test.py` checks
  the documented values and properties. Taken from those objects and kept, although they differ from Sony's
  descriptions: `RotNclip4` returns the FLAG word (not the outer product) for a front face, its return register being
  reused; `RotAverage4`'s FLAG leaves out AVSZ4's; `CompMatrix` cuts `m1`'s translation to 16 bits (the documented
  limit); `VectorNormal`/`MatrixNormal` cut |v|^2 to 8 bits (results up to 1/128 long) and take the components' low
  16 bits; a zero vector puts the square-root table's entry for 202 in IR0 (what the PS1 reads before its 1/sqrt
  table); `PushMatrix` on a full stack and `PopMatrix` on an empty one change nothing (the PS1 prints to its TTY: a
  trace line here). The tables are computed and were checked against the second game's EXE: `RotMatrix`'s sine and
  cosine (rsin/rcos's, all 4096 entries), the 1/sqrt table (floor(32768 / sqrt(i)), 192 entries), `ratan2`'s
  (round(atan((i + 3/8) / 1024) * 2048 / pi), all 1025), `csqrt`'s CORDIC constant (floor(2^22 / Kh^2), Kh the
  hyperbolic CORDIC gain). **`catan` is not exact:** LIBGTE's twelve CORDIC angles follow no formula found (they
  differ from round(atan(2^-i) * 2048 / pi) at four entries), so the shim's catan differs from the PS1's by 1 or 3
  (of 4096 per turn) for every input; copying LIBGTE's twelve numbers would make it exact (an owner's decision: they are
  SDK data). `csqrt` is the PS1's approximation (7 CORDIC steps: within 0.02 %).
- **LIBGPU for the second game:** `StoreImage` writes whole words: with an odd pixel count the last word's high half is
  0 here (the PS1's is not known); `GetDispEnv` returns `PutDispEnv`'s environment as given (the PS1's `PutDispEnv` also
  rewrites `env->pad0` with the video mode, which the shim does not); the `2` transfer forms are the queued ones (both
  complete at once here); `LoadImage`/`StoreImage` clamp the rectangle locally (LIBGPU writes the clamped size back into
  the caller's RECT). `SetDrawEnv`/`PutDrawEnv`'s texture-window word is 0xE2000000 for an empty window whatever its
  x, y (LIBGPU keeps the offset bits; the GPU ignores them under a zero mask, so only the primitive hash differs);
  `SetTexWindow`/`SetDrawMode` keep them as LIBGPU does.
- **LIBGS for the second game:** not yet checked against the PS1 (its emulator goldens are to come); each function makes
  the calls and GTE commands of the second game's LIBGS 4.x objects in their order, with their stores and tests, and
  `tests/psyq_test.py` checks views worked out by hand, a three-level hierarchy with its cache, and a synthetic TMD
  (`GsMapModelingData`, `GsLinkObject4`, `GsSortObject4` with `GsTMDfastF3L`/`GsTMDfastG4L`: the packets, their OT
  entries, the picture; the attribute's choice of `GsFCALL4` entry). Assumed or decided:
  **a mapped TMD's form on the host** (a 32-bit word holds no host address): `GsMapModelingData` sets the flags' bit 0
  as the PS1 does but leaves offsets in the object table, each object's `vert_top`/`normal_top`/`primitive_top`
  relative to that object's own 28-byte entry (object 0's are the file's offsets unchanged; the PS1 writes the
  table's address + offset), and `GsLinkObject4`, `GsSortObject4` and a game's own readers resolve `(u8 *)entry +
  (s32)word`; a TMD that arrives with bit 0 set (PS1 addresses) cannot be used. The handlers do not read the next
  primitive ahead (the PS1's prefetch can read past the last one), and their screen coordinates go through a
  consecutive four-word cache before the packet so that the sub-pixel shadow finds them at the link (the PS1 stores
  them into the packet directly): the packets, OT and GTE registers are the same. `GsLinkObject4`/`GsSortObject4` on
  a mode LIBGS does not draw: the PS1 prints and `GsSortObject4` loops on it for ever, the shim traces and ends the
  object (also on a run of length 0, an object `GsLinkObject4` has not seen). `GsSwapDispBuff`'s buffers are both at
  0, 0 (no `GsDefDispBuff`), and `GsInitGraph`'s display environment is NTSC (the PS1 moves the screen 24 lines down
  under `GetVideoMode() == MODE_PAL`, which the shim does not have). `GsSetAmbient` is declared `void` (the PS1
  returns `SetBackColor`'s leftover register). `GsSetLightMode` with a mode outside 0..3 changes nothing (the PS1
  prints).
- **LIBGPU and the GPU:** checked against the PS1 (session 16): the layer-1 family `gpu`
  (`tests/golden/families/gpu.py`) runs GP0 lists through the game's own `DrawOTag`/`LoadImage`/`MoveImage`/
  `ClearImage(2)` in the emulator and reads the VRAM back with `StoreImage` (every primitive type and mode, random and
  at the edges: shared edges, thin and degenerate triangles, the 1023/511 size limit, 11-bit wrapping, draw areas,
  texture windows, mask bits, every blend mode and depth; probes that pin the rasteriser's rules), and calls
  `SetDrawEnv`, `SetDefDrawEnv`, `SetDrawMove` and `BreakDraw` for their packets and results; `tests/host/gpu_replay.py`
  (run by `tests/host/replay.py`) replays its 715 cases through `libgpu.c`/`gpu.c`. What the goldens changed:
  `SetDrawEnv`'s packet (E3/E4 clamped, E1 and E2 after E5, the E6 word, a TILE rather than a fill for `isbg`; the
  per-frame primitive hashes changed with it), `SetDefDrawEnv`'s `dfe` (`h < 289`), `MoveImage` of an empty rectangle,
  `ClearImage`'s TILE path, `BreakDraw` (NULL while idle: FIGHTSTG's cursor copies are drawn). The known differences
  (`tests/host/known_mismatches.json`, 133 cases, all 1..few pixels or emulator-only behaviour): exact .5
  interpolation ties (the emulator's fixed-point rounding), semi-transparency modes 2/3 of modulated textures (the
  emulator's two-pixels-at-once arithmetic), and three hardware rules kept over the emulator's (fill rounding/wrap,
  copies obey the mask, a 1x1 draw area). Not covered: GP1, interlaced `dfe`, timing.
- **GPU speed (session 16):** `gpu.c` (built at `-O3`) draws `first_battle_save` (41,779 frames, ~180,000 pixels a
  frame) in ~7 s, the whole run ~8.5 s against ~1.6 s without drawing (22 s before): span loops per primitive kind,
  edge walking, sprite rows from decoded texture segments kept by VRAM write stamps, vectorisable select loops
  (`gpu.c`, "Speed"). Every step byte-identical: the 715 `gpu` cases' raw output, both scripts' logs, screenshots and
  the whole VRAM every 250 frames, and a randomized comparison with the unoptimised rasteriser.
- **LIBCD:** verified with the BIN (session 16): mode `0xA0` (cdload) gets the 2340-byte window from the 12-byte
  offset, header + subheader + data, and `cdload_check_sector` accepts every sector (boot, sound banks, CNTY_SEL and
  STDWTITL load at both timings); `CdlReadN` acknowledging with `CdlComplete` before its first sector is the order
  cdload's state machine runs through; several sectors per tick (one handler call each) and a `Pause` issued from the
  ready handler (no later sector delivered) work; the movie stream (mode `0x1E0`) plays MOVIEOPN.STR's 1776 frames
  in order at 15 frames/s in "realistic" timing (5916 vsyncs from frame 1 to 1776 = its real 118.3 s) and ends by
  itself (`stdwtitl_movie_done`). Still assumed: a blocking `CdControl` runs no sync handler; the `0x10` window
  (2328 bytes); the seek times (a deterministic stand-in: 3 ticks + 1 per 8192 sectors, at most 40); what LIBCD does
  when the ring is full ("realistic" drops the frame, "instant" waits); the 5000 `StGetNext` polls per vsync (an
  estimate of the PS1 loop's speed); our ring layout (LIBCD's own is not documented publicly; the game only needs
  `*addr` contiguous and `*header`'s frameCount, width and height).
- **XA audio (M5, T17):** the opening movie played to its end (no START) puts its soundtrack in the port's `--wav` from
  vsync 364 (the first audio sector arrives in tick 362; one tick of the render's pipeline, one of wait) to the `Pause`
  at vsync 6281: 2220 of the file's 2261 audio sectors (118.4 s; the game stops at its last video frame), and the
  left channel is exactly the decoded stream x the CD volume (7FFEh, `SsSetSerialVol`) x the main volume (3FFFh), the
  right one within 1 LSB (the reverb's tail). The emulator's capture of the same script agrees (0.1 s windows from the
  music's start: correlation >= 0.9997, levels 1.000..1.002, no drift). Assumed: when the hardware clears the decoder's
  history and ring (here: at every flush); the one-tick wait (ours: the sectors arrive in whole ticks); what plays
  after a `Pause` (here: nothing beyond the tick already handed to the SPU).
- **LIBPAD (M3):** the mode ids (4 digital, 7 analog), state 6, a digital pad without actuators.
- **LIBAPI (the second game):** from psx-spx and the game's use, not checked against the emulator: the event handles
  (`0xF1000000 + n`, 32 blocks), an `EvMdINTR` event's status staying active while its handler runs, deliveries matching
  class and spec exactly; root counter 3 delivering once per vblank whatever its target (the game sets 1), only with
  `RCntMdINTR`, only once started; where its handler runs relative to the `VSyncCallback` handler (after it, here);
  `EnterCriticalSection`'s return; interrupts are never actually held off (no tick runs inside a critical section unless
  the game waits there, which the trace shows).
- **LIBCARD and the BIOS's file calls (the second game):** the event class per command (the game's waits: SwCARD after
  `_card_info`, `_card_load` and a file transfer, HwCARD after `_card_clear`); `_card_info` answering `EvSpNEW` until
  `_card_clear` or a write, `_card_load` answering `EvSpNEW` for an unformatted card; one tick per command (the card's
  real speed is a few ms per 128-byte frame) and a second command refused while one runs; the return values (`open`'s
  descriptors from 2, an asynchronous `read`/`write` returning the byte count, `close` the descriptor, `_card_status`
  1/0x11); `read`/`write` refusing a range outside the file rather than clipping it; `erase` marking the blocks
  A1/A2/A3; a synchronous transfer delivering no event; `nextfile` continuing the last `firstfile` whatever entry it is
  given. LIBMCRD and LIBCARD share one new-card flag per card (LIBMCRD clears it at its first access, LIBCARD's
  `_card_clear` and writes clear it).
- **LIBCD's high-level calls (the second game):** `CdSearchFile` is instant and leaves the drive as it was (LIBCD reads
  the directories with the drive and caches them; its limits on directory sizes and depth are not modelled), compares
  names exactly; `CdRead` stops the drive after its last sector; `CdSync` with no command pending is `CdlComplete`;
  `CdMix`'s matrix (80h unity, products >> 7, clamped) applies to XA audio only and at once; `StRingStatus`'s and
  `StGetBackloc`'s bookkeeping is ours (the game only stores the first and restarts a stalled stream with the second).
- **LIBSND and LIBSPU (the second game):** `SsStart`'s tick once per vsync for `SS_TICK60`/`SS_TICK50` whatever the
  video rate (a PS1 with another rate may tick otherwise), after the `VSyncCallback` handler; `SsInit` stopping it; a SEQ
  file's header (`pQES`, 4-byte version, resolution, 3-byte tempo, rhythm, then the events); `SsUtKeyOnV` taking the
  voice whatever plays on it (the others aging as at an allocation); `SsSetMono`/`SsSetStereo` affecting the next
  notes and volume changes only; LIBSPU's attribute bits (`SPU_VOICE_*`, `SPU_COMMON_*`), its envelope modes' mapping to
  the ADSR bits, and the order of its stores. The second game's SPU write trace has not been compared with the
  emulator's yet (M3 for that game).
- **LIBMCRD:** checked against the emulator (session 16, `first_battle_save`: `memcard_state` traced each frame): the
  command numbers, results 3 (the first Accept after boot and after the reset), 5 (ReadFile of a missing file) and 0.
  Still assumed: a missing card is result 1 for every command; ReadFile/WriteFile also answer 3 on a new card; the
  results for a transfer outside the file (2) and for CreateFile's errors; `DIRENTRY.attr`/`head` (the game reads
  only `size`).
- **LIBPRESS (M5):** checked against the emulator (session 16): the layer-1 family `mdec` (`tests/golden/families/mdec.py`: DecDCTvlc2 on 5 frames of
  3 movies, equal word for word; DecDCTin's command words; the MDEC on those frames and on made-up run-level data, `tests/host/mdec_replay.py`) and
  the opening movie: the port's VRAM after MOVIEOPN frames 30, 60, 100, 150, 200, 250, 300 against PCSX-Redux's after the same frames (an exec
  breakpoint on `stdwtitl_update_movie`, `PCSX.GPU.getVRAM()`): everything outside the two 24-bit movie buffers equal, inside them 39-46% of the
  bytes differ, by 1-3 (one or two bytes by 4 per frame). That is PCSX-Redux's IDCT and colour conversion, which round otherwise than psx-spx's
  (an exact floating-point IDCT differs from it as much); the port follows psx-spx. Where the two disagree beyond rounding, psx-spx is kept: 0xFE00
  before a block's DC is skipped (the emulator decodes it as a block) and out-of-range colours clamp (the emulator's wrap); neither occurs in the
  movies but for 2 bytes of one frame. 15-bit output rounds to 5 bits as the emulator does (not checked against psx-spx). Still assumed: the
  callback running synchronously (the real MDEC runs it from the DMA's end); DecDCTvlc2 on a damaged stream; the mono (4/8-bit) MDEC modes and
  custom tables, which LIBPRESS's API as the game uses it never selects.

## Checks
`tests/shim_test.py` (CI) compiles `psyq/*.c` with `tests/shim/shim_test.c` alone (no runtime, no game, no disc) and
checks the deterministic pieces: the events and root counter 3 on the tick, LIBCARD and the file calls on a fresh card
image (and LIBMCRD reading it), `CdSearchFile` and `CdRead` over an ISO 9660 image the test builds, LIBSPU's attributes,
LIBSND's SEQ calls and `SsStart`'s tick; `--sanitize`, `--m32`.
`psyq/check.sh` (no CMake, no SDL): compiles every file with the port's flags and `-Wall -Wextra -Werror`,
archives `build/port_psyq/libpsyq.a`, then checks coverage against `tools/port_inventory.py`'s probe objects
(`build/port_inventory/m64/obj`, made by `port_inventory.py probe`): every one of the 123 Psy-Q functions plus the
three data symbols is defined by the shim or by the host libc, no global is defined twice, and what stays undefined
is the game's own asm-only data and the runtime's `port_*`. `EXTRA="-O2 -fsanitize=address,undefined"
psyq/check.sh --compile` compiles with more flags.
