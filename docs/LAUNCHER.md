# The launcher, the settings file and the mods

How the launcher (`launcher/`) and a game's PC port (the stack's runtime plus the game's adapter) talk to each other,
the settings file they share, the mod manifests and the mod runtime. Building and running the launcher, its screens and
its self-test: `launcher/README.md`. The game is `<id>` throughout (the description's `id`, GAME_CONTRACT.md "1.
game.json": `dw2003` for the first game); `<PREFIX>` is its `env_prefix`. What each of a game's built-in mods does is
the game's documentation, not the stack's.

Code cites this file as `docs/LAUNCHER.md "<heading>"`; keep the headings stable.

## Contract

- The game takes **one option for it, `--config FILE`** (JSON, "Settings file"). Command-line options override the
  file.
- **The game never loads a configuration on its own.** Without `--config` it behaves as the bare binary always did:
  the tests and CI run it that way and must not depend on the machine.
- The launcher edits that file and starts `<id> --config <dir>/settings.json --crash-dir <dir>/crashes --mods-dir
  <dir>/mods` in the settings directory (`SDL_CreateProcess`). Before that it runs `<id> --config FILE --mods-dir
  <dir>/mods --print-settings`, the game's own check of the file (a game without `--config`, `--crash-dir` or
  `--mods-dir` in its usage is too old for the launcher). The game stays startable without the launcher (`--config`, or plain options).
- A game started with `--config` has, by default: a window, memory card files beside the settings file, and the
  watchdog off.
- The disc's SHA-1 check stays the game's (the runtime's `disc.c`, `sha1.c`), against the discs the description lists.
  The launcher compiles `runtime/sha1.c` and `runtime/json.c` in as they are, to verify the disc before storing it and
  to read JSON with the same parser, and `runtime/sha256.c` to verify a downloaded release ("Applying one").
- Exit statuses the launcher reports: 0 normal, 1 a fatal error, 4 the watchdog, 64 bad options or settings, a
  signal; on Windows an unhandled exception's code instead of a signal (`0xC0000005` an access violation, ..., named
  by `game_exit_text`), and `abort()` is status 3 there too. On any of them but 0 the game may have written a crash
  report ("Crash report").
- The launcher's screens use the same SDL3 and Dear ImGui as the game's **in-game menu** ("In-game menu"): a game
  built with `PSXSTACK_MENU` draws the launcher's UI (`launcher/ui/`) inside its own window while it is paused, so
  on a handheld the launcher's window is needed once, for the disc.

## Settings directory

The launcher picks one directory, the first of these that applies (`launcher/src/settings.cpp`):

1. `--config-dir DIR`, else `$<PREFIX>_CONFIG_DIR` (relative to the current directory; created at the first save).
2. A file `portable.txt` beside the launcher's executable: that directory (portable mode).
3. A `settings.json` that already exists in the current directory (local testing; never created there implicitly).
4. The per-user directory, `SDL_GetPrefPath("", "<id>")`: `~/.local/share/<id>/` on Linux (`$XDG_DATA_HOME` when
   set), `%APPDATA%\<id>\` on Windows.

The directory holds `settings.json`, the memory cards, `logs/` (the game's output: `last-run.log`, the previous run's
as `last-run.1.log`), `crashes/` (the game's crash reports) and `mods/` (the user's data mods, "Data mods"). The launcher shows which directory it
chose and why. The game itself never looks for a settings directory: it reads only the file `--config` names.

## Settings file

Schema 1, read by the runtime's `settings.c` (the top level), `input.c` (`input`) and `mods.c` (`mods`).
`<id> --config FILE --print-settings` prints the effective settings (the file, then the command line) with every key
and absolute paths and exits 0, or exits 64 naming the bad key.

```json
{
  "schema": 1,
  "disc": { "path": "game.cue", "sha1": "457cb233..." },
  "video": { "window": true, "scale": 2, "fullscreen": false, "refresh": 50, "renderer": "software" },
  "audio": { "mute": false },
  "memcard1": "card1.mcd",
  "memcard2": "card2.mcd",
  "watchdog": 0,
  "input": {
    "keyboard": { "cross": "X", "start": ["Return", "Keypad Enter"] },
    "gamepad":  { "cross": "south", "up": ["dpup", "lefty-"] },
    "hotkeys":  { "pause": "P", "fullscreen": "F11", "save_state": "F5", "load_state": "F9" }
  },
  "mods": {
    "fast_forward":   { "enabled": true, "hold": "Tab" },
    "skip_dialogues": { "enabled": false, "toggle": "F2", "fast_forward_waits": false }
  },
  "launcher": { }
}
```

### General rules

- **Every key but `schema` is optional;** an absent key keeps its default. `schema` must be 1: a missing one, or any
  other number, ends the game with status 64 (a higher one is named "a newer launcher's").
- **Paths** (`disc.path`, `memcard1`, `memcard2`) are relative to the settings file's directory, or absolute.
- **Errors:** a value of the wrong type or out of range ends the game with status 64 and a message naming the key
  (`port: settings FILE: video.scale: an integer from 1 to 16, not 17`), as a bad option does. **An unknown key is
  logged and ignored** (at every level: top, `disc`, `video`, `audio`, `input`, a PS1 button, a hotkey action, a mod,
  a mod's option), so an older game runs a newer launcher's file.
- **The command line overrides the file:** `--disc`, `--scale`, `--fullscreen`, `--window`, `--mute`,
  `--memcard1|2 PATH|none`, `--watchdog`, `--refresh`, `--fps`.

### Members

| Member | Type | Default | Meaning |
|---|---|---|---|
| `schema` | number | required | Must be 1 |
| `disc.path` | string | none | The disc image (`.cue` or `.bin`). The launcher stores it absolute |
| `disc.sha1` | string | none | The launcher's record of the SHA-1 it verified. The game does not trust it: it checks the disc itself (`disc.c`, with its stamp cache) |
| `video.window` | bool | true | false runs headless (tests, a check run) |
| `video.scale` | integer 1-16 | 2 | The window is 320*scale x 240*scale |
| `video.fullscreen` | bool | false | |
| `video.scaling` | `"fit"`, `"integer"` or `"stretch"` | `"fit"` | The picture's size in the window or on the screen (fullscreen): as large as it allows at the picture's aspect; as tall as a whole multiple of the game's lines allows (every line the same height, black bars around it); or the whole of it, out of shape. Applied live from the in-game menu. The launcher writes it once chosen. A game built before it logs the key and ignores it (it scales by whole multiples) |
| `video.refresh` | 50 or 60 | the game's nominal rate | The nominal rate, or the game's other one ("The rate") |
| `video.renderer` | `"software"` or `"gpu"` | `"software"` | The window's renderer: SDL_Renderer, or the hardware renderer (SDL_GPU; it falls back to software when no device can present). The launcher's "Renderer" choice. A game built before it logs the key and ignores it |
| `video.internal_scale` | integer 1-8 | 1 | The hardware renderer's resolution, N times the PS1's (1: the software picture). The launcher's "Resolution" slider, shown for the GPU renderer |
| `video.subpixel` | `"off"`, `"on"` or `"perspective"` | `"on"` | Above internal scale 1, the hardware renderer draws the 3D at the GTE's sub-pixel positions (`docs/PORT.md` "Sub-pixel precision") instead of the PS1's whole pixels; `"perspective"` also textures it perspective-correct (not the PS1's look: its affine textures swim on large polygons). The launcher's "3D vertices" choice, shown for the GPU renderer above x1. A game built before it logs the key and ignores it |
| `video.filter` | `"none"`, `"sharp"`, `"scanlines"`, `"crt"` or `"smooth"` | `"none"` | The hardware renderer's present filter (docs/PORT.md "Rendering"): `none` shows the picture's pixels as whole blocks, `sharp` is sharp bilinear, `scanlines` sharp with dark lines, `crt` a TV, `smooth` xBR on the 1x picture (the internal scale does not apply to it). The software renderer shows the picture unfiltered and logs it. The launcher's "Filter" combo, shown for the GPU renderer; it writes the member only once it is set |
| `video.crt` | `{ "scanlines", "mask", "curvature" }`, integers 0-100 | 50, 30, 0 | The filters' parameters: `scanlines` (the lines' strength) for `scanlines` and `crt`, `mask` (the aperture grille) and `curvature` for `crt`. The launcher's sliders under the Filter combo; written once set, as `video.filter` |
| `video.menu_size` | `"smaller"`, `"auto"` or `"larger"` | `"auto"` | The in-game menu's size ("In-game menu"): its fit to the window, a step smaller or larger. The Settings screen's "Menu size"; written once set. The menu reads it, the runtime does not (`--print-settings` leaves it out); a game built before it logs the key and ignores it |
| `audio.mute` | bool | false | No audio device (switched off in the in-game menu: the device opens then) |
| `memcard1`, `memcard2` | string or `null` | `"card1.mcd"`, `"card2.mcd"` | A `.mcd` image, created formatted when missing; `null`: no card in that slot. An empty string is an error |
| `watchdog` | integer 0-3600 | 0 | Seconds without a vsync before the game exits 4; 0 is off. The bare binary (no `--config`) keeps its 10 s |
| `input` | object | the game's | "Input bindings" |
| `mods` | object | every mod off | "Mods section" |
| `mod_order` | list of strings | by id | The data mods' ids by priority, the first winning ("Data mods"). The launcher's Earlier/Later buttons write the whole list. Ids of data mods not found are kept |
| `launcher` | object | none | The launcher's own state (e.g. `last_dir`, where its file dialog opens). The game never reads it and `--print-settings` prints it back unchanged |
| `launcher.updates` | `{ "check": bool, "skipped": string }` | `check` true | The update check ("Updates"): whether it runs at the launcher's start, and the release tag the user skipped (`"v0.5.0"`). Written once it differs from the defaults |
| `launcher.steam` | `{ "dir", "account", "account_name", "appid", "exe", "name", "art" }` | none | The Steam shortcut ("Steam"): the Steam folder and account it was added to (and the account's name then), its appid, the launcher path and name it was made with, and the pictures' file names written beside it (`art`, absent when none). Written by Add to Steam, removed by Remove; Update shortcut and Remove act on it without looking for Steam again. One that does not read is warned about, ignored and left in the file |

### Input bindings

`input` has four members; `--print-settings` prints it back as it was given.

- **`keyboard`**: PS1 button -> one key name or a list of them. **`gamepad`**: PS1 button -> one gamepad input name
  or a list. A button named replaces all of that button's entries of that kind; a button that is absent keeps its
  defaults; `""` or `[]` unbinds it. Buttons: `up down left right cross circle square triangle start select l1 r1 l2
  r2`. An unknown button is logged and ignored.
- **Key names are SDL3's scancode names** (`SDL_GetScancodeName`/`SDL_GetScancodeFromName`: `"X"`, `"Return"`,
  `"Keypad Enter"`, `"Right Shift"`, `"F11"`, `"Tab"`). Scancodes name the key's place, whatever the layout. A
  headless build (no SDL) checks only that a name is non-empty.
- **Gamepad input names are ours** (SDL's positional buttons): `south east west north back guide start leftstick
  rightstick leftshoulder rightshoulder dpup dpdown dpleft dpright misc1 paddle1 paddle2 paddle3 paddle4 touchpad`,
  and the axes past half way: `lefttrigger righttrigger leftx- leftx+ lefty- lefty+ rightx- rightx+ righty- righty+`
  (`lefty-` is the left stick up). Every connected gamepad is ORed into pad 1, unless it is player 2's.
- **`player2`**: `"off"` (the default), `"first"` or `"second"`: the gamepad, by connection order, that is player 2.
  It drives pad 2 (the PS1's port 1) with the `gamepad` map; port 1 has a pad in it only while that gamepad is there
  (a game that reads port 0 alone is unaffected); the keyboard and every other gamepad stay player 1's, and player 2's inputs never
  fire a hotkey. `"first"` suits a player 1 on the keyboard. A gamepad unplugged and plugged in again counts as the
  newest. Anything else ends the game with status 64.
- **Default map** (the runtime's `input.c`): arrows the D-pad, `X` cross, `C` circle, `Z` square, `S` triangle,
  `Return` and `Keypad Enter` START, `Backspace` and `Right Shift` SELECT, `Q`/`E` L1/R1, `1`/`3` L2/R2; gamepads:
  south cross, east circle, west square, north triangle, back SELECT, start START, the shoulders L1/R1, the triggers
  L2/R2, the D-pad and the left stick the D-pad. The launcher mirrors these defaults for display
  (`launcher/src/input.cpp`).
- **`hotkeys`**: the port's own actions -> a binding. Actions: `pause` (default `"P"`), `fullscreen` (default
  `"F11"`), `save_state` (default `"F5"`: the whole game to the quick slot, `quick.state` beside this file),
  `load_state` (default `"F9"`: on from the slot; docs/RUNTIME.md "Save states") and `menu` (default `["Escape",
  ["pad:back", "pad:start"]]`: the in-game menu, "In-game menu"; in a build without it, a plain pause). A mod's
  bindings are its options, under `mods.<id>`, not here.
- **A binding** is a string or a list; each element is one trigger, any of which fires the action (at most 8). A
  trigger is one input (a string) or a chord (a list of 1 to 4 inputs, all held). An input is a key name, or `"pad:"`
  + a gamepad input name. `""` or `[]`: unbound. Examples: `"F2"`; `["F2", "pad:guide"]`;
  `[["pad:guide", "pad:south"], "Tab"]`.
- **Hotkeys never reach the pad.** A trigger that completes latches until all its inputs are released, and its inputs
  are masked out of the pad while latched: a single-key hotkey is never seen by the game. The first input of a chord
  made only of game buttons does reach the pad until the chord is complete, so chords should start with an input the
  pad map does not use (`pad:guide`, the stick clicks).
- **The pause** (the runtime's `pump.c`) takes effect at the end of the vsync where its key is pressed: the window
  keeps polling and presenting the last image, the audio device is paused, the watchdog is re-armed, and no vsync runs
  (so nothing reaches the game, the log or the record). The key again resumes, with the pace's schedule started over.
- With `--script` the script owns the pad: the keyboard and gamepads are read but never sent to the game (the hotkeys
  still act). `--input-test` tests the active map: the defaults without `--config`, the settings' with it.

### Mods section

`mods`: `<mod id>` -> `{ "enabled": bool, "<option id>": value }`, with the option ids and types of the mod's manifest
("Mod manifest").

- **A mod that is absent, or has no `enabled`, is off.** An absent option keeps the manifest's default.
- An unknown mod id ("no such mod in this build") is logged and ignored, an unknown option too. A value of the wrong
  type or range ends the game with status 64, as elsewhere.
- `--print-settings` prints `mods` resolved: every mod of the game with `enabled` and every option (defaults filled
  in), then the unknown mods as they were given.
- **Under `--script` every mod is off** unless `--script-mods` is given ("Mod runtime").
- The launcher writes `"enabled": true` for a mod switched on, writes an option only when it differs from the
  manifest's default, keeps a switched-off mod's changed options with `"enabled": false`, and keeps unknown mods and
  options.

## Mod manifest

Each mod has a manifest, `mods/<mod id>/mod.json` in the game repo. The game's build copies them beside the binary
(`<build dir>/mods/<mod id>/mod.json`), where the launcher lists them; the launcher also lists the data mods in the settings
directory's `mods/` ("Data mods"). The user-facing text (names, descriptions, labels) lives in the manifests only; the user's values
live in the settings file, never in the manifest.

**Top level:**

| Key | Required | Meaning |
|---|---|---|
| `schema` | yes | 1 |
| `id` | yes | Must equal the mod's directory name; the key under the settings' `mods` |
| `name` | yes | Shown in the launcher |
| `version` | no | A string |
| `description` | no | Shown on the mod's page |
| `kind` | yes | `builtin` (code in the game, from the game's build) or `data` (files: "Data mods") |
| `textures` | data mods | `{ "dir": "textures", "filter": "linear" }`, both optional: the mod is a texture pack (RUNTIME.md "Texture packs"), its PNGs under `dir`, filtered `linear` or `nearest` |
| `requires_port` | no | Integer: the stack's mod interface the mod needs (`PSXSTACK_API`, GAME_CONTRACT.md "Compatibility and versions"; 1 now) |
| `applies` | no | `live` (default) or `restart`: whether the in-game menu can switch the mod on or off and change its options while the game runs ("In-game menu"); `restart` for a mod whose `start` patches the game or must run before the window opens. The registry prints the value in force (a `PortMod` with a `start` and no `stop` is `restart` unless it says `live`), so a manifest that names it must agree |
| `options` | no | A list of options |
| `presets` | no | A list of presets: `{ "id", "name", "description" (optional), "values": { "<option id>": value } }`. The launcher shows a button per preset (pressed while all its values are in place) that sets those values; the game never sees a preset, only the values |

**An option:** `id` (not `enabled`, unique in the mod), `name`, `description` (the launcher's tooltip), `type`,
`default`, and optionally `group` (a heading on the mod's page; ungrouped options come first) and `applies` (`live` or
`restart`; `restart` is marked in the launcher). Types:

| Type | Extra keys | Value in the settings |
|---|---|---|
| `bool` | | `true`/`false` |
| `int`, `float` | `min`, `max`, `step`; `slider_max` with `input_toggle` | A number in range (an integer for `int`) |
| `enum` | `values`: `[{ "id", "label" }]` | One of the value ids |
| `binding` | | The binding grammar of "Input bindings"; the default written the same way |

There is no `string` or `path` type. An `int` or `float` with `min` and `max` is a slider, without them a number field.
**`slider_max` and `input_toggle`** (together, on an `int` or `float` with `min` and `max`): `input_toggle` names a
`bool` option of the same mod; while it is off the option is a slider from `min` to `slider_max` (above `min`, at most
`max`), and a larger value in the settings counts as `slider_max` (the game logs it and uses `slider_max`; the file
keeps the value); while it is on the option is a typed number from `min` to `max`. A preset's value above
`slider_max` must come with the toggle set to `true` in the same preset. Example (`tests/fixtures/mods/skip_dialogues/`):

```json
{
  "schema": 1, "id": "skip_dialogues", "name": "Skip dialogues", "version": "1.0", "kind": "builtin",
  "description": "Text appears at once and advances by itself. Choices still wait for you.",
  "options": [
    { "id": "toggle", "name": "Toggle", "type": "binding", "default": "F2" },
    { "id": "hold",   "name": "Hold",   "type": "binding", "default": "" },
    { "id": "fast_forward_waits", "name": "Also fast-forward cutscene waits", "type": "bool", "default": false,
      "applies": "live" }
  ]
}
```

The built-in mods use the same schema as later data mods, so one renderer in the launcher serves both. A manifest the
launcher cannot use (not JSON, another schema, an `id` that is not its directory's name, a bad option or preset, an
unknown `kind`, a data mod with options or without `textures`) is listed with the reason and cannot be switched on.

## Data mods

A data mod is files, no code: `<settings dir>/mods/<mod id>/mod.json` with `"kind": "data"` and the files it names. The
only kind of data so far is `textures`, a texture pack (RUNTIME.md "Texture packs"; drawn by the hardware renderer).
- **The launcher** lists them on the Mods screen below the built-in mods, in their priority order, each with its
  on/off switch and, on its page, **Earlier**/**Later** (priority) and its directory. A built-in manifest there, an id a
  built-in mod has, or a data mod with options is listed as unusable.
- **The game** finds them through `--mods-dir DIR` (the launcher passes `<settings dir>/mods`; the game never looks for
  a directory itself) and reads each `DIR/<mod id>/mod.json`: schema 1, the id its directory's name, `kind` `data`,
  `requires_port` at most its API; others are logged and skipped. The settings switch one on as a built-in mod
  (`mods.<mod id>.enabled`; an unknown id is not logged for a data mod found there) and `mod_order` orders them, the
  first winning where two replace the same texture; those it does not name come after, by id. Packs given with
  `--texture-pack` come before all of them. Off under `--script` unless `--script-mods`, as every mod. Only the SDL
  build takes `--mods-dir` (the headless build exits 64).

## Mod runtime

The runtime's `mods.c` holds the registry of built-in mods the game's adapter provides (GAME_CONTRACT.md "4. The adapter
units": ids, versions, option types, defaults, ranges, `start`/`frame` callbacks), reads their values from
`mods.<mod id>`, registers each enabled mod's `binding` options as hotkey actions (named `<mod id>.<option>`), and runs
them at every vsync; the in-game menu re-reads the section live (`port_mods_apply`, "In-game menu"). `<id> --print-mods` prints the registry as JSON; a game's tests require every manifest to agree
with it.

**Rules for a mod that changes the game's behaviour:**

1. Its hook in the game's C sits in an `#ifdef PC_PORT` block that tests a `port_mod_*` flag (declared in the game's
   hook header). Never `if (MACRO_THAT_IS_0)` or `|| MACRO`: an expression that is constant on the PS1 can change the
   old compiler's code. The game's byte-identical build gates every hook.
2. **Mods are off under `--script`** unless the run passes `--script-mods`, so the replays, goldens and records stay
   the bare binary's.
3. A hotkey never reaches the pad: no `I` line in the frame log, no change to a record.
4. A mod's own state lives in the runtime's variables. A game global a mod must set is set before
   `port_overlay_init()` (the reset's snapshot); a later write breaks the reset check and is undone by a reset.
5. A run with a mod on needs its own expected results: a game's random generator usually steps once a frame, so any
   skipping shifts later rolls (as a faster player would).
6. **`start` is a setter and `stop` undoes it** ("In-game menu"): `start` sets the mod's `port_mod_*` flag and reads
   its options into the runtime's variables, and the engine may run it again when the options change in the menu;
   `stop` clears the flag (and anything else `start` set). A mod with a `start` and no `stop`, or whose `start` must
   run before the window opens (widescreen), says `applies: "restart"` (the engine assumes it) and the menu marks it.

**The pace split** (the runtime's `pump.c`): `port_rate` is the nominal rate (50 or 60: the vsyncs per second the game
is made for, which sets the audio's samples per vsync); the pace (`port_pace_set`; 0 = unthrottled) is the wall
clock's vsyncs per second. Every change of the pace starts the schedule over, so lowering it does not stall the game.
`--fps N` sets both.

## Launcher

`<id>-launcher` (`launcher/`) is C++17 with Dear ImGui on SDL3 + `SDL_Renderer`; it is its own CMake project and the
game stays C. Every name, string, hash and number it shows comes from the game's description (`launcher/src/brand.h`
over the generated header). It finds the settings directory, edits `settings.json` (keeping every member it does not
edit, unknown ones included), and starts the game. Screens: **Disc** (file dialog, drag-and-drop, or a typed path; only
the described discs' SHA-1s are accepted), **Play** (starts the game; on an error shows the exit status, the crash
report and the last lines of output, "Crash report"), **Settings** (scale, fullscreen, the rate when the game offers
two, renderer, mute, memory cards, the update check when the game has one: "Updates", Add to Steam: "Steam"), **Controls** (keyboard, gamepad
with the second player's gamepad, hotkeys), **Mods** (an on/off switch per mod and a page generated from its manifest; the data mods in the settings
directory's `mods/` in priority order, "Data mods"). Build, run, details and the self-test: `launcher/README.md`.

## Updates

A game whose description names its GitHub repository (`launcher.updates.github`, GAME_CONTRACT.md "1. game.json") gets
an update check in the launcher; without one there is no network call and nothing of it on the screens. In order:
the check at start finds a newer release ("The check"), a prompt offers it, **Update now** downloads this platform's
package and verifies it ("Update now"), and the package replaces the install and the new launcher starts ("Applying
one"). Where the launcher cannot update itself (a development build, a folder it cannot write), the prompt's action is
the release's page in the browser.

### The check

- **When:** at the launcher's start, on a worker thread, when `launcher.updates.check` is true (the default) and the
  launcher was not started with `--no-update-check`; and at the Settings screen's **Check now**, whatever the setting.
- **What:** one GET of `https://api.github.com/repos/<repo>/releases/latest` (GitHub leaves drafts and prereleases
  out) through the system's `curl` as a child process (`launcher/src/http.cpp`; DECISIONS "Updates: GitHub releases,
  curl as a process"): `-fsSL`, the User-Agent `<id>-launcher/<version>`, 10 s at most. The answer is read with the
  runtime's `json.c`.
- **The comparison:** the release's `tag_name` (`vX.Y.Z`) against the launcher's version stamp (`launcher_version`,
  GAME_CONTRACT.md "Releases") as three integers. Only a stamp that is exactly `X.Y.Z` is offered an update; a
  development build (`dev-<commit>`, `X.Y.Z-N-g<hash>`, `-dirty`) is told the latest version and nothing more.
- **The package:** this platform's asset, found by the packages' names (`<id>-<version>-x86_64.AppImage`,
  `<id>-<version>-windows-x86_64.zip`), is kept with its size, URL and SHA-256 (the asset's `digest`); a release
  without it is still reported.
- **The prompt:** when the check at start finds a newer release, a modal over the Play screen (never while the game
  runs; it waits until the game has ended): "<title> X.Y.Z is available (you have A.B.C)." with **Update now** (first,
  the default: Enter; only when this install can update itself, "Update now"), **Release page** (the release's page in
  the browser), **Later** (or Escape: asked again at the next start) and **Skip this version** (writes
  `launcher.updates.skipped`: that release and older ones are not offered again, a newer one is). When the install
  cannot update itself, a line says why ("This build cannot update itself. ..."; the plan's reason, "Applying one")
  and Release page is the action. Check now only reports, on the Settings screen.
- **The Settings screen** ("Updates"): the checkbox "Check for updates when the launcher starts"
  (`launcher.updates.check`), the launcher's version, the last check's result and time, the skipped release
  (**Forget** clears it), **Update now** when a newer release was found and the install can update itself (else the
  line saying why), **Check now** and **Release page**. The launcher's window only: the shared Settings screen
  (`launcher/ui/screens.cpp`) leaves a place after "Memory cards" that its host fills (`Screens::set_settings_extra`),
  and the in-game menu fills none, so the game's menu has no update check.
- **Failures** are reported in words and never retried by themselves: curl missing (Windows 10 before 1803, a Linux
  without it; **Release page** then opens the repository's latest release), no network (curl's own error), an HTTP
  error (404: the repository has no release; 403: GitHub's limit of 60 requests an hour from one address without an
  account), an answer that is not a release (not JSON, no tag, a tag that is not `vX.Y.Z`).

### Update now

On the main loop, polled each frame like the disc check (`launcher/src/app.cpp`); never while the game runs, and no
update check is started while it goes on (Check now is disabled).

1. **The plan** (`update_plan(update_system(), <settings dir>)`, made when the launcher starts and again at the press):
   whether this install can update itself and where the package goes ("Applying one"). Not possible: the line instead
   of the button. The release must also have this platform's package, and GitHub must give its `digest`: without one
   nothing is downloaded (a download that cannot be verified is never applied) and the prompt says so.
2. **The download:** the release asset's `browser_download_url` through curl to the plan's file in the settings
   directory (`HttpDownload`: the asset's size is the expected one, a different size is a failure), with a progress bar
   in the prompt ("1.7 of 5.5 MB") and **Cancel** (or Escape), which kills curl, removes the partial file and closes
   the prompt as Later.
3. **The check:** `update_verify` of the file against `sha256:<the asset's digest>`.
4. **The swap** (`update_apply` with the plan, the file, the digest and the launcher's own arguments, argv without
   the program's name: `--config-dir`, `--game`, `--no-update-check` as given). The settings are saved first. When it
   started the new launcher (`exit_now`), this one quits through its normal path. The new launcher is not prompted
   again: its version stamp is the release it installed, so its check finds nothing newer.

A failure at any step (curl's error, a wrong size, a digest that does not match, the swap's own failures) leaves the
old install as it was, removes the downloaded file and shows the message in the prompt with **Release page** and
**Close** (Escape); Update now can be pressed again from the Settings screen. If the swap succeeded but the new
launcher could not be started, the prompt says so (the update is in place; start the launcher again).

`$<PREFIX>_LAUNCHER_UPDATE_NOW=1` makes the check at start answer its own prompt with Update now: for a test run of
the whole flow against the real releases (launcher/README.md "Updates").

### Applying one

How a downloaded release replaces the running install (`launcher/src/update_apply.{h,cpp}`); the download and the
prompt are the caller's. Every step that fails leaves the old install as it was and says what failed: a damaged
download, a folder that is not writable, no tar, a zip tar cannot read, a zip without the two executables, a file where
the release has a folder, a move that did not happen. The user's data is never in the install (settings, cards, logs,
crash reports and data mods are in the settings directory, "Settings directory").

- **The plan** (`update_plan`) comes first, before anything is downloaded, so the UI can say whether the launcher can
  update itself or must send the player to the release page:
  - Linux: only from an AppImage, the running image `$APPIMAGE` (set by the AppImage runtime), in a writable folder.
    A development or unpacked build has no self-update.
  - Windows: the launcher's folder holds `<id>-launcher.exe` and is writable, and a tar was found:
    `%SystemRoot%\System32\tar.exe` first (bsdtar, Windows 10 1803 and later), then `tar.exe` on PATH (a GNU tar
    there, such as Git's, reads no zip).
  - The download goes into the settings directory: `<id>-update.AppImage` or `<id>-update.zip`.
- **The check**: the file's SHA-256 (`runtime/sha256.c`, read in chunks) against the asset's `digest` in GitHub's
  release API, `sha256:<hex>`, before anything is touched; `update_apply` checks it again itself.
- **AppImage**: the new image is copied beside the running one as `.<name>.update` (the same filesystem; the download
  may be on another), made executable and renamed over the old one: one rename, so the folder holds either image
  whole. The running launcher keeps the old image mounted until it exits.
- **Windows folder**: the staging folder is `<id>-update.tmp\` inside the install folder (the same volume, so every
  move is a rename). The zip is copied there and unpacked with `tar -xf package.zip -C new` in that folder (relative
  paths only). The release is the zip's one top folder (`<id>-<version>-windows-x86_64\`, the package's layout) or its
  top, and must hold `<id>-launcher.exe` and `<id>.exe`. Each of its files then replaces the install's: the running
  `<id>-launcher.exe` cannot be overwritten but can be renamed, so it becomes `<id>-launcher.exe.old`; every other
  replaced file (the game is not running) moves into the staging folder's `old\` first. Missing folders are created;
  files the release does not have (a mod the player added beside the game, notes) stay. The moves are journaled: a
  failure moves everything back (should that fail too, the staging folder keeps what could not be put back and the
  message says where). At the end the staging folder goes, the replaced files with it.
- **The relaunch**: the new launcher (the image; `<id>-launcher.exe`) is started detached with the arguments the
  caller passes (its own, such as `--config-dir`), the download is removed, and the caller exits at once. When the
  start fails the update stays and the message asks the player to start the launcher again.
- **At start** (`update_cleanup_at_start`, from `main()`): a `<id>-launcher.exe.old` beside the launcher is removed
  (retried for about a second while the old launcher exits; else at the next start), and so are a stale staging folder
  (unless its `old\` holds files: a failed rollback's) and an AppImage's leftover temporary file.

## Steam

The launcher can put the game in the player's Steam library as a non-Steam shortcut and take it out again (issue
#93), so Steam's library, Game Mode on the Steam Deck, its controller configuration and its overlay reach it. Nothing
of it runs until the player asks: the launcher never looks for Steam, reads its files or checks that it is installed
at its start or in the self-test's normal path.

- **The Settings section** (`launcher/src/steam_section.cpp`; the launcher's window only, through the shared Settings
  screen's hook: the in-game menu has none). **Add to Steam** looks for Steam (below) and shows what it found: the
  Steam folder and the account, each a choice when there is more than one (the most recent account first), then
  **Add** or **Cancel**; nothing found, or no account yet, is said with where it looked. **Add** writes the shortcut
  to the launcher itself (the AppImage, `$APPIMAGE`, the path the self-update keeps; on Windows `<id>-launcher.exe`),
  named after the game (game.json's title), and records it in `launcher.steam`. The section then shows "In Steam,
  for <account>" with **Remove from Steam**, and, when the launcher now runs from another path than the shortcut's
  (the AppImage was moved), **Update shortcut**, which replaces the old entry with one for the new path in one write.
  Remove takes out that shortcut only; one already gone (removed in Steam, the account gone) is forgotten, and said
  so. A file that does not read is left as it is and nothing is recorded.
- **Steam must be closed**: it keeps the shortcuts in memory and writes them back when it exits, which would undo the
  change. Every button checks for a running Steam first (`steam`, `/proc/*/comm`; `steam.exe` on Windows) and, when
  it runs, asks the player to quit it (on the Steam Deck, from the desktop mode) and writes nothing. It never stops
  Steam itself.

- **Where Steam is** (`launcher/src/steam.cpp` `steam_find`): on Linux `$XDG_DATA_HOME/Steam`, `~/.local/share/Steam`,
  `~/.steam/steam` (usually a link to the one before it: one folder, counted once) and Flatpak's
  `~/.var/app/com.valvesoftware.Steam/.local/share/Steam`; on Windows `HKEY_CURRENT_USER\Software\Valve\Steam`'s
  `SteamPath`, then `%ProgramFiles(x86)%\Steam`. A folder counts when it holds `userdata/`. More than one may be found.
- **Its accounts**: `userdata/<id>/`, one per account that has logged in on the machine (`0` and other names are not
  accounts), named from `config/loginusers.vdf` (a text VDF: its `PersonaName`, else `AccountName`; an account's
  SteamID64 is its id plus 76561197960265728). The one marked `MostRecent` is offered first.
- **The file**: `userdata/<id>/config/shortcuts.vdf`, a binary VDF (`launcher/src/vdf.cpp`: maps, strings and
  numbers, each kept as its bytes, so a file is written back exactly as it was read; a file with anything else in it is
  refused and never rewritten). A shortcut is added with every field Steam writes, in its order (`appid`, `AppName`,
  `Exe` and `StartDir` in quotes, `icon`, `ShortcutPath`, `LaunchOptions`, `IsHidden`, `AllowDesktopConfig`,
  `AllowOverlay`, `OpenVR`, `Devkit`, `DevkitGameID`, `DevkitOverrideAppID`, `LastPlayTime`, `FlatpakAppID`, `sortas`,
  `tags`) at Steam's defaults for the ones the launcher does not set; one with the same appid is replaced, never doubled;
  removing takes out only the entries with that appid; the entries are numbered again from 0 either way. The file is
  written beside itself and renamed into place, and the first time the launcher writes it, the file as it was is kept
  as `shortcuts.vdf.bak`.
- **The appid**: CRC-32 of the quoted `Exe` followed by `AppName`, with the top bit set (the convention Steam's own tools
  follow; it also names the artwork in `userdata/<id>/config/grid/`).
- **The artwork** (a game that names it: game.json `launcher.steam_art`, GAME_CONTRACT.md "1. game.json"; embedded in
  the launcher at build time, `launcher/src/steam_art.cpp`): Add writes each picture into the account's
  `config/grid/` as `<appid>p.png` (the portrait capsule), `<appid>.png` (the wide capsule), `<appid>_hero.png`,
  `<appid>_logo.png` and `<appid>_icon.png`, the last also the shortcut's `icon`, and records their names in
  `launcher.steam.art`; Update shortcut writes them under the new appid and removes the old ones; Remove removes
  exactly the files recorded (a name that is not a plain file name is never touched). A picture that cannot be written
  leaves the shortcut in place and is said; Steam then shows its own. A game without artwork gets Steam's placeholders.
- **The test**: the self-test (`launcher/src/selftest_steam.cpp`) works on fixture folders only:
  `tests/fixtures/steam/` (written by its `make_fixtures.py`, an encoder separate from the launcher's) holds a
  `shortcuts.vdf` that must come back byte for byte, the same file with the launcher's shortcut added (adding it must
  give exactly those bytes, removing it the original), and a `loginusers.vdf`; a fixture home holds the Linux folders
  (the link included) and a Flatpak one. On Windows the registry reader is checked on a value of its own, written
  under `HKEY_CURRENT_USER\Software\<id>-launcher-self-test` and removed. The window's part gives the App a lookup,
  a running check and launcher paths of its own (`AppOptions::steam`): nothing is looked up before Add to Steam is
  pressed (counted), a running Steam refuses Add, Update and Remove with the files unchanged, Add keeps the other
  shortcuts byte for byte and the old file as `.bak`, the record is saved at once and read back at the next start
  (still nothing looked up), a moved launcher gets Update shortcut, Remove gives back the file as it was before Add,
  and the not-found, already-removed and damaged-file cases; pictures of each state (`20*-Settings-steam*.png`). The
  artwork: written by appid with the icon the shortcut's, moved by Update, removed by Remove with another shortcut's
  left; the build's own pictures checked as PNGs. CI also builds the launcher of `examples/hello`, whose game.json
  names artwork of its own (`examples/hello/steam/`, drawn by its `make_art.py`), and runs its self-test, so the path
  from game.json to the pictures written is covered; its play path is skipped (it describes no disc).

## In-game menu

A game configured with `-DPSXSTACK_MENU=ON` (with `PSXSTACK_SDL`; `scripts/setup.sh imgui`) has the launcher's UI
inside its own window: `launcher/ui/` (C++17 over Dear ImGui, `cmake/imgui.cmake`) and the launcher's data layer
(`launcher/src/`: settings, input, mods, json_value, paths) are compiled into the game as an object library of their
own (the launcher's headers first in its include order: its `settings.h` and `mods.h` share their names with the
runtime's), and the game then links as C++; the game's units and the runtime stay C, and a build without the option
links the do-nothing stubs of `runtime/menu.c`, so the runtime's call sites need no `#ifdef`. The C API is
`include/psxstack/menu.h` (`port_menu_*`). It is for handhelds and the couch (Steam Deck: gamescope shows one window
at a time), where leaving the game for the launcher's window is a chore.

- **The `menu` hotkey** (`input.hotkeys.menu`; default Escape, or Back held with Start on a gamepad: the chord's
  first input reaches the pad until it is complete, "Input bindings") opens the menu in a pause of the pump
  (`runtime/pump.c`: the game held between two vsyncs, the audio device paused, the watchdog re-armed). While it is
  up, the pause's polls build the menu's frame and the window presents the last picture with the menu over it, on
  either present path: `SDL_Renderer` (`imgui_impl_sdlrenderer3`, before `SDL_RenderPresent`) or the hardware
  renderer (`imgui_impl_sdlgpu3`: a pass over the swapchain texture in the present's command buffer, before its
  submit). The window's events go to ImGui as well as to the runtime's keys (`runtime/input.c`): the menu navigates
  with the keyboard, the mouse and every gamepad; the pause, save and load hotkeys do not act while it is up. The
  hotkey again, or **Resume**, closes it and the game goes on; **Quit** ends the run with status 0.
- **The pages** (the navigation on the left; the shoulder buttons or Ctrl+PageUp/PageDown step through them): **Game**
  (the title, Resume, Quit) and the launcher's own **Settings**, **Controls** and **Mods** screens
  (`launcher/ui/screens.cpp`, the one implementation both draw; the update check, "Updates", is the window's own),
  over the settings file the game was started with (`--config`: the runtime's `settings.c` read it; the menu reads it
  again through the launcher's `SettingsFile` and writes it as the launcher would, keeping every member it does not
  edit) and the mods beside the executable plus the user's data mods (`--mods-dir`). A change is saved when no widget
  is in use (and when the menu closes), and the runtime then re-reads the file and applies it (`runtime/live.c`
  `port_live_apply`): the window's scale and fullscreen, the hardware renderer's filter and sub-pixel mode,
  `audio.mute` (a game started muted opens its audio device when the mute goes off), the pad map and every hotkey (the
  table starts over, the enabled mods' bindings registered again) and the mods (`port_mods_apply`: one switched on
  gets its hotkeys and its `start`, one switched off its `stop`, one that stays on with other values its `start`
  again, since a mod's `start` is a setter: "Mod runtime" rule 6). What waits for the next start is marked
  **(restart)** on the page: the disc, the renderer and its internal scale, the rate, the memory cards, the data mods,
  and a mod whose manifest says `"applies": "restart"` ("Mod manifest"). A file that does not read back is logged and
  changes nothing. A game started without `--config` has the Game page alone; one the launcher started
  (`<PREFIX>_LAUNCHER` in its environment) says Quit *to launcher*. The disc stays the launcher's window's.
- **The style** (`launcher/ui/style.cpp`, shared with the launcher's window) is scaled to fit the launcher's canvas,
  its window at scale 1 (960x620, `launcher/ui/style.h` `CANVAS_W`/`CANVAS_H`), into the game's window, so a page the
  launcher fits fits the menu too: about 1.74 at 1920x1080, 1.29 at 1280x800, at least 1. `video.menu_size` steps
  it ("smaller" 0.8 times, "larger" 1.25 times; Settings, "Menu size", applied as soon as it is chosen), never past
  the scale that leaves a 640x480 canvas. The shared screens fit that smallest canvas: the Mods screen's list
  becomes a combo above the mod's page when the two do not fit side by side, a mod's options stack (the name and
  Default on a line, the widget across the page) when the table's widget would be narrower than 160, rows of
  buttons and chips wrap, and a mod's name the list cannot fit ends in an ellipsis (the whole name in its tooltip).
  The list and the page share the gamepad's navigation (`ImGuiChildFlags_NavFlattened`), and the page scrolls to
  the widget it reaches.
- **The test:** `--input-test` ends with the menu's round trip (its hotkey at a vsync, the menu shown in the pause's
  polls, the hotkey again, the next vsync follows the paused one), and `<PREFIX>_PORT_MENU_SHOT=PATH` writes the
  window's output with the menu up as a PNG, on either renderer (the hardware renderer's swapchain cannot be read
  back, so the present that takes it draws the picture and the menu into a texture of the swapchain's format, blits
  that into the swapchain and reads it back: `runtime/render_gpu.c`), on the page
  `<PREFIX>_PORT_MENU_SCREEN=game|settings|controls|mods` names (`mods:<id>`: with that mod chosen;
  `controls:keyboard|gamepad|hotkeys`: on that tab), with the window first sized to `<PREFIX>_PORT_MENU_WINDOW=WxH`
  when it is set (SDL's software renderer follows it: `SDL_RENDER_DRIVER=software`). The shot's log line names the
  windows whose content reaches past their right edge ("nothing wider than its window" when none does).
  The round trip also re-applies the settings live at its second poll, as the menu does after a save, and with
  `<PREFIX>_PORT_MENU_LIVE_FILE=PATH` that file is put in place of the settings first (the hotkey pressed next is the
  file's binding, so a rebinding must take). `tests/hello_test.py --menu` builds `examples/hello` that way and runs
  both on SDL's offscreen driver with a settings file of its own, muted (the live file rebinds the menu hotkey,
  switches fast-forward on and the mute off, so the audio device must open), the picture its Settings page (with
  `--gpu` also on the hardware renderer, on Mesa's lavapipe, which presents to the offscreen driver: the software
  picture but for the anti-aliased edges, at most 0.1 % of the pixels off by more than 8 levels), then the Mods page's
  at 1920x1080, 1280x800 and 640x480 and at 1280x800 "larger" (`tests/fixtures/mods/multipliers` chosen, a page larger
  than any a game ships so far), none of them wider than its window; CI's `launcher` job runs it and keeps the
  pictures. The screens' logic is the launcher self-test's.
- **The release:** a game that ships the menu links libstdc++ and libgcc statically with
  `-DPSXSTACK_MENU_STATIC_RUNTIME=ON`, as the launcher does (`launcher/README.md` "Releases").

## Crash report

The game writes a crash report when it dies or stops on an error (`docs/PORT.md` "Crash report"): the launcher passes
`--crash-dir <settings dir>/crashes`, streams the game's whole output to `<settings dir>/logs/last-run.log` (the
previous run's kept as `last-run.1.log`) and, when the game ends with an error, reads the report the game named
(`port: crash report: PATH`) and shows its path on the Play screen. **Copy** puts on the clipboard, in this order: the
launcher's version and the platform, the game's path and version, the command, the result in words, the report's
text, the last 40 lines of the output and the log file's path: what to paste into an issue. **Open folder** opens the
crash directory. A game too old for `--crash-dir` is reported as too old, like one without `--config`. On Windows the
same, with the minidump (`crash-<date>.dmp`) beside the report in `%APPDATA%\<id>\crashes\` for a tester to attach.

## The rate

A setting, not a mod: `video.refresh` (or `--refresh N`), default the description's `video.rate` (50 PAL, 60 NTSC). The
launcher offers the description's `video.rates` and shows its `rate_note` when the other rate is chosen; a game with one
rate has no Refresh row. The runtime sets `port_rate` and the pace to the rate (735 or 882 audio samples a vsync) and the
CD drive's vsync rate (its sectors and XA frames per second stay the same), then calls the adapter's
`game_apply_rate(rate)` before `port_overlay_init()` (so the reset's snapshot holds whatever it set): that is where a
game runs its own mode for the other rate, as Digimon World 2003 does with the flag its NTSC patch sets (its docs
describe it). `--fps N` alone is not that mode: it speeds everything up, music and the play-time clock included.
