#!/usr/bin/env bash
# Links another checkout's built tools into this one's tools/ instead of building them again: a developer with the
# first game's checkout beside this repo (its scripts/setup.sh built SDL3, Dear ImGui, llvm-mingw, ...) runs
#
#   scripts/dev_link_tools.sh ../dw2003recomp/tools
#
# Every tool directory this repo's .gitignore names (tools/sdl3, tools/imgui, ...) that exists there and not here gets
# a symlink. Nothing is copied or built; scripts/setup.sh still builds whatever is missing. Idempotent.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ $# -eq 1 && -d "$1" ]] || { echo "usage: scripts/dev_link_tools.sh <tools dir of another checkout>" >&2; exit 64; }
src="$(cd "$1" && pwd)"
mkdir -p "$ROOT/tools"
n=0
for name in $(sed -n 's|^tools/||p' "$ROOT/.gitignore"); do
    [[ "$name" != local.env && -e "$src/$name" ]] || continue
    dst="$ROOT/tools/$name"
    if [[ -e "$dst" || -L "$dst" ]]; then
        continue
    fi
    ln -s "$src/$name" "$dst"
    echo "tools/$name -> $src/$name"
    n=$((n + 1))
done
echo "dev_link_tools: $n link(s) made"
