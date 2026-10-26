# The launcher

`<id>-launcher` (`dw2003-launcher` for the first game) finds the user's settings, edits `settings.json` and starts the
PC port. The contract with the game (the settings file, the mods' manifests) is `docs/LAUNCHER.md`; what the launcher
knows about a game (its title, executable name, discs, rates, strings) comes from the game's description
(GAME_CONTRACT.md "1. game.json") through `src/brand.h`, never from the sources. It is C++17 with Dear ImGui on SDL3 +
`SDL_Renderer`; the game stays C.

**Status (Linux):** the window and its navigation, the settings directory, reading and writing `settings.json`, the
disc screen (file dialog, drag-and-drop, typed path, the SHA-1 check against the described discs), the play button (the
game's exit status and last lines on an error), the settings screen (video, the rate, renderer, audio, memory cards),
the controls screen (keyboard, gamepad, hotkeys), the mods screen (each mod's page generated from its `mod.json`), the
update check against the game's GitHub releases, the self-test. The code uses SDL's calls, not POSIX, so the same source cross-builds for Windows (a GUI-subsystem
`<id>-launcher.exe` with a manifest, `launcher/windows/`; the self-test passes under Wine); on real Windows it has not
run yet.

## Build

```sh
scripts/setup.sh sdl3 imgui        # SDL3 (static) and Dear ImGui at their pinned versions, into tools/
cmake -S launcher -B build/launcher -G Ninja [-DPSXSTACK_GAME_JSON=/path/to/game.json]
cmake --build build/launcher
build/launcher/<id>-launcher [--config-dir DIR] [--game PATH] [--no-update-check]
```

`PSXSTACK_GAME_JSON` (default `examples/dw2003.game.json`) is read at configure time by `tools/game_gen.py` into
`build/launcher/gen/psxstack_game_gen.h`; a game repo points it at its own description. SDL3 comes from `tools/sdl3`
(or `SDL3_DIR`), Dear ImGui from `tools/imgui` (or `-DPSXSTACK_IMGUI_DIR=...`); `-DPSXSTACK_TOOLS_DIR=DIR` names
another `tools/` first, and in a git worktree the main checkout's is tried too. A developer with the first game's
checkout beside this repo links its built tools instead of building them: `scripts/dev_link_tools.sh
../dw2003recomp/tools`. For a desktop window, SDL3 needs the X11/Wayland `-dev` headers at its build (`scripts/setup.sh
sdl3`); without them only the offscreen driver exists (the self-test's).

Three files of the runtime are compiled into the launcher as they are, never changed for it: `runtime/json.c` (the
strict JSON reader), `runtime/sha1.c` and `runtime/sha256.c` (a downloaded release's check; `tests/sha256_test.py`
tests it on its own).

**Windows:** `cmake -S launcher -B build/launcher-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/windows-x86_64.cmake
-DCMAKE_BUILD_TYPE=Release` after `scripts/setup.sh llvm-mingw sdl3-windows imgui`: `<id>-launcher.exe` and its PDB.

## The settings directory

The first of these that applies:

1. `--config-dir DIR`, else `$<PREFIX>_CONFIG_DIR` (relative to the current directory; created at the first save).
2. A file `portable.txt` beside the launcher: the launcher's own directory (portable mode).
3. A `settings.json` that already exists in the current directory (for local testing; never created there by itself).
4. The per-user directory, `SDL_GetPrefPath("", "<id>")`: `~/.local/share/<id>/` on Linux (`$XDG_DATA_HOME` when
   set), `%APPDATA%\<id>\` on Windows.

The status bar and the System screen show the directory and which rule chose it. The directory holds
`settings.json`, the memory cards and `mods/` (data mods: texture packs; docs/LAUNCHER.md "Data mods").

## settings.json

Schema 1, as settled by the game's side (`docs/LAUNCHER.md` "Settings file": the runtime's `settings.c` reads it, `<id>
--config FILE --print-settings` validates it). The launcher edits these members and **keeps every other one as it was**
(order included): `input`, `mods`, `watchdog`, `video.window` and keys it does not know.

| Member | Type | Default | Meaning |
|---|---|---|---|
| `schema` | number | 1 | The file's version; a newer one than the launcher's is read but never written |
| `disc.path` | string | none | The disc image (`.cue` or `.bin`); the launcher stores it absolute |
| `disc.sha1` | string | none | The SHA-1 the launcher verified for that path (the game checks the disc again itself) |
| `video.scale` | 1-16 | 2 | The window is 320*scale x 240*scale |
| `video.fullscreen` | bool | false | |
| `video.refresh` | 50 or 60 | the game's nominal rate | The nominal rate, or the game's other one (docs/LAUNCHER.md "The rate") |
| `video.renderer` | `"software"` or `"gpu"` | `"software"` | The window's renderer |
| `audio.mute` | bool | false | |
| `memcard1`, `memcard2` | string or null | `"card1.mcd"`, `"card2.mcd"` | The memory cards (created by the game when missing); null: no card |
| `input.keyboard.<button>` | name or list | the game's | Keys (SDL3 scancode names) for a PS1 button |
| `input.gamepad.<button>` | name or list | the game's | Gamepad inputs (docs/LAUNCHER.md "Input bindings": `south`, `dpup`, `lefty-`, ...) |
| `input.hotkeys.<action>` | binding | `pause` P, `fullscreen` F11 | A string or a list of triggers; a trigger is an input or a chord (a list); pads as `"pad:<name>"` |
| `launcher.last_dir` | string | none | The launcher's own state: where the file dialog opens. The game never reads `launcher` |
| `launcher.updates.check`, `.skipped` | bool, string | true, none | The update check at start, and the release tag the user skipped ("Updates") |

**Only the bindings the user changed are written.** A button or action absent from `input` keeps the game's default
(`src/input.cpp` mirrors the runtime's defaults for display), so a default changed in a later game reaches everyone who
never rebound it; **Default** removes the entry again, and an `input` object the launcher emptied is removed.

**Paths in the file are relative to the file's directory** (or absolute). The file is written only when its text
changes, through a temporary file and a rename. An unreadable file is reported, the defaults are used, and the first
save keeps the old file as `settings.json.broken`. A value of the wrong type or out of range is reported and its
default used.

## The disc

The Disc screen shows the description's `disc_hint`, then takes the image three ways: **Choose a file...**
(`SDL_ShowOpenFileDialog`: the XDG portal, else `zenity`, on Linux; when neither is there it says so), **dropping** the
`.cue` or `.bin` on the window, or **typing** its path. The launcher reads the `.cue` itself (the first `FILE` line,
relative to the cue: the rules of the runtime's `disc.c`) and hashes the whole BIN with `runtime/sha1.c` on a worker
thread (~4 s warm, longer from a cold disk), with a progress bar and Cancel. Only a disc the description lists (its
SHA-1) is stored, with its SHA-1; a wrong file is refused, naming the accepted discs, and the previous disc stays. A
stored disc is not hashed again while its SHA-1 is one of the described and its BIN has that disc's size; a disc set by
hand without a SHA-1 is checked at start.

## Starting the game

**Play** is enabled when the disc is verified and the game is found: `--game PATH`, else `$<PREFIX>_GAME`, else `<id>`
beside the launcher, else the development tree's SDL build (`build/port-sdl/<id>` beside `build/launcher/`). Play saves
the settings, runs `<id> --config FILE --print-settings` (the game's own check of the file), then starts `<id> --config
FILE --crash-dir <dir>/crashes` in the settings directory and hides the launcher's window. The game's output (stdout and
stderr) is read without blocking, copied to the launcher's stderr, streamed whole to `<dir>/logs/last-run.log` (the
previous run's kept as `last-run.1.log`), and its last 200 lines kept. When the game ends the window comes back; on an
error status it shows the status in words (1 a fatal error, 4 the watchdog, 64 bad settings, a signal), the crash
report the game wrote (`docs/LAUNCHER.md` "Crash report") and the last 40 lines, with **Copy** (the launcher's and the
game's versions, the command, the result, the report's text, the last lines) and **Open folder**. Closing the launcher
does not end a running game.

A game build from before `--config` or `--crash-dir` (its usage, exit 64, at the probe) is reported as too old.

## Settings and controls

**Video:** the window's scale (1-16, with its size), fullscreen, the scaling, the rate when the description offers
two (the nominal one named by its TV standard; the other shows the description's note), the in-game menu's size.
**Graphics:** the renderer, then the hardware renderer's resolution, sub-pixel mode and filters (greyed out under the
software renderer, with a line that says so). **Audio:** mute. **System:** the two memory card slots (a file name
relative to the settings directory, or no card), the update check when the description names a repository
("Updates"), Add to Steam ("Steam"), and the settings file's location and state.

**Controls**, three tabs: **Keyboard** and **Gamepad** list the 14 PS1 buttons with their inputs as chips (x removes
one, + adds one, Default restores the game's); **Hotkeys** has the port's actions (pause, fullscreen) with their
bindings. **+** opens a prompt that takes the next key (Keyboard), gamepad button, trigger or stick direction past
half way (Gamepad), or, for a hotkey, any of them or a chord (everything held together until all are released; at
most 4 inputs, and 8 triggers a binding: the game's limits). Escape alone, or Cancel, closes it. The prompt takes its
events before ImGui, and gamepad navigation pauses until the gamepad is released, so the press that was bound does
not also click a button. An input used by two buttons (or a button and a single-input hotkey) is marked, with a
tooltip naming the other use. A mod's own bindings are its options (Mods, below). The **Gamepad** tab starts with
**Player 2** (`input.player2`: Off, First gamepad, Second gamepad, by connection order; docs/LAUNCHER.md "Input
bindings"), a line on what the choice does, and the gamepads connected now with the player each is (in the in-game
menu the game's own count; in the launcher the same connection order the game will count). Both players' gamepads use
the tab's map. The tables wrap their chips, so every tab fits the in-game menu over a 640x480 window.

## Mods

The launcher lists the manifests beside the game, `<game dir>/mods/<mod id>/mod.json` (the game's build copies them
there), sorted by name. Each mod has an on/off switch and a page generated from its manifest: its name, version and
description, then its options, the ungrouped ones first and then each `group` under its title. One widget per type:
`bool` a checkbox; `int` and `float` a slider between `min` and `max` (a number field with `step` when either is
missing; floats are rounded to `step`); `enum` a list of the values' labels; `binding` the chips and the prompt of the
hotkeys (any key, gamepad input or chord). An option's `description` is its tooltip; `applies: restart` is marked. A
manifest that cannot be used (not JSON, another schema, an `id` that is not its directory's name, a bad option, an
unknown `kind`) is listed with the reason and cannot be switched on.

The settings directory's `mods/` holds the user's data mods (docs/LAUNCHER.md "Data mods": texture packs). They are
listed after the built-in mods under "Data mods", in their priority order; a data mod's page shows what it is, its
priority with **Earlier** and **Later** (which write `mod_order`, the whole list) and its directory. The game gets
`--mods-dir <settings dir>/mods`.

In `settings.json` (`mods.<mod id>`, docs/LAUNCHER.md "Mods section"): `"enabled": true` when on; an option is written
only when it differs from the manifest's default (setting it back, or **Default**, removes it); a mod turned off keeps
its changed options with `"enabled": false`; a mod with nothing left is removed, and so is an empty `mods`. Unknown mods
and options are kept. A stored value the manifest rejects is reported on the mod's page, and its default shown. The
mods' bindings take part in the conflict marks (a key both on a PS1 button and a mod's hold, say).

## Updates

When the description names the game's GitHub repository (`launcher.updates.github`), the launcher asks GitHub for its
latest release at start, on a worker thread (`src/update.cpp`; docs/LAUNCHER.md "Updates"), through the system's
`curl` run as a child process (`src/http.cpp`: Linux and Windows alike; Windows 10 1803 and later ship `curl.exe`).
A release newer than the launcher's version stamp (an exact `X.Y.Z`: a development build is only told the latest)
brings up a prompt over the Play screen, never while the game runs: **Update now** (the default), **Release page**
(the browser), **Later** (Escape), **Skip this version** (`launcher.updates.skipped`: not offered again, a newer one
is). The System screen has the checkbox "Check for updates when the launcher starts", the launcher's version, the
last check's result, **Update now**, **Check now** (whatever the checkbox says; it only reports) and **Release page**.
`--no-update-check` skips the check at start for one run. Without curl the check says so, and **Release page** opens
the repository's latest release. `$<PREFIX>_LAUNCHER_FAKE_CURL` names a program to run in curl's place (the
self-test's stand-in).

**Update now** (`src/app.cpp` over `src/update_apply.cpp`; docs/LAUNCHER.md "Update now", "Applying one") is offered
when the install can update itself: on Linux an AppImage (`$APPIMAGE`) in a writable folder, on Windows the release's
folder, writable, with Windows' `tar.exe`; otherwise a line says why and Release page is the action. It downloads this
platform's package into the settings directory with a progress bar and Cancel, checks its SHA-256 against the digest
GitHub gives for the asset (none given: nothing is downloaded), swaps it in (the AppImage renamed over the old one; the
folder's files moved in, the running launcher renamed to `.old` and removed at the next start), starts the new launcher
with the same arguments and quits. A failure leaves the install as it was, removes the download and shows why, with
Release page and Close.

To try the whole flow against the real releases: build with an older stamp (`PSXSTACK_VERSION=v0.1.0 cmake --build
...`), copy the launcher into a scratch folder as `<id>-0.1.0-x86_64.AppImage` and start it there with
`APPIMAGE=<that file> <PREFIX>_LAUNCHER_UPDATE_NOW=1 SDL_VIDEO_DRIVER=offscreen ... --config-dir <scratch>/settings`:
the check at start answers its prompt with Update now, and the log says each step. The new launcher runs detached with
no output (and offscreen it stays up: stop it).

## Keys

The mouse, the keyboard (arrows, Space/Enter, Escape) and a gamepad (ImGui's navigation) all work. Ctrl+PageDown /
Ctrl+PageUp, or the gamepad's R1 / L1, go to the next / previous screen. A first run (no disc set, or the disc gone)
starts on the Disc screen.

## The self-test

```sh
SDL_VIDEO_DRIVER=offscreen build/launcher/<id>-launcher --self-test DIR    # exit 0 = passed, 1 = failed
```

(`<PREFIX>_SELFTEST_VIDEO_DRIVER=offscreen` does the same through SDL's hint, for the Windows build under Wine, which
drops `SDL_VIDEO_DRIVER` from the program's environment.)

No disc and no display needed (CI runs it). It replaces `DIR/launcher-self-test/` and checks: the path helpers (Windows
forms too), the JSON writer, the lookup order, the settings file's round trips (the documented example with its unknown
members kept, invalid values, a broken file, a newer schema; `video.scene_2d` for a game with scenes, left as it is by
one without), the binding grammar, `input.player2`, and the input prompt
(keys, gamepad buttons, stick directions, chords, Escape), the `.cue` reader and the SHA-1 check (against the described
discs), the launch path with **the launcher itself as the game's stand-in** (`<PREFIX>_LAUNCHER_FAKE_GAME=mode`: a game
with and without `--config`, one that rejects the file, one that crashes, one that fails after 250 lines), then opens
the window, walks every screen with injected key events and a virtual gamepad, plays with the stand-in (a file of the
first described disc's size stands for the verified disc), rebinds a key through the prompt and checks the file, shows
the Gamepad tab with player 2 set and two virtual gamepads connected, the GPU renderer's rows (for a game with scenes
the two Filter rows, the 2D one marked as the in-game menu marks the set on screen), drops a wrong file on the window,
renders the mods screen over the fixture manifests (`tests/fixtures/mods/`: the first game's `fast_forward` as shipped,
the documented `skip_dialogues` example, one with every option type, one with presets and a slider whose toggle makes
it a typed number; three unusable ones written at test time) and rebinds a mod's key through the prompt, and saves a picture of
each screen in `DIR/launcher-self-test/screens/`. The update check never touches the network:
`<PREFIX>_LAUNCHER_FAKE_CURL` points at **the launcher itself as curl's stand-in** (it answers with a file, in pieces
for a download, or with curl's errors; `<PREFIX>_SELFTEST_CURL_REPLY` says which), and every check of the self-test goes
to it. It checks the version rules (only an exact `X.Y.Z` is offered an update), GitHub's answer
(`tests/fixtures/updates/`: the tag, the page, this platform's package with its size and SHA-256; a tag that is not a
version, a cut answer), `launcher.updates` in the file, `src/http.cpp` (a GET, curl's own errors, curl missing, a
download with its progress, a cancel, a wrong size), then in the window: the prompt over Play (a picture), Escape as
Later, the Release page, Skip and a newer release after it, the setting off with Check now, `--no-update-check`, the
prompt waiting for a running game, curl missing, a cut answer, a development build and a description without a
repository (pictures of the System screen's part). Then **Update now** in the window, the stand-in "downloading"
fixture packages (`<PREFIX>_SELFTEST_CURL_DOWNLOAD` is its answer for a download, the release's JSON being the check's):
the line instead of the button when the plan is not possible (no `$APPIMAGE`), the progress and Cancel on a slow
download (no check started meanwhile, the file removed, the install unchanged), a wrong digest and a missing one
(nothing applied, the message, Release page still works), the whole flow into a fake AppImage install (`$APPIMAGE` at a
copy of the launcher; Enter presses Update now) and into a fake Windows folder (a copy of the launcher, the release's
zip made with the self-update's tar), each ending with the new launcher (a marked copy) started with the arguments given
and this one quitting, and the relaunched stamp not prompted again (pictures of the prompt's steps).

The Steam shortcut (`selftest_steam.cpp`, in `DIR/launcher-self-test/steam/`; never this system's Steam): the fixture
`shortcuts.vdf` read and written back byte for byte, the launcher's shortcut added (the bytes of
`shortcuts_added.vdf`), added again (replaced), removed (the fixture again), another entry removed (the rest numbered
again, unchanged), damaged files refused (cut short, bytes after the end, an unknown type, empty) and left as they
are, the save's backup (once, as the file was), `loginusers.vdf` and the text form's comments, escapes and errors,
and Steam's folders and accounts in a fixture home (a link to a folder counted once, Flatpak's, the most recent account
first; Windows' order on the same folders); on Windows, the registry reader on a value of its own; a broken
`launcher.steam` kept; the process list (this launcher found by its name). Then the System screen's section in the
window, with a fixture lookup, a running check and launcher paths of its own: nothing looked up before Add to Steam,
a running Steam refusing every button, Add, the next start, a moved launcher's Update shortcut, Remove (the file as it
was before Add), and the not-found, already-removed and damaged-file cases, with a picture of each state; the artwork
written by appid (the icon the shortcut's), moved by Update and removed by Remove. The self-test runs for a game that
describes no disc too (its play path skipped): CI runs it for `examples/hello`, whose artwork is embedded.

The self-update (`selftest_update.cpp`, in `DIR/launcher-self-test/update/`, no network): SHA-256 of fixture files (the
FIPS 180-4 examples) and the digest check (right, capitals, wrong, malformed, missing file); the plan's answers (no
`$APPIMAGE`, an image that is gone, a folder without the launcher, no tar, a tar path that names nothing, a read-only
folder, the PATH search over an empty directory); the AppImage swap with a copy of the launcher as the image and as the
new one (a wrong digest and a folder turned read-only change nothing; then the new image is in place and runs, which it
can only if it was made executable); the Windows folder swap on a fake install from a zip (on Linux too: the same file
operations with the Linux names), where a wrong digest, a file tar cannot read, a zip without the game, a missing tar, a
file where the release has a folder and a move that fails halfway (POSIX: a read-only subfolder) each leave the install
byte for byte as it was, then the swap (the release's files in, the user's files kept, the old launcher as `.old`) and
the cleanup; and **the running launcher**: a copy of the launcher in a fixture folder applies a release to its own folder
(its running executable renamed to `.old`), starts the new one and exits, and the new one removes the `.old` at its
start. The zips are made and read by `$<PREFIX>_SELFTEST_TAR` (CI's Linux job: bsdtar), else bsdtar on PATH or
Windows' tar.exe, else the launcher itself as a stand-in tar (stored zips): Wine has no tar.exe, and a Unix bsdtar
started from Wine is neither waited for nor gives its status, so under Wine the folder swap runs with the stand-in.
The read-only cases are POSIX only (a Windows folder's read-only flag does not stop writes).

Optional, with a game and its disc: `<PREFIX>_SELFTEST_DISC=path/to/game.cue` also checks the real disc;
`<PREFIX>_SELFTEST_GAME=path/to/<id>` also probes the real game and, with the disc, runs it 300 frames from the
launcher's command (offscreen, unthrottled), which must end with status 0 and the disc checked. The file it probes has
rebinds, a chord, an unbound hotkey and every mod beside that game switched on with every option changed: the game's
own parser must accept it. `<PREFIX>_SELFTEST_GAME=beside` takes the game the launcher finds by itself beside its own
executable (it must be there, with at least one mod manifest beside it): a release's smoke test runs it inside the
package.

## Releases

A game's players' build is one package with the launcher, the game and the mods (the game's packaging: dw2003recomp's
`scripts/package_appimage.sh` and `package_windows.sh`). Its launcher is built with `-DCMAKE_BUILD_TYPE=Release -DPSXSTACK_LAUNCHER_STATIC_RUNTIME=ON` (libstdc++ and
libgcc static) against an SDL3 with the desktop backends (`scripts/setup.sh sdl3-desktop`).

## Files

| File | Contents |
|---|---|
| `src/main.cpp` | Options, the main loop |
| `src/brand.h` | The game's identity, from the generated `psxstack_game_gen.h` (the only place the sources touch it) |
| `src/app.cpp`, `app.h` | The window, the Play and Disc screens, the navigation |
| `src/settings.cpp`, `settings.h` | The settings directory's lookup, the settings file |
| `src/json_value.cpp`, `json_value.h` | An editable JSON tree over the runtime's reader, and a writer |
| `src/disc.cpp`, `disc.h` | The `.cue` reader, the described discs, and the SHA-1 check on a thread |
| `src/http.cpp`, `http.h` | HTTP through the system's curl as a child process: a GET, a download with progress and cancel |
| `src/update.cpp`, `update.h` | The update check: the release query, the version rules, the package's lookup |
| `src/game.cpp`, `game.h` | Finding the game, its `--print-settings` probe, the command, the running game |
| `src/input.cpp`, `input.h` | The PS1 buttons, the game's default bindings, the binding grammar, the input prompt |
| `src/mods.cpp`, `mods.h` | The manifests (reading, checking), the user's values in `mods.<mod id>` |
| `src/paths.cpp`, `paths.h` | Paths and files through SDL's calls only (no POSIX; `/` and `\` and drive letters alike) |
| `src/update_apply.cpp`, `update_apply.h` | A downloaded release applied: the plan, the SHA-256 check, the AppImage and Windows folder swaps, the relaunch, the cleanup at start (docs/LAUNCHER.md "Applying one") |
| `src/selftest.cpp`, `selftest.h` | The self-test; `tests/fixtures/mods/` its manifests and `tests/fixtures/updates/` GitHub's answers, compiled in by CMake |
| `src/selftest_update.cpp` | The self-test's part for `update_apply`, its child processes and its stand-in tar |
| `src/vdf.cpp`, `vdf.h` | Valve's KeyValues files: the binary form read and written back byte for byte, the text form read |
| `src/steam.cpp`, `steam.h` | The Steam shortcut: where Steam is, its accounts, the shortcut added to and removed from `shortcuts.vdf` (docs/LAUNCHER.md "Steam") |
| `src/steam_art.cpp` | The game's Steam artwork, embedded (`gen/steam_art.h`, from game.json `launcher.steam_art`) |
| `src/steam_section.cpp`, `steam_section.h` | The System screen's Steam section: Add to Steam, Remove from Steam, Update shortcut, the running-Steam check, `launcher.steam` |
| `src/selftest_steam.cpp` | The self-test's part for the Steam shortcut, on `tests/fixtures/steam/` and fixture folders, the section in the window included |
| `ui/screens.cpp`, `screens.h` | The Video, Graphics, Audio, Controls, Mods and System screens and the input prompt, drawn by the window and by the game's in-game menu |
| `ui/widgets.cpp`, `widgets.h`, `ui/style.cpp`, `style.h` | Small shared pieces and the look |
| `ui/menu.cpp` | The in-game menu (`include/psxstack/menu.h`): compiled into a game built with `PSXSTACK_MENU`, never into the launcher; docs/LAUNCHER.md "In-game menu" |
| `windows/launcher.rc.in`, `launcher.manifest.in` | The Windows resources, configured for the game's id |
