// The launcher's window: Dear ImGui over SDL3 + SDL_Renderer (docs/LAUNCHER.md "Launcher"). The Play and Disc
// screens are its own; Settings, Controls and Mods are the shared screens (launcher/ui/screens.h), which the game's
// in-game menu draws too.
#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "disc.h"
#include "game.h"
#include "mods.h"
#include "screens.h"
#include "settings.h"
#include "steam_section.h"
#include "update.h"
#include "update_apply.h"

namespace psxstack {

enum class Screen { Play, Disc, Video, Graphics, Audio, Controls, Mods, System, Count };
const char *screen_name(Screen s);

// SDL video, the window, the renderer and ImGui's context and backends. False with the reason in `err`.
bool gui_open(SDL_Window **window, SDL_Renderer **renderer, std::string *err);
void gui_close(SDL_Window *window, SDL_Renderer *renderer);

// The disc in the settings, as the launcher sees it.
enum class DiscStatus {
    Unset,      // no disc.path
    Missing,    // the file (or the .cue's BIN) is not there
    Unverified, // there, but no matching disc.sha1, or the BIN's size changed: to check
    Checking,   // the check is running
    Verified,   // disc.sha1 is the EU disc's and the BIN has its size
};

struct AppOptions {
    std::string game;       // --game: the game's executable ("" = game_find's lookup)
    bool echo_game = true;  // copy the game's output to the launcher's stderr
    // The update check (docs/LAUNCHER.md "Updates"): false = --no-update-check (none at start; Check now still works);
    // the repository ("" = no update check, nothing of it shown) and the stamp it compares (the self-test passes
    // others); how a page is opened (default SDL_OpenURL; the self-test records it instead).
    bool update_check = true;
    std::string update_repo = GAME_UPDATES_GITHUB;
    std::string version = launcher_version;
    std::function<bool(const std::string &)> open_url;
    // The self-update (docs/LAUNCHER.md "Updates"): the launcher's own arguments (argv without argv[0]), given to the
    // new launcher it starts; the install it replaces (default update_system(): $APPIMAGE, the Windows folder; the
    // self-test gives fixtures); `update_now`: the check at start answers Update now by itself when it finds a newer
    // release ($<PREFIX>_LAUNCHER_UPDATE_NOW=1, for a test run against the real releases).
    std::vector<std::string> args;
    std::function<UpdateSystem()> update_system;
    bool update_now = false;
    // The System screen's Steam section (docs/LAUNCHER.md "Steam"): this system's Steam; the self-test's fixtures.
    SteamOptions steam;
};

// The update prompt's buttons (Close, after a failed update, is Later).
enum class UpdateAnswer { UpdateNow, ReleasePage, Later, Skip };

// Where Update now is.
enum class UpdateStep {
    None,        // not started (or cancelled, or closed)
    Downloading, // the package is being downloaded: update_download()
    Installing,  // downloaded: verified and swapped in at the next frame
    Failed,      // it did not happen (or the new launcher could not be started): update_message(), Release page, Close
};

class App {
public:
    using BindingUse = Screens::BindingUse;

    App(SDL_Window *window, SDL_Renderer *renderer, const SettingsDir &location, const AppOptions &options = {});

    // Every SDL event goes through here (ImGui's backend first).
    void handle_event(const SDL_Event &e);
    // One frame: the UI, the render and the present. With `screenshot`, the frame is also saved there as a PNG
    // before the present (the self-test's pictures).
    void frame(const char *screenshot = nullptr);
    // Saves the settings now if they changed.
    void flush();

    bool quit_requested() const { return quit_; }
    Screen screen() const { return screen_; }
    void set_screen(Screen s) { screen_ = s; }
    SettingsFile &settings() { return settings_; }
    // The last save or screenshot error, shown in the status bar.
    const std::string &last_error() const;
    // Frames are wanted soon (a check or the game running): the main loop then waits less.
    bool busy() const {
        return check_.state() == DiscCheck::State::Running || run_.running() || update_.checking() ||
               update_step_ == UpdateStep::Downloading || update_step_ == UpdateStep::Installing;
    }

    // The disc screen's actions (the self-test calls them too).
    void choose_disc(const std::string &path);
    DiscStatus disc_status() const;
    DiscCheck &disc_check() { return check_; }
    const std::string &disc_message() const { return disc_message_; }
    // The play button: settings saved, checked by the game, the game started (the launcher's window hidden).
    void play();
    const GameRun &game_run() const { return run_; }
    const std::string &play_error() const { return play_error_; }
    // The crash report of the last run ("" when none) and what Copy puts on the clipboard: the launcher, the game,
    // the command, the exit, the report's text and the last lines (docs/LAUNCHER.md "Crash report").
    const std::string &play_report_path() const { return play_report_path_; }
    std::string play_copy_text() const;
    const std::string &game_path() const { return game_; }
    // The shared screens' state (the self-test drives the controls' prompt and picks mods through these).
    Screens &screens() { return screens_; }
    InputCapture &capture() { return screens_.capture(); }
    void capture_for(int section, const std::string &id) { screens_.capture_for(section, id); }
    void capture_for_mod(const std::string &mod, const std::string &option) { screens_.capture_for_mod(mod, option); }
    const std::vector<ModManifest> &mods() const { return screens_.mods(); }
    const std::string &mods_dir() const { return screens_.mods_dir(); }
    const std::string &user_mods_dir() const { return screens_.user_mods_dir(); }
    std::vector<std::string> data_mod_ids() const { return screens_.data_mod_ids(); }
    void select_mod(const std::string &id) { screens_.select_mod(id); }
    // The update check: the start's (when on), Check now, the prompt over the Play screen and its buttons.
    UpdateCheck &update_check() { return update_; }
    void update_check_now();
    bool update_prompt() const { return update_prompt_; } // a newer release waits to be (or is being) shown
    bool update_prompt_shown() const { return update_prompt_open_; } // the modal is on the screen
    void update_answer(UpdateAnswer a);
    // Update now: the plan (whether this install can update itself, computed with the launcher), whether the button is
    // offered (the plan is possible and the release has this platform's package), and why not; the steps after it.
    const UpdatePlan &update_plan() const { return update_plan_; }
    bool update_now_offered() const;
    std::string update_now_unavailable() const; // "" when offered
    UpdateStep update_step() const { return update_step_; }
    const std::string &update_message() const { return update_message_; }
    const HttpDownload &update_download() const { return download_; }
    void update_cancel(); // the download's Cancel: stopped, its file removed, the prompt closed (Later)
    // The System screen's Steam section (its buttons; nothing looks for Steam before one is pressed).
    SteamSection &steam() { return steam_; }

private:
    void draw();
    void draw_nav();
    void draw_status_bar();
    void draw_play();
    void draw_disc();
    void draw_play_error();
    void draw_updates(float label_w);
    void draw_update_prompt();
    void update_poll();
    void update_now();
    void update_step_poll();
    void update_fail(const std::string &message);
    void update_replan();
    bool open_url(const std::string &url);
    void step_screen(int delta);
    void update_disc_check();
    void update_game();
    void open_file_dialog();
    static void SDLCALL file_dialog_done(void *self, const char *const *files, int filter);

    SDL_Window *window_;
    SDL_Renderer *renderer_;
    Screens screens_;        // the shared screens, which own the settings file
    SettingsFile &settings_; // screens_'s
    SettingsDir location_;
    Screen screen_ = Screen::Play;
    bool quit_ = false;
    std::string error_; // the last screenshot error

    // The disc: the check, its outcome, the typed path, the file dialog's answer (it may come from another thread).
    DiscCheck check_;
    std::string disc_message_;
    bool disc_message_ok_ = false;
    char disc_input_[1024] = "";
    std::mutex dialog_mutex_;
    bool dialog_open_ = false, dialog_done_ = false;
    std::string dialog_file_, dialog_error_;

    // The game.
    std::string game_;                    // its executable ("" = not found)
    bool echo_game_ = true;
    std::vector<std::string> game_tried_; // where game_find looked
    GameRun run_;
    std::string play_error_; // why the last start failed, or how the game ended
    std::vector<std::string> play_log_; // the game's last lines when it ended with an error
    std::string play_report_path_, play_report_text_; // the crash report it wrote, and its text
    std::string play_command_, play_version_; // the run's command and the game's version line

    // The update check: the one started with the launcher prompts, Check now only reports.
    UpdateCheck update_;
    bool update_auto_ = false;
    bool update_prompt_ = false, update_prompt_open_ = false;
    std::function<bool(const std::string &)> open_url_;
    // Update now: the plan, the download, the step, the failure's message, the arguments the new launcher gets.
    UpdatePlan update_plan_;
    HttpDownload download_;
    UpdateStep update_step_ = UpdateStep::None;
    std::string update_message_;
    std::vector<std::string> args_;
    std::function<UpdateSystem()> update_system_;
    bool update_now_auto_ = false;
    SteamSection steam_;
};

} // namespace psxstack
