// The Settings screen's Steam section (docs/LAUNCHER.md "Steam"): Add to Steam, Remove from Steam, Update shortcut,
// over steam.h and the settings' launcher.steam. The launcher window's alone (drawn through the shared Settings
// screen's hook, Screens::set_settings_extra; the in-game menu has none). Nothing looks for Steam until a button is
// pressed: the lookup, the running check and the files are all behind the actions below.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "settings.h"
#include "steam.h"

namespace psxstack {

// Where the section's Steam comes from: this system's by default; the self-test gives fixture folders, a running
// check of its own and a launcher path.
struct SteamOptions {
    std::function<SteamLookup()> lookup = steam_lookup_from_system;
    std::function<bool()> running = steam_running;
    std::function<std::string()> launcher_path = steam_launcher_path;
    std::string name = GAME_TITLE; // the shortcut's AppName
};

class SteamSection {
public:
    // `save` writes the settings file now (the record must outlive a launcher closed right after).
    void configure(const SteamOptions &options, SettingsFile *settings, std::function<void()> save);
    void draw(float label_w);

    // The buttons (the self-test presses them through these). begin_add looks for Steam: the folders and their
    // accounts, or a message that none was found. add writes the shortcut for the folder and account chosen.
    void begin_add();
    bool add();
    bool remove();
    bool update_shortcut(); // the launcher moved since: the old shortcut out, one for this path in
    void cancel();
    void choose(int install, int account);

    bool choosing() const { return choosing_; }
    const std::vector<SteamInstall> &installs() const { return installs_; }
    int install() const { return install_; }
    int account() const { return account_; }
    // The last action's outcome ("" for none), and whether it went as asked.
    const std::string &message() const { return message_; }
    bool message_ok() const { return message_ok_; }
    // The launcher runs from another path than the shortcut's (an AppImage moved): Update shortcut is offered.
    bool moved() const;

private:
    // Steam closed, else the message that asks to quit it (`what`: the button's name). True when it is closed.
    bool steam_closed(const char *what);
    void say(const std::string &text, bool ok);
    // The shortcut for this launcher into `dir`/`account`'s file (the entry with `replace` taken out in the same
    // write), the record kept. False with the message set (`verb`: "added", "updated").
    bool write_shortcut(const std::string &dir, const std::string &account, const std::string &account_name,
                        uint32_t replace, const char *verb);
    // The record's shortcut out of its file. False with the message set (a file that does not read).
    bool remove_shortcut(int *removed);

    SteamOptions options_;
    SettingsFile *settings_ = nullptr;
    std::function<void()> save_;
    bool choosing_ = false;
    std::vector<SteamInstall> installs_;
    int install_ = -1, account_ = -1;
    std::string looked_in_; // where begin_add looked, for "not found"
    std::string message_;
    bool message_ok_ = false;
};

// The self-test of the section in the launcher's window (launcher/src/selftest_steam.cpp): fixture folders, a running
// check and launcher paths of its own; pictures in <root>/screens/.
void self_test_steam_window(SDL_Window *window, const std::string &root, int *checks, int *failures);

} // namespace psxstack
