// The game in the Steam library as a non-Steam shortcut (docs/LAUNCHER.md "Steam"; issue #93): where Steam is, its
// accounts, and the shortcut in an account's userdata/<id>/config/shortcuts.vdf (vdf.h). Nothing here runs unless the
// player asks: the launcher never looks for Steam at its start.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vdf.h"

namespace psxstack {

// ---- The shortcut

struct SteamShortcut {
    std::string name;           // AppName: the game's title (game.json)
    std::string exe;            // the launcher's path, unquoted (the AppImage, the .exe)
    std::string icon;           // a picture's path, "" for none
    std::string launch_options; // LaunchOptions, "" for none
    std::vector<std::string> tags;
};

// `path` in double quotes, as Steam keeps Exe and StartDir.
std::string steam_quote(const std::string &path);
// The non-Steam shortcut's id: CRC-32 of the quoted Exe followed by AppName, with the top bit set (the convention
// Steam's own tools and the artwork's file names, userdata/<id>/config/grid/<appid>*.png, follow).
uint32_t steam_shortcut_appid(const std::string &exe, const std::string &name);
// The shortcut's entry with every field Steam writes, in its order (Steam's defaults for those the launcher does not
// set: shown, overlay and desktop configuration allowed).
VdfNode steam_shortcut_entry(const SteamShortcut &s);

// The file's "shortcuts" map in `root` (created when missing).
VdfNode *steam_shortcuts(VdfNode *root);
// Adds `s` (replacing an entry with the same appid rather than adding a second), the entries numbered again from 0.
// Returns the appid.
uint32_t steam_shortcuts_add(VdfNode *root, const SteamShortcut &s);
// Removes the entries with `appid` (only those), the rest numbered again from 0. Returns how many were removed.
int steam_shortcuts_remove(VdfNode *root, uint32_t appid);
// The entry with `appid`, or nullptr.
const VdfNode *steam_shortcuts_find(const VdfNode &root, uint32_t appid);

// userdata/<account>/config/shortcuts.vdf: read into `root`; a missing file is an empty one. False, with the reason,
// for a file that does not read (it is then never written).
bool steam_shortcuts_load(const std::string &path, VdfNode *root, std::string *err);
// Written next to `path` and renamed over it; the first time, the file as it was is kept as shortcuts.vdf.bak.
bool steam_shortcuts_save(const std::string &path, const VdfNode &root, std::string *err);

// ---- Where Steam is

struct SteamAccount {
    std::string id;           // userdata/<id>: the account's 32-bit id
    std::string name;         // config/loginusers.vdf's PersonaName (else AccountName); "" when it has none
    bool most_recent = false; // loginusers.vdf's MostRecent
};

struct SteamInstall {
    std::string dir;   // Steam's folder (it holds userdata/)
    std::string where; // how it was found: "~/.local/share/Steam", "Flatpak", "the registry", ...
    std::vector<SteamAccount> accounts; // userdata's accounts, by id
    // The account to offer first: the most recent one, else the first; -1 when there is none.
    int default_account() const;
};

// The folders to look in, each with how it was found, in order.
struct SteamLookup {
    std::vector<std::pair<std::string, std::string>> candidates;
};
// Linux's: $XDG_DATA_HOME/Steam, <home>/.local/share/Steam, <home>/.steam/steam (usually a link to it), Flatpak's
// <home>/.var/app/com.valvesoftware.Steam/.local/share/Steam. `xdg_data` is $XDG_DATA_HOME ("" when unset).
SteamLookup steam_lookup_linux(const std::string &home, const std::string &xdg_data);
// Windows': `registry` (HKCU\Software\Valve\Steam's SteamPath; "" when absent), then `program_files_x86`\Steam.
SteamLookup steam_lookup_windows(const std::string &registry, const std::string &program_files_x86);
// This system's (the environment, the registry): only when the player asks for Steam.
SteamLookup steam_lookup_from_system();
// The candidates that are Steam folders (a userdata/ folder), each once (a link to another counted once).
std::vector<SteamInstall> steam_find(const SteamLookup &in);
// A Steam folder's accounts: userdata's numbered folders, named from config/loginusers.vdf when it reads.
std::vector<SteamAccount> steam_accounts(const std::string &steam_dir);
std::string steam_shortcuts_path(const std::string &steam_dir, const std::string &account);

// The self-test of all the above on fixture folders under `dir` (launcher/src/selftest_steam.cpp); never the system's.
void self_test_steam(const std::string &dir, int *checks, int *failures);

// A string value under HKEY_CURRENT_USER (`subkey` with backslashes), "" when absent; always "" off Windows.
std::string windows_registry_string(const std::string &subkey, const std::string &value);

} // namespace psxstack
