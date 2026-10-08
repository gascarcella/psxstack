#!/usr/bin/env bash
# Installs a pinned PCSX-Redux headless into a directory, for the replay runners (tools/replay/emulator.py). The pin
# is the game's (GAME_CONTRACT.md "6. Tests"): the game's setup script calls this with its own values, and the stack
# downloads nothing by itself. The result: DEST/app/ (the AppImage's contents), DEST/sysroot/ (only when the host
# glibc is too old for the build), the wrapper DEST/pcsx-redux (what the runner executes) and DEST/.sha256.
#
#   tools/replay/redux.sh --dest DIR --sha256 SHA (--zip FILE | --appimage FILE | --url URL --src-dir DIR)
#                         [--sysroot-debs FILE --mirror URL] [--force]
#     --zip FILE          the release zip (distrib.app's PCSX-Redux-<changeset>-linux-x86_64.zip) holding the AppImage
#     --appimage FILE     the AppImage itself
#     --url URL           where to download the zip when neither is given, into --src-dir (kept for the next run)
#     --sha256 SHA        the zip's (or AppImage's) SHA-256; the install is skipped when DEST/.sha256 already says so
#     --sysroot-debs FILE a list of `<pool path> <sha256>` lines (one per Debian package) that make up a runtime
#                         sysroot for a host whose glibc is older than the build's; fetched from --mirror (an Ubuntu
#                         archive) and unpacked with dpkg-deb only when the AppImage does not run natively
#     --force             reinstall even when DEST/.sha256 matches
#   Headless use of the result: DEST/pcsx-redux -no-ui -stdout -testmode -run -iso <cue> -bios <bin> -dofile <lua>
#   Exit 0 installed (or already there); 1 on any failure; 2 usage.
set -euo pipefail

dest= zip= appimage= url= src_dir= sha= debs= mirror= force=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dest) dest="$2"; shift 2 ;;
        --zip) zip="$2"; shift 2 ;;
        --appimage) appimage="$2"; shift 2 ;;
        --url) url="$2"; shift 2 ;;
        --src-dir) src_dir="$2"; shift 2 ;;
        --sha256) sha="$2"; shift 2 ;;
        --sysroot-debs) debs="$2"; shift 2 ;;
        --mirror) mirror="$2"; shift 2 ;;
        --force) force=1; shift ;;
        -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
        *) echo "redux.sh: unknown argument $1" >&2; exit 2 ;;
    esac
done
[[ -n "$dest" && -n "$sha" ]] || { echo "redux.sh: --dest and --sha256 are required" >&2; exit 2; }
[[ -n "$zip" || -n "$appimage" || ( -n "$url" && -n "$src_dir" ) ]] || {
    echo "redux.sh: give --zip FILE, --appimage FILE, or --url URL with --src-dir DIR" >&2; exit 2; }

log() { printf '\033[1;34m[redux]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[redux]\033[0m %s\n' "$*" >&2; exit 1; }
# fetch URL FILE: a download that survives a transient failure (a transfer under 1 KB/s for a minute is given up,
# 5 retries on any error, no connect longer than 30 s, no transfer longer than 10 min).
fetch() {
    curl -fL --retry 5 --retry-all-errors --retry-delay 5 --connect-timeout 30 --max-time 600 \
         --speed-limit 1024 --speed-time 60 -o "$2" "$1"
}

if [[ $force -eq 0 && -x "$dest/pcsx-redux" && -f "$dest/.sha256" && "$(cat "$dest/.sha256")" == "$sha" ]]; then
    log "already installed ($dest/pcsx-redux)"
    exit 0
fi
src=
if [[ -n "$zip" && -f "$zip" ]]; then
    src="$zip"
elif [[ -n "$appimage" && -f "$appimage" ]]; then
    src="$appimage"
elif [[ -n "$url" ]]; then
    name="$(basename "$url")"
    mkdir -p "$src_dir"
    if [[ -f "$src_dir/$name" ]] && echo "$sha  $src_dir/$name" | sha256sum -c --quiet - 2>/dev/null; then
        src="$src_dir/$name"
    else
        log "downloading $name"
        fetch "$url" "$src_dir/$name.part" || { rm -f "$src_dir/$name.part"; die "download failed: $url"; }
        mv "$src_dir/$name.part" "$src_dir/$name"
        src="$src_dir/$name"
    fi
else
    die "no source: $zip$appimage does not exist"
fi
echo "$sha  $src" | sha256sum -c --quiet - || die "checksum mismatch for $src"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
if [[ "$src" == *.zip ]]; then
    log "unpacking $(basename "$src")"
    unzip -q "$src" -d "$work/unzip"
    appimage="$(find "$work/unzip" -name '*.AppImage' | head -1)"
    [[ -n "$appimage" ]] || die "no AppImage found in $src"
else
    appimage="$src"
fi
chmod +x "$appimage"
(cd "$work" && "$appimage" --appimage-extract >/dev/null)   # no FUSE needed
rm -rf "$dest"
mkdir -p "$dest"
mv "$work/squashfs-root" "$dest/app"
[[ -x "$dest/app/usr/bin/pcsx-redux" ]] || die "app/usr/bin/pcsx-redux missing after extraction"
host_glibc="$(ldd --version | head -1 | awk '{print $NF}')"
if (unset DISPLAY WAYLAND_DISPLAY; cd "$dest/app/usr/bin" && ./pcsx-redux -version >/dev/null 2>&1); then
    log "the AppImage runs natively (host glibc $host_glibc)"
elif [[ -n "$debs" ]]; then
    [[ -n "$mirror" ]] || die "host glibc $host_glibc is too old and --sysroot-debs needs --mirror"
    log "host glibc $host_glibc is too old; unpacking the pinned runtime sysroot"
    cache="${src_dir:-$dest}/redux-debs"
    mkdir -p "$cache" "$dest/sysroot"
    while read -r file want; do
        [[ -n "$file" && "$file" != \#* ]] || continue
        deb="$cache/$(basename "$file")"
        if [[ ! -f "$deb" ]] || ! echo "$want  $deb" | sha256sum -c --quiet - 2>/dev/null; then
            fetch "$mirror/$file" "$deb.part" || { rm -f "$deb.part"; die "download failed: $mirror/$file"; }
            mv "$deb.part" "$deb"
        fi
        echo "$want  $deb" | sha256sum -c --quiet - || die "checksum mismatch for $(basename "$file")"
        dpkg-deb -x "$deb" "$dest/sysroot"
    done < "$debs"
    [[ -x "$dest/sysroot/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" ]] || die "the sysroot has no loader"
else
    die "host glibc $host_glibc is too old for this build and no --sysroot-debs was given"
fi
cat > "$dest/pcsx-redux" <<'EOF'
#!/usr/bin/env bash
# Generated by psxstack's tools/replay/redux.sh: runs the pinned PCSX-Redux (app/) through the pinned glibc sysroot
# when the host's glibc is too old for it. Headless use: pcsx-redux -no-ui -stdout -testmode -run -iso <cue> -bios <bin> -dofile <lua>
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -d "$here/sysroot" ]]; then
    exec "$here/sysroot/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" \
        --library-path "$here/sysroot/usr/lib/x86_64-linux-gnu:$here/app/usr/lib" "$here/app/usr/bin/pcsx-redux" "$@"
fi
exec "$here/app/usr/bin/pcsx-redux" "$@"
EOF
chmod +x "$dest/pcsx-redux"
echo "$sha" > "$dest/.sha256"
log "installed to $dest (openbios: app/usr/share/pcsx-redux/resources/openbios.bin)"
