#!/usr/bin/env bash
# Project-local tool setup for psxstack. Idempotent: re-running skips finished steps. Nothing here needs sudo or
# installs outside the repo (into tools/<name>, gitignored).
#
# Usage: scripts/setup.sh [step...]
#        scripts/setup.sh --pins      print the pinned versions, tags, hashes and URLs (CI's tool-cache key)
#   steps (default: sdl3 imgui link):
#   sdl3:   SDL3 built from its pinned source tarball into tools/sdl3 (static); its backends follow the -dev headers
#           present (none: the offscreen driver only, which the self-tests use)
#   sdl3-desktop: the same SDL into tools/sdl3-desktop with the desktop backends REQUIRED (a release's);
#           `scripts/setup.sh --sdl3-desktop-apt` prints the Ubuntu -dev packages it needs
#   imgui:  Dear ImGui at its pinned tag into tools/imgui (the launcher)
#   llvm-mingw: the pinned llvm-mingw release (clang + lld, UCRT) into tools/llvm-mingw: the Windows cross toolchain
#           (cmake/windows-x86_64.cmake)
#   sdl3-windows: SDL3 cross-built static for Windows into tools/sdl3-windows (needs llvm-mingw)
#   dxc:    the pinned DirectX Shader Compiler into tools/dxc: the hardware renderer's shaders (PSXSTACK_SDL)
#   cmake:  CMake and Ninja pip-installed into tools/venv, only when either is missing from PATH
#   link:   in a git worktree, symlink the main checkout's tools into this one
#
# A developer with another checkout's tools (the first game's) links them instead: scripts/dev_link_tools.sh DIR.
# Worktrees: built tools go into the MAIN checkout's tools/ (shared by every worktree); `link` symlinks them here.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAIN="$(dirname "$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)")"
TOOLS="$ROOT/tools"
INSTALL="$MAIN/tools"
SRC="$INSTALL/src"
JOBS="${PSXSTACK_JOBS:-$(nproc)}"
PINS_BEFORE="$(compgen -A variable)"   # `--pins`: every variable defined from here to the steps is a pin (below)

# SDL3 (zlib): one pinned source tarball, SHA-256 checked.
SDL3_VER=3.4.18
SDL3_SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
# The drivers a desktop SDL must have: each one's bootstrap symbol in libSDL3.a.
SDL3_DESKTOP_DRIVERS=(X11_bootstrap Wayland_bootstrap PIPEWIRE_bootstrap PULSEAUDIO_bootstrap ALSA_bootstrap)
# Ubuntu 24.04's -dev packages for them and SDL's other desktop features (XInput2, Xrandr, libdecor, IBus, udev, KMSDRM).
SDL3_DESKTOP_APT="libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev
    libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl1-mesa-dev libgles2-mesa-dev libgl1-mesa-dev
    libdrm-dev libgbm-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libdbus-1-dev libibus-1.0-dev libudev-dev"
# Dear ImGui (MIT): the tag and the commit it must be at.
IMGUI_VER=1.92.9b
IMGUI_COMMIT=f1cc2ae15e53a861a874c3034aae6798fde194ab
# llvm-mingw (Apache-2.0 with LLVM exceptions; the mingw-w64 runtime under its own permissive licences): one pinned
# release tarball for Linux x86_64.
LLVM_MINGW_VER=20260922
LLVM_MINGW_NAME=llvm-mingw-$LLVM_MINGW_VER-ucrt-ubuntu-22.04-x86_64
LLVM_MINGW_SHA256=bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21
# CMake and Ninja from PyPI, for a host without them.
CMAKE_PIP=cmake==3.31.10
NINJA_PIP=ninja==1.13.0

log() { printf '\033[1;34m[setup]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[setup]\033[0m %s\n' "$*" >&2; exit 1; }
# fetch URL FILE: a download that survives a transient failure (retries; a transfer under 1 KB/s for a minute is given
# up; no connect longer than 30 s, no transfer longer than 10 min).
fetch() {
    curl -sSfL --retry 5 --retry-all-errors --retry-delay 5 --connect-timeout 30 --max-time 600 \
        --speed-limit 1024 --speed-time 60 -o "$2" "$1"
}

step_cmake() {
    local venv="$INSTALL/venv"
    if command -v cmake >/dev/null && command -v ninja >/dev/null; then
        log "cmake: cmake and ninja on PATH ($(command -v cmake), $(command -v ninja))"
        return
    fi
    if [[ ! -x "$venv/bin/pip" ]]; then
        log "venv: creating with $(python3 --version)"
        python3 -m venv "$venv"
    fi
    if [[ ! -x "$venv/bin/cmake" || ! -x "$venv/bin/ninja" ]]; then
        log "cmake: installing $CMAKE_PIP $NINJA_PIP into tools/venv (missing from PATH)"
        "$venv/bin/pip" install -q "$CMAKE_PIP" "$NINJA_PIP"
    fi
    export PATH="$PATH:$venv/bin"
    log "cmake: using $(command -v cmake), $(command -v ninja) (tools/venv)"
}

sdl3_fetch() {
    local name="$1" dir="$2" tarball="$SRC/SDL3-$SDL3_VER.tar.gz"
    mkdir -p "$SRC"
    if [[ ! -f "$tarball" ]] || ! echo "$SDL3_SHA256  $tarball" | sha256sum -c --quiet - 2>/dev/null; then
        log "$name: downloading $SDL3_VER"
        fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL3_VER/SDL3-$SDL3_VER.tar.gz" \
            "$tarball.part"
        mv "$tarball.part" "$tarball"
    fi
    echo "$SDL3_SHA256  $tarball" | sha256sum -c --quiet - || die "$name: checksum mismatch"
    rm -rf "$dir"
    mkdir -p "$dir"
    tar -C "$dir" --strip-components=1 -xzf "$tarball"
}

# Prints the drivers of SDL3_DESKTOP_DRIVERS that a libSDL3.a lacks (nothing: a desktop build).
sdl3_missing_drivers() {
    local syms d
    syms="$(nm -g --defined-only "$1" 2>/dev/null)" || { printf '%s\n' "${SDL3_DESKTOP_DRIVERS[@]}"; return; }
    for d in "${SDL3_DESKTOP_DRIVERS[@]}"; do
        grep -q " $d\$" <<<"$syms" || echo "$d"
    done
}

# sdl3_build NAME PREFIX DESKTOP: SDL3 static into PREFIX. SDL stops at an optional dependency whose headers are
# missing and names the option that turns it off; each such feature is turned off in turn and logged (a host with
# neither X11 nor Wayland headers gets SDL_UNIX_CONSOLE_BUILD: offscreen and dummy video only). A desktop build never
# turns off one of its backends: it fails and names the packages.
sdl3_build() {
    local name="$1" prefix="$2" desktop="$3" dir="$SRC/SDL3-$SDL3_VER-$1"
    if [[ -f "$prefix/lib/libSDL3.a" && -f "$prefix/.sha256" && "$(cat "$prefix/.sha256")" == "$SDL3_SHA256" ]] &&
        { [[ $desktop -eq 0 ]] || [[ -z "$(sdl3_missing_drivers "$prefix/lib/libSDL3.a")" ]]; }; then
        log "$name: $SDL3_VER already installed ($prefix)"
        return
    fi
    step_cmake
    sdl3_fetch "$name" "$dir"
    rm -rf "$prefix"
    log "$name: configuring (static; no tests, examples or camera)"
    local opts=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib
                -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_DEPS_SHARED=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF
                -DSDL_EXAMPLES=OFF -DSDL_CAMERA=OFF) off tries=0
    if [[ $desktop -eq 1 ]]; then
        opts+=(-DSDL_X11=ON -DSDL_WAYLAND=ON -DSDL_PIPEWIRE=ON -DSDL_PULSEAUDIO=ON -DSDL_ALSA=ON)
    fi
    until cmake -S "$dir" -B "$dir/build" -G Ninja "${opts[@]}" >"$dir/configure.log" 2>&1; do
        off="$(grep -o -- '-DSDL_[A-Z0-9_]*=OFF' "$dir/configure.log" | head -1)" || off=
        if [[ -z "$off" ]] && grep -q 'X11 or Wayland' "$dir/configure.log"; then
            off=-DSDL_UNIX_CONSOLE_BUILD=ON
        fi
        if [[ $desktop -eq 1 && "$off" =~ ^-DSDL_(X11|WAYLAND|PIPEWIRE|PULSEAUDIO|ALSA|UNIX_CONSOLE_BUILD)= ]]; then
            die "$name: needs ${BASH_REMATCH[1]}'s headers (Ubuntu: apt-get install $(echo $SDL3_DESKTOP_APT));" \
                "see $dir/configure.log"
        fi
        tries=$((tries + 1))
        [[ -n "$off" && $tries -le 30 ]] || die "$name: configure failed (see $dir/configure.log)"
        log "$name: headers missing for an optional feature: $off"
        opts+=("$off")
    done
    grep -E '^--   (Video|Audio|Joystick) drivers:' "$dir/configure.log" | sed "s/^-- */  $name: /" || true
    log "$name: building with $JOBS jobs"
    cmake --build "$dir/build" -j"$JOBS" >/dev/null
    cmake --install "$dir/build" >/dev/null
    rm -rf "$dir"
    [[ -f "$prefix/lib/libSDL3.a" && -f "$prefix/lib/cmake/SDL3/SDL3Config.cmake" ]] || die "$name: install incomplete"
    if [[ $desktop -eq 1 ]]; then
        local missing
        missing="$(sdl3_missing_drivers "$prefix/lib/libSDL3.a" | tr '\n' ' ')"
        [[ -z "$missing" ]] || die "$name: built without ${missing}(Ubuntu: apt-get install $(echo $SDL3_DESKTOP_APT))"
    fi
    echo "$SDL3_SHA256" > "$prefix/.sha256"
    log "$name: installed $SDL3_VER to $prefix"
}
step_sdl3() { sdl3_build sdl3 "$INSTALL/sdl3" 0; }
step_sdl3-desktop() { sdl3_build sdl3-desktop "$INSTALL/sdl3-desktop" 1; }

step_imgui() {
    local dir="$INSTALL/imgui"
    if [[ -f "$dir/imgui.cpp" && -d "$dir/.git" && "$(git -C "$dir" rev-parse HEAD)" == "$IMGUI_COMMIT" ]]; then
        log "imgui: $IMGUI_VER already installed ($dir)"
        return
    fi
    rm -rf "$dir"
    log "imgui: cloning v$IMGUI_VER"
    git -c advice.detachedHead=false clone -q --depth 1 --branch "v$IMGUI_VER" https://github.com/ocornut/imgui.git "$dir"
    [[ "$(git -C "$dir" rev-parse HEAD)" == "$IMGUI_COMMIT" ]] || die "imgui: v$IMGUI_VER is not at $IMGUI_COMMIT"
    [[ -f "$dir/backends/imgui_impl_sdl3.cpp" && -f "$dir/backends/imgui_impl_sdlrenderer3.cpp" ]] ||
        die "imgui: the SDL3 backends are missing"
    log "imgui: installed $IMGUI_VER (${IMGUI_COMMIT:0:12}) to $dir"
}

step_llvm-mingw() {
    local dir="$INSTALL/llvm-mingw" tarball="$SRC/$LLVM_MINGW_NAME.tar.xz" unpack="$SRC/$LLVM_MINGW_NAME"
    if [[ -x "$dir/bin/x86_64-w64-mingw32-clang" && -f "$dir/.sha256" && "$(cat "$dir/.sha256")" == "$LLVM_MINGW_SHA256" ]]; then
        log "llvm-mingw: $LLVM_MINGW_VER already installed ($dir)"
        return
    fi
    [[ "$(uname -s)-$(uname -m)" == Linux-x86_64 ]] || die "llvm-mingw: the pinned tarball is a Linux x86_64 build"
    mkdir -p "$SRC"
    if [[ ! -f "$tarball" ]] || ! echo "$LLVM_MINGW_SHA256  $tarball" | sha256sum -c --quiet - 2>/dev/null; then
        log "llvm-mingw: downloading $LLVM_MINGW_VER (~80 MB)"
        fetch "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW_VER/$LLVM_MINGW_NAME.tar.xz" \
            "$tarball.part"
        mv "$tarball.part" "$tarball"
    fi
    echo "$LLVM_MINGW_SHA256  $tarball" | sha256sum -c --quiet - || die "llvm-mingw: checksum mismatch"
    rm -rf "$dir" "$unpack"
    mkdir -p "$unpack"
    log "llvm-mingw: unpacking"
    tar -C "$unpack" --strip-components=1 -xJf "$tarball"
    [[ -x "$unpack/bin/x86_64-w64-mingw32-clang" ]] || die "llvm-mingw: no x86_64-w64-mingw32-clang in the tarball"
    mv "$unpack" "$dir"
    "$dir/bin/x86_64-w64-mingw32-clang" --version >/dev/null || die "llvm-mingw: the compiler does not run"
    echo "$LLVM_MINGW_SHA256" > "$dir/.sha256"
    log "llvm-mingw: installed $LLVM_MINGW_VER to $dir ($("$dir/bin/x86_64-w64-mingw32-clang" --version | head -1))"
}

step_sdl3-windows() {
    local name=sdl3-windows prefix="$INSTALL/sdl3-windows" dir="$SRC/SDL3-$SDL3_VER-sdl3-windows"
    if [[ -f "$prefix/lib/libSDL3.a" && -f "$prefix/.sha256" && "$(cat "$prefix/.sha256")" == "$SDL3_SHA256" ]]; then
        log "$name: $SDL3_VER already installed ($prefix)"
        return
    fi
    [[ -x "$INSTALL/llvm-mingw/bin/x86_64-w64-mingw32-clang" ]] || step_llvm-mingw
    step_cmake
    sdl3_fetch "$name" "$dir"
    rm -rf "$prefix"
    log "$name: configuring (static, Windows x86_64; no tests, examples or camera)"
    PSXSTACK_LLVM_MINGW="$INSTALL/llvm-mingw" cmake -S "$dir" -B "$dir/build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/windows-x86_64.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_CAMERA=OFF \
        >"$dir/configure.log" 2>&1 || die "$name: configure failed (see $dir/configure.log)"
    grep -E '^--   (Video|Audio|Joystick) drivers:' "$dir/configure.log" | sed "s/^-- */  $name: /" || true
    log "$name: building with $JOBS jobs"
    cmake --build "$dir/build" -j"$JOBS" >"$dir/build.log" 2>&1 || die "$name: build failed (see $dir/build.log)"
    cmake --install "$dir/build" >/dev/null
    rm -rf "$dir"
    [[ -f "$prefix/lib/libSDL3.a" && -f "$prefix/lib/cmake/SDL3/SDL3Config.cmake" ]] || die "$name: install incomplete"
    echo "$SDL3_SHA256" > "$prefix/.sha256"
    log "$name: installed $SDL3_VER to $prefix"
}

# The DirectX Shader Compiler (LLVM Release License and the University of Illinois/NCSA licence; Microsoft's parts MIT)
# for the hardware renderer's shaders (runtime/render_gpu.c): HLSL compiled at build time to SPIR-V for SDL_GPU's Vulkan backend,
# and to DXIL for its D3D12 one (libdxil.so signs DXIL on Linux too, so the Windows cross-build can compile them here).
# Chosen over glslang/shaderc (SPIR-V only) and SDL_shadercross (no releases or tags: a commit built against DXC
# anyway). One pinned release tarball for Linux x86_64, SHA-256 checked (GitHub's published digest), unpacked into
# tools/dxc; its binaries need glibc 2.38 (Ubuntu 24.04, as CI and the release's Docker image). A tool, not shipped:
# the SPIR-V it writes is ours. Optional (not in the default steps): scripts/setup.sh dxc
DXC_VER=v1.9.2602.24
DXC_TARBALL=linux_dxc_2026_05_26.x86_64.tar.gz
DXC_SHA256=928b3e9986d11dc4279050e02340950c29bcbd1e5efb9d3ded9669dade37639d
step_dxc() {
    local dir="$INSTALL/dxc" tarball="$SRC/dxc-$DXC_VER.tar.gz" unpack="$SRC/dxc-$DXC_VER" probe
    if [[ -x "$dir/bin/dxc" && -f "$dir/.sha256" && "$(cat "$dir/.sha256")" == "$DXC_SHA256" ]]; then
        log "dxc: $DXC_VER already installed ($dir)"
        return
    fi
    [[ "$(uname -s)-$(uname -m)" == Linux-x86_64 ]] || die "dxc: the pinned tarball is a Linux x86_64 build"
    mkdir -p "$SRC"
    if [[ ! -f "$tarball" ]] || ! echo "$DXC_SHA256  $tarball" | sha256sum -c --quiet - 2>/dev/null; then
        log "dxc: downloading $DXC_VER (~13 MB)"
        fetch "https://github.com/microsoft/DirectXShaderCompiler/releases/download/$DXC_VER/$DXC_TARBALL" "$tarball.part"
        mv "$tarball.part" "$tarball"
    fi
    echo "$DXC_SHA256  $tarball" | sha256sum -c --quiet - || die "dxc: checksum mismatch"
    rm -rf "$dir" "$unpack"
    mkdir -p "$unpack"
    tar -C "$unpack" -xzf "$tarball"
    [[ -x "$unpack/bin/dxc" && -f "$unpack/lib/libdxcompiler.so" && -f "$unpack/lib/libdxil.so" ]] ||
        die "dxc: bin/dxc, lib/libdxcompiler.so or lib/libdxil.so missing from the tarball"
    # The smoke test: a pixel shader in SDL_GPU's binding layout (fragment textures in space2) to SPIR-V and to DXIL.
    probe="$unpack/probe.hlsl"
    printf 'Texture2D<uint> t : register(t0, space2);\nfloat4 main(float4 p : SV_Position) : SV_Target0 {\n    return float4(t.Load(int3(int2(p.xy), 0)) / 65535.0, 0, 0, 1);\n}\n' > "$probe"
    "$unpack/bin/dxc" -T ps_6_0 -E main -spirv -fspv-target-env=vulkan1.0 -Fo "$unpack/probe.spv" "$probe" >/dev/null ||
        die "dxc: cannot compile to SPIR-V (glibc older than 2.38?)"
    "$unpack/bin/dxc" -T ps_6_0 -E main -Fo "$unpack/probe.dxil" "$probe" >/dev/null || die "dxc: cannot compile to DXIL"
    [[ "$(head -c 4 "$unpack/probe.spv" | od -An -tx4 | tr -d ' ')" == 07230203 ]] || die "dxc: the SPIR-V has no magic number"
    rm -f "$probe" "$unpack/probe.spv" "$unpack/probe.dxil"
    mv "$unpack" "$dir"
    echo "$DXC_SHA256" > "$dir/.sha256"
    log "dxc: installed $DXC_VER to $dir ($("$dir/bin/dxc" --version 2>&1 | head -1))"
}

step_link() {
    [[ "$MAIN" != "$ROOT" ]] || return 0
    local src name dst
    mkdir -p "$TOOLS"
    for src in "$INSTALL"/*; do
        [[ -e "$src" ]] || continue
        name="$(basename "$src")"
        dst="$TOOLS/$name"
        [[ -e "$dst" || -L "$dst" ]] && continue
        if git -C "$ROOT" check-ignore -q "tools/$name"; then
            ln -s "$src" "$dst"
            log "link: tools/$name -> $src"
        fi
    done
}

pins() {
    local v
    for v in $(comm -13 <(sort <<< "$PINS_BEFORE") <(compgen -A variable | sort)); do
        [[ "$v" =~ ^[A-Z][A-Z0-9_]*$ ]] || continue
        case "$v" in PINS_BEFORE|FUNCNAME) continue ;; esac
        declare -p "$v"
    done
}

steps=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pins) pins; exit 0 ;;
        --sdl3-desktop-apt) echo $SDL3_DESKTOP_APT; exit 0 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        -*) die "unknown option: $1" ;;
        *) steps+=("$1"); shift ;;
    esac
done
[[ ${#steps[@]} -gt 0 ]] || steps=(sdl3 imgui link)
mkdir -p "$INSTALL"
for s in "${steps[@]}"; do
    declare -F "step_$s" >/dev/null || die "unknown step: $s"
    "step_$s"
done
log "done"
