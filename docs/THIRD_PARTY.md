# Third-party material

What psxstack borrows and ships, and what it only uses as a tool.

## Shipped in the repository
- **`include/psxstack/psyq/*.h`**: the Psy-Q 4.7 declarations recovered by
  [dw2003recomp](https://github.com/gascarcella/dw2003recomp) (`include/psyq/`, MIT, the same owner), written from
  the game's use of the API and public documentation, moved here as the stack's own set; the shim and `examples/hello`
  compile against them (DECISIONS "Psy-Q declarations: the stack's").
  No Sony code or SDK file is in either repository.
- **`runtime/`, `psyq/`, `launcher/`, `tools/`**: moved from dw2003recomp's `port/`, `launcher/` and `tools/` (MIT,
  the same owner): our own code, written from public hardware documentation (psx-spx) and the games' use of the API.

## Tools (pinned by `scripts/setup.sh`, never shipped in the repository)
- **SDL3** (zlib): the window, input, audio and SDL_GPU; linked statically into the binaries a game ships.
- **Dear ImGui** (MIT): the launcher's screens; compiled into the launcher.
- **DirectX Shader Compiler** (LLVM Release License / NCSA; Microsoft's parts MIT): compiles the hardware renderer's
  HLSL to SPIR-V (and for Windows to DXIL, signed by its `libdxil.so`) at build time; the output is ours, DXC is not
  shipped.
- **llvm-mingw** (Apache-2.0 with LLVM exceptions; the mingw-w64 runtime under its own permissive licences): the
  Windows cross toolchain; its runtime pieces link statically into the Windows binaries.
- **CMake and Ninja** (BSD-3-Clause / Apache-2.0) from PyPI when a host has none.
