// The settings directory and the settings file (docs/LAUNCHER.md "Settings directory", "Settings file"): the contract with the game, which
// reads the same file through `<game> --config <dir>/settings.json`. Paths inside the file are relative to its
// directory (or absolute).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "brand.h"
#include "input.h"
#include "json_value.h"

namespace psxstack {

constexpr int SETTINGS_SCHEMA = 1;
constexpr const char *SETTINGS_FILE = "settings.json";
constexpr const char *SETTINGS_PORTABLE_FILE = "portable.txt";
constexpr const char *SETTINGS_DIR_ENV = ENV_CONFIG_DIR; // <PREFIX>_CONFIG_DIR
// SDL_GetPrefPath's names: "" (no organisation level) and the application's directory name, so the per-user
// directory is ~/.local/share/<app>/ on Linux and %APPDATA%\<app>\ on Windows.
constexpr const char *SETTINGS_PREF_ORG = "";
constexpr const char *SETTINGS_PREF_APP = GAME_ID;

// ---- Where the settings live, in this order.
enum class DirSource {
    Argument,    // 1. --config-dir DIR
    Environment, // 1. $<PREFIX>_CONFIG_DIR
    Portable,    // 2. an (empty) portable.txt beside the executable: the executable's directory
    CurrentDir,  // 3. a settings.json that already exists in the current directory (never created there implicitly)
    User,        // 4. the per-user directory (SDL_GetPrefPath)
    Config,      // the game's --config file's directory (the in-game menu; never the launcher's own lookup)
};
const char *dir_source_name(DirSource s);

struct DirLookup {
    std::string arg;     // --config-dir, or ""
    std::string env;     // $<PREFIX>_CONFIG_DIR, or ""
    std::string exe_dir; // SDL_GetBasePath()
    std::string cwd;     // SDL_GetCurrentDirectory()
    std::function<std::string()> user_dir; // called only when the first three do not apply (it creates the dir)
};

struct SettingsDir {
    std::string dir; // no trailing separator; "" when none could be found
    DirSource source = DirSource::User;
    std::string error;
};

// The real inputs: SDL's base path, the current directory, the environment, SDL_GetPrefPath.
DirLookup dir_lookup_from_system(const std::string &arg);
SettingsDir settings_dir_choose(const DirLookup &in);

// ---- The settings file (schema 1, settled by the game's side): the values the launcher edits. The rest of the file is kept as it
// was; the defaults are the game's (port/src/settings.c), so a missing member means the same on both sides.
struct MemoryCard {
    bool present = true; // false: null in the file, no card in the slot
    std::string path;    // a .mcd image, relative to the settings directory (created formatted by the game)
};

// video.filter's values, the game's (runtime/video_filter.c port_filter_names), in the Filter combo's order.
inline const char *const FILTER_NAMES[] = { "none", "sharp", "scanlines", "crt", "smooth" };
// video.menu_size's values (the in-game menu's text, launcher/ui/menu.cpp), in the Settings screen's order.
inline const char *const MENU_SIZE_NAMES[] = { "smaller", "auto", "larger" };
// input.player2's values (the game's runtime/input.c): none, or the gamepad by connection order that is player 2.
inline const char *const PLAYER2_NAMES[] = { "off", "first", "second" };

// launcher.steam (docs/LAUNCHER.md "Steam"): what Add to Steam did, so Remove and Update shortcut undo exactly that
// without looking for Steam again. appid 0: not in Steam.
struct SteamRecord {
    std::string dir;          // Steam's folder
    std::string account;      // userdata/<account>
    std::string account_name; // its name when it was added ("" when Steam had none)
    uint32_t appid = 0;       // the shortcut's
    std::string exe;          // the launcher's path it points at
    std::string name;         // its AppName
    std::vector<std::string> art; // the pictures written beside it, in userdata/<account>/config/grid
    bool added() const { return appid != 0; }
};

struct Settings {
    std::string disc_path; // "" = unset
    std::string disc_sha1; // the SHA-1 the launcher verified for disc_path ("" = not verified)
    int scale = 2;         // the window: 320*scale x 240*scale (1..16)
    bool fullscreen = false;
    int refresh = GAME_RATE; // 50 or 60: the nominal rate, or the game's other one (docs/LAUNCHER.md "The rate")
    std::string renderer = "software"; // "software" or "gpu" (the hardware renderer; docs/LAUNCHER.md "Members")
    int internal_scale = 1;            // the hardware renderer's resolution, 1..8 times the PS1's
    std::string subpixel = "on";       // "on", "perspective" or "off": the 3D at the GTE's sub-pixel positions above x1
    std::string filter = "none";       // the hardware renderer's present filter (video.filter): one of FILTER_NAMES
    int crt_scanlines = 50, crt_mask = 30, crt_curvature = 0; // video.crt: the scanlines and crt filters', 0..100
    std::string menu_size = "auto";    // the in-game menu's size (video.menu_size): one of MENU_SIZE_NAMES
    bool mute = false;
    MemoryCard memcard[2] = { { true, "card1.mcd" }, { true, "card2.mcd" } };
    std::string last_dir; // launcher.last_dir: where the file dialog opens (the launcher's own state)
    // launcher.updates (docs/LAUNCHER.md "Updates"): the check at start, and the release tag the user skipped.
    bool update_check = true;
    std::string update_skipped; // "v0.5.0"; "" = none
    SteamRecord steam;          // launcher.steam
    // input: only what the file sets; a button or action that is absent keeps the game's default (input.h), so a
    // changed default in a later game reaches every user who never rebound it.
    std::map<std::string, std::vector<std::string>> keyboard; // input.keyboard: button -> key names
    std::map<std::string, std::vector<std::string>> gamepad;  // input.gamepad: button -> gamepad input names
    std::map<std::string, Binding> hotkeys;                   // input.hotkeys: action -> binding
    std::string player2 = "off";                              // input.player2: one of PLAYER2_NAMES

    // The effective values (the file's, else the defaults).
    std::vector<std::string> keys_for(const std::string &button) const;
    std::vector<std::string> pad_for(const std::string &button) const;
    Binding hotkey_for(const std::string &action) const;
};

// Reads the known members of `doc` into a Settings (defaults for missing ones; a warning for each invalid one).
Settings settings_from_json(const Json &doc, std::vector<std::string> *warnings);
// Writes `s` into `doc`, keeping every other member and the members' order.
void settings_to_json(const Settings &s, Json *doc);

class SettingsFile {
public:
    enum class State {
        New,    // no file yet: defaults, written at the first save
        Loaded, // read
        Broken, // not JSON (or not an object): defaults; the first save keeps the old file as settings.json.broken
        Newer,  // a newer schema than this launcher's: shown, never written
    };

    // Reads <dir>/settings.json. Never fails: problems end up in state() and messages().
    void load(const std::string &dir);
    // Writes the file when its text changed (creating the directory). False with the reason in `err`.
    bool save(std::string *err);

    const std::string &dir() const { return dir_; }
    const std::string &path() const { return path_; }
    State state() const { return state_; }
    bool writable() const { return state_ != State::Newer; }
    const std::vector<std::string> &messages() const { return messages_; }
    // A path from the file (relative to its directory) as an absolute one.
    std::string resolve(const std::string &p) const;

    Settings values;
    Json doc; // the file as read; `values` are written over it at save

private:
    std::string dir_, path_, written_;
    State state_ = State::New;
    std::vector<std::string> messages_;
};

} // namespace psxstack
