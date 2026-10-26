#include "app.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "paths.h"
#include "screens.h"
#include "style.h"
#include "widgets.h"

// The launcher's own version stamp (launcher/CMakeLists.txt, cmake/version.cmake): the Copy text's header
// (launcher_version is declared in update.h).
extern "C" const char launcher_commit[];

namespace psxstack {

static const char *const SCREEN_NAMES[] = { "Play", "Disc", "Video", "Graphics", "Audio", "Controls", "Mods", "System" };
static_assert(sizeof(SCREEN_NAMES) / sizeof(SCREEN_NAMES[0]) == (size_t)Screen::Count, "one name per screen");

const char *screen_name(Screen s) {
    return (int)s >= 0 && s < Screen::Count ? SCREEN_NAMES[(int)s] : "?";
}

bool gui_open(SDL_Window **window, SDL_Renderer **renderer, std::string *err) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        *err = std::string("SDL_Init: ") + SDL_GetError();
        return false;
    }
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0) {
        scale = 1;
    }
    *window = SDL_CreateWindow(WINDOW_TITLE, (int)(CANVAS_W * scale), (int)(CANVAS_H * scale),
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (*window == nullptr) {
        *err = std::string("SDL_CreateWindow: ") + SDL_GetError();
        return false;
    }
    *renderer = SDL_CreateRenderer(*window, nullptr);
    if (*renderer == nullptr) {
        *err = std::string("SDL_CreateRenderer: ") + SDL_GetError();
        SDL_DestroyWindow(*window);
        return false;
    }
    SDL_SetRenderVSync(*renderer, 1); // may fail (the offscreen driver): then the main loop's wait paces it
    SDL_SetWindowMinimumSize(*window, (int)(640 * scale), (int)(420 * scale));
    SDL_SetWindowPosition(*window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(*window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; // no imgui.ini: the layout is fixed
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    gui_style(scale);
    io.Fonts->AddFontDefaultVector();
    ImGui_ImplSDL3_InitForSDLRenderer(*window, *renderer);
    ImGui_ImplSDLRenderer3_Init(*renderer);
    return true;
}

void gui_close(SDL_Window *window, SDL_Renderer *renderer) {
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

App::App(SDL_Window *window, SDL_Renderer *renderer, const SettingsDir &location, const AppOptions &options)
    : window_(window), renderer_(renderer), settings_(screens_.settings()), location_(location) {
    std::string exe_dir;
    if (const char *base = SDL_GetBasePath()) {
        exe_dir = path_strip_slash(base);
    }
    game_ = game_find(options.game, exe_dir, &game_tried_);
    echo_game_ = options.echo_game;
    // The settings file and the mods: the game's beside it (mods/<id>/mod.json; port/CMakeLists.txt copies them
    // there), the user's data mods in the settings directory's mods/ (docs/LAUNCHER.md "Data mods"; the game gets
    // --mods-dir).
    screens_.load(location_, game_.empty() ? "" : path_join(path_dir(game_), "mods"),
                  location_.dir.empty() ? "" : game_mods_dir(location_.dir), !game_.empty());
    SDL_strlcpy(disc_input_, settings_.values.disc_path.c_str(), sizeof(disc_input_));
    // The update check at start (docs/LAUNCHER.md "Updates"): when the description names a repository, the setting is
    // on and --no-update-check was not given.
    open_url_ = options.open_url;
    update_.configure(options.update_repo, options.version);
    args_ = options.args;
    update_system_ = options.update_system;
    update_now_auto_ = options.update_now;
    if (update_.available()) {
        update_replan();
    }
    if (update_.available() && options.update_check && settings_.values.update_check) {
        update_.start();
        update_auto_ = true;
    }
    // The System screen's own parts (the in-game menu has neither): Updates when the game has a repository, Steam.
    steam_.configure(options.steam, &settings_, [this] {
        screens_.mark_dirty();
        screens_.flush();
    });
    screens_.set_system_extra([this](float label_w) {
        if (update_.available()) {
            draw_updates(label_w);
        }
        steam_.draw(label_w);
    });
    // First run, or the disc went away: start on the disc screen (docs/LAUNCHER.md "Launcher"). A disc set by hand (no
    // verified SHA-1, or its size changed) is checked right away.
    switch (disc_status()) {
    case DiscStatus::Unset:
    case DiscStatus::Missing:
        screen_ = Screen::Disc;
        break;
    case DiscStatus::Unverified:
        screen_ = Screen::Disc;
        check_.start(settings_.resolve(settings_.values.disc_path));
        break;
    case DiscStatus::Checking:
    case DiscStatus::Verified:
        break;
    }
}

void App::handle_event(const SDL_Event &e) {
    if (screens_.handle_event(e)) {
        return;
    }
    ImGui_ImplSDL3_ProcessEvent(&e);
    if (e.type == SDL_EVENT_QUIT ||
        (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(window_))) {
        quit_ = true;
    }
    // A file dropped on the window: a disc image (the fallback when the file dialog is not available).
    if (e.type == SDL_EVENT_DROP_FILE && e.drop.data != nullptr && !run_.running()) {
        screen_ = Screen::Disc;
        choose_disc(e.drop.data);
    }
}

// ---- the disc

DiscStatus App::disc_status() const {
    const Settings &s = settings_.values;
    if (check_.state() == DiscCheck::State::Running) {
        return DiscStatus::Checking;
    }
    if (s.disc_path.empty()) {
        return DiscStatus::Unset;
    }
    std::string bin, err;
    if (!disc_bin_path(settings_.resolve(s.disc_path), &bin, &err) || disc_file_size(bin) < 0) {
        return DiscStatus::Missing;
    }
    if (!disc_known(s.disc_sha1, disc_file_size(bin))) {
        return DiscStatus::Unverified;
    }
    return DiscStatus::Verified;
}

void App::choose_disc(const std::string &typed) {
    // A typed or pasted path may come with quotes or spaces around it; a relative one is taken from the current
    // directory (it is stored absolute).
    std::string path = typed;
    while (!path.empty() && (path.back() == ' ' || path.back() == '\t' || path.back() == '\n' || path.back() == '\r')) {
        path.pop_back();
    }
    size_t first = path.find_first_not_of(" \t");
    path = first == std::string::npos ? "" : path.substr(first);
    if (path.size() >= 2 && (path[0] == '"' || path[0] == '\'') && path.back() == path[0]) {
        path = path.substr(1, path.size() - 2);
    }
    if (path.empty()) {
        return;
    }
    if (!path_is_absolute(path)) {
        if (char *cwd = SDL_GetCurrentDirectory()) {
            path = path_join(cwd, path);
            SDL_free(cwd);
        }
    }
    SDL_strlcpy(disc_input_, path.c_str(), sizeof(disc_input_));
    disc_message_.clear();
    std::string bin, err;
    if (!disc_bin_path(path, &bin, &err)) {
        disc_message_ = err;
        disc_message_ok_ = false;
        return;
    }
    if (!path_is_file(bin)) {
        disc_message_ = "Not found: " + bin;
        disc_message_ok_ = false;
        return;
    }
    check_.start(path);
}

void App::update_disc_check() {
    DiscCheck::State before = check_.state();
    DiscCheck::State now = check_.poll();
    if (before != DiscCheck::State::Running || now == DiscCheck::State::Running) {
        return;
    }
    Settings &s = settings_.values;
    if (now == DiscCheck::State::Passed) {
        if (settings_.resolve(s.disc_path) != check_.path()) {
            s.disc_path = check_.path(); // a new disc: absolute (a relative one written by hand stays as it is)
        }
        s.disc_sha1 = check_.sha1();
        s.last_dir = path_dir(check_.path());
        screens_.mark_dirty();
        const PsxstackGameDisc *d = disc_by_sha1(check_.sha1());
        disc_message_ = std::string("This is ") + (d != nullptr ? d->label : "the disc") + ".";
        disc_message_ok_ = true;
    } else if (now == DiscCheck::State::Failed) {
        disc_message_ = check_.message();
        disc_message_ok_ = false;
        // The disc in the settings failed its check (changed on disk): forget its SHA-1, keep the path to show.
        if (settings_.resolve(s.disc_path) == check_.path() && !s.disc_sha1.empty()) {
            s.disc_sha1.clear();
            screens_.mark_dirty();
        }
    } else {
        disc_message_ = "Check cancelled.";
        disc_message_ok_ = false;
    }
}

void SDLCALL App::file_dialog_done(void *self, const char *const *files, int) {
    App *app = static_cast<App *>(self);
    std::lock_guard<std::mutex> lock(app->dialog_mutex_);
    app->dialog_done_ = true;
    if (files == nullptr) {
        app->dialog_error_ = SDL_GetError();
    } else if (files[0] != nullptr) {
        app->dialog_file_ = files[0];
    }
}

void App::open_file_dialog() {
    static const SDL_DialogFileFilter filters[] = {
        { "Disc image (.cue, .bin)", "cue;bin" },
        { "All files", "*" },
    };
    const Settings &s = settings_.values;
    std::string where = !s.last_dir.empty() ? s.last_dir
                        : !s.disc_path.empty() ? path_dir(settings_.resolve(s.disc_path))
                                               : std::string();
    {
        std::lock_guard<std::mutex> lock(dialog_mutex_);
        dialog_open_ = true;
        dialog_done_ = false;
        dialog_file_.clear();
        dialog_error_.clear();
    }
    SDL_ShowOpenFileDialog(file_dialog_done, this, window_, filters, 2, where.empty() ? nullptr : where.c_str(),
                           false);
}

// ---- the game

void App::play() {
    play_error_.clear();
    play_log_.clear();
    if (run_.running()) {
        return;
    }
    if (game_.empty()) {
        play_error_ = "The game was not found.";
        return;
    }
    screens_.mark_dirty();
    flush();
    if (!screens_.last_error().empty()) {
        play_error_ = "The settings could not be saved: " + screens_.last_error();
        return;
    }
    GameProbe probe = game_probe(game_, settings_.path());
    std::vector<std::string> args;
    switch (probe.result) {
    case GameProbe::Result::Valid:
        args = game_args(game_, settings_);
        break;
    case GameProbe::Result::NoConfig:
        play_error_ = "This game build is too old for the launcher: it has no --config. Rebuild it from the "
                      "current source (cmake --build build/port-sdl).";
        return;
    case GameProbe::Result::Invalid:
        play_error_ = "The game does not accept the settings file:\n" + probe.message;
        return;
    case GameProbe::Result::Failed:
        play_error_ = "The game could not be run: " + probe.message;
        return;
    }
    // The game's whole output to logs/last-run.log (the previous run's kept as last-run.1.log), its crash reports to
    // crashes/ (game_args passes --crash-dir).
    const std::string logs = game_log_dir(settings_.dir());
    std::string log_path;
    if (path_make_dir(logs, nullptr)) {
        log_path = path_join(logs, GAME_LOG_FILE);
        if (path_is_file(log_path)) {
            SDL_RenamePath(log_path.c_str(), path_join(logs, GAME_LOG_FILE_PREVIOUS).c_str());
        }
    }
    path_make_dir(game_crash_dir(settings_.dir()), nullptr);
    std::string err;
    if (!run_.start(args, settings_.dir(), &err, echo_game_, log_path)) {
        play_error_ = err;
        return;
    }
    play_report_path_.clear();
    play_report_text_.clear();
    play_command_ = run_.command();
    std::fprintf(stderr, "launcher: started %s\n", run_.command().c_str());
    SDL_HideWindow(window_);
}

// ---- the update check

bool App::open_url(const std::string &url) {
    std::fprintf(stderr, "launcher: opening %s\n", url.c_str());
    if (open_url_) {
        return open_url_(url);
    }
    return SDL_OpenURL(url.c_str());
}

void App::update_check_now() {
    if (update_step_ == UpdateStep::Downloading || update_step_ == UpdateStep::Installing) {
        return; // the release being installed stays the one checked
    }
    update_auto_ = false;
    update_.start();
}

void App::update_poll() {
    if (!update_.checking() || update_.poll() == UpdateCheck::State::Checking) {
        return;
    }
    std::fprintf(stderr, "launcher: update check (%s): %s%s\n", update_.repo().c_str(), update_.summary().c_str(),
                 update_.state() == UpdateCheck::State::NoCurl ? (" (" + update_.message() + ")").c_str() : "");
    // Only the check at start prompts, once, and not for the release the user skipped (or an older one).
    Version skipped;
    const bool is_skipped = version_parse(settings_.values.update_skipped, &skipped) &&
                            version_compare(update_.release().version, skipped) <= 0;
    if (update_auto_ && update_.state() == UpdateCheck::State::Newer && !is_skipped) {
        update_prompt_ = true;
        if (update_now_auto_ && update_now_offered()) {
            std::fprintf(stderr, "launcher: update: Update now, answered by %s\n", ENV_UPDATE_NOW);
            update_now();
        }
    }
    update_auto_ = false;
}

// ---- Update now: the download, the check, the swap, the relaunch (docs/LAUNCHER.md "Updates")

void App::update_replan() {
    if (location_.dir.empty()) {
        update_plan_ = UpdatePlan();
        update_plan_.reason = "There is no settings directory to download the release into.";
        return;
    }
    update_plan_ = psxstack::update_plan(update_system_ ? update_system_() : update_system(), location_.dir);
}

bool App::update_now_offered() const {
    return update_now_unavailable().empty();
}

std::string App::update_now_unavailable() const {
    if (!update_plan_.possible()) {
        return update_plan_.reason;
    }
    if (update_.state() == UpdateCheck::State::Newer && !update_.release().has_asset) {
        return "The release has no " + update_asset_name(version_text(update_.release().version)) + " package.";
    }
    return "";
}

void App::update_now() {
    if (update_step_ == UpdateStep::Downloading || update_step_ == UpdateStep::Installing || run_.running() ||
        update_.state() != UpdateCheck::State::Newer) {
        return; // never while the game runs; only for the release the check found
    }
    update_replan(); // the install may have changed since the start
    update_prompt_ = true; // the modal shows the steps, on any screen
    const std::string why = update_now_unavailable();
    if (!why.empty()) {
        update_fail("This build cannot update itself. " + why);
        return;
    }
    const ReleaseAsset &a = update_.release().asset;
    if (a.sha256.empty()) {
        update_fail("GitHub gives no SHA-256 digest for " + a.name +
                    ", so a download could not be verified: nothing was downloaded or changed.");
        return;
    }
    path_make_dir(location_.dir, nullptr);
    update_message_.clear();
    update_step_ = UpdateStep::Downloading;
    std::fprintf(stderr, "launcher: update: downloading %s (%lld bytes) to %s\n", a.url.c_str(), (long long)a.size,
                 update_plan_.download.c_str());
    download_.start(a.url, update_plan_.download, a.size);
}

void App::update_cancel() {
    if (update_step_ != UpdateStep::Downloading) {
        return;
    }
    download_.cancel();
    while (download_.poll() == HttpDownload::State::Running) {
        SDL_Delay(5); // the process was killed: its thread ends at once, and poll() removes the partial file
    }
    SDL_RemovePath(update_plan_.download.c_str());
    std::fprintf(stderr, "launcher: update: the download was cancelled\n");
    update_step_ = UpdateStep::None;
    update_prompt_ = false; // as Later
}

void App::update_fail(const std::string &message) {
    if (!update_plan_.download.empty()) {
        SDL_RemovePath(update_plan_.download.c_str()); // never a download left behind by a failure
    }
    update_step_ = UpdateStep::Failed;
    update_message_ = message;
    update_prompt_ = true;
    std::fprintf(stderr, "launcher: update: %s\n", message.c_str());
}

void App::update_step_poll() {
    if (update_step_ == UpdateStep::Downloading) {
        if (download_.poll() == HttpDownload::State::Running) {
            return;
        }
        const HttpResult &r = download_.result();
        if (r.status == HttpResult::Status::Cancelled) {
            update_step_ = UpdateStep::None;
        } else if (r.status != HttpResult::Status::Ok) {
            update_fail("The download failed (" + r.message + "); nothing was changed.");
        } else {
            std::fprintf(stderr, "launcher: update: downloaded %lld bytes\n", (long long)download_.received());
            update_step_ = UpdateStep::Installing; // a frame says so, then the next one installs
        }
        return;
    }
    if (update_step_ != UpdateStep::Installing) {
        return;
    }
    const std::string digest = "sha256:" + update_.release().asset.sha256;
    std::string err;
    if (!update_verify(update_plan_.download, digest, &err)) {
        update_fail("The download did not pass its check (" + err + "); nothing was changed.");
        return;
    }
    std::fprintf(stderr, "launcher: update: %s matches the release's SHA-256\n", update_plan_.download.c_str());
    flush(); // the settings on disk before the new launcher starts and reads them
    const UpdateResult res = update_apply(update_plan_, update_plan_.download, digest, args_);
    std::fprintf(stderr, "launcher: update: %s\n", res.message.c_str());
    if (res.exit_now) {
        // The new launcher runs: this one quits through the normal path (main() saves the settings).
        update_step_ = UpdateStep::None;
        update_prompt_ = false;
        quit_ = true;
        return;
    }
    if (!res.ok) {
        update_fail("The update did not happen: " + res.message + ".");
        return;
    }
    update_step_ = UpdateStep::Failed; // in place, but the new launcher did not start
    update_message_ = "The update is installed: " + res.message + ".";
}

void App::update_answer(UpdateAnswer a) {
    if (update_step_ == UpdateStep::Installing) {
        return;
    }
    if (update_step_ == UpdateStep::Downloading) {
        if (a != UpdateAnswer::UpdateNow) {
            update_cancel();
        }
        return;
    }
    switch (a) {
    case UpdateAnswer::UpdateNow:
        update_now();
        return; // the prompt stays: it shows the download
    case UpdateAnswer::ReleasePage:
        open_url(update_.release_page());
        break;
    case UpdateAnswer::Later:
        break;
    case UpdateAnswer::Skip:
        settings_.values.update_skipped = update_.release().tag;
        screens_.mark_dirty();
        break;
    }
    update_step_ = UpdateStep::None;
    update_message_.clear();
    update_prompt_ = false;
}

// The prompt: a modal over the Play screen, never while the game runs or a prompt for an input is open. Update now's
// steps show in it too, on whichever screen it was pressed.
void App::draw_update_prompt() {
    static const char *const title = "Update available";
    if (!update_prompt_ && !update_prompt_open_) {
        return;
    }
    const bool steps = update_step_ != UpdateStep::None;
    if (update_prompt_ && !update_prompt_open_ && (screen_ == Screen::Play || steps) && !run_.running() &&
        !screens_.capture().active() && !ImGui::IsPopupOpen(title)) {
        ImGui::OpenPopup(title);
        if (!steps) {
            std::fprintf(stderr, "launcher: update prompt: %s %s is available (you have %s)%s\n", GAME_TITLE,
                         version_text(update_.release().version).c_str(), update_.current().c_str(),
                         update_now_offered() ? "" : ("; no Update now: " + update_now_unavailable()).c_str());
        }
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        update_prompt_open_ = false;
        return;
    }
    update_prompt_open_ = true;
    const Release &r = update_.release();
    const float wrap = ImGui::GetFontSize() * 34.0f;
    ImGui::Text("%s %s is available (you have %s).", GAME_TITLE, version_text(r.version).c_str(),
                update_.current().c_str());
    ImGui::PushTextWrapPos(wrap);
    switch (update_step_) {
    case UpdateStep::None: {
        const bool offered = update_now_offered();
        if (offered) {
            ImGui::TextDisabled("Update now downloads %s, checks it and restarts the launcher.", r.asset.name.c_str());
        } else {
            ImGui::TextDisabled("This build cannot update itself. %s", update_now_unavailable().c_str());
            ImGui::TextDisabled("The release page has its packages and what changed.");
        }
        ImGui::Spacing();
        if (offered) {
            if (ImGui::IsWindowAppearing()) {
                ImGui::SetKeyboardFocusHere(); // the default: Enter
            }
            if (ImGui::Button("Update now")) {
                update_answer(UpdateAnswer::UpdateNow);
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Release page")) {
            update_answer(UpdateAnswer::ReleasePage);
        }
        ImGui::SameLine();
        if (ImGui::Button("Later") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            update_answer(UpdateAnswer::Later);
        }
        ImGui::SameLine();
        if (ImGui::Button("Skip this version")) {
            update_answer(UpdateAnswer::Skip);
        }
        break;
    }
    case UpdateStep::Downloading: {
        const double mib = 1024.0 * 1024.0;
        char label[96];
        SDL_snprintf(label, sizeof(label), "%.1f of %.1f MB", (double)download_.received() / mib,
                     (double)download_.expected_size() / mib);
        ImGui::TextDisabled("Downloading %s", r.asset.name.c_str());
        ImGui::ProgressBar(download_.progress(), ImVec2(wrap, 0), label);
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            update_cancel();
        }
        break;
    }
    case UpdateStep::Installing:
        ImGui::TextDisabled("Checking the download and installing it...");
        break;
    case UpdateStep::Failed:
        ImGui::PushStyleColor(ImGuiCol_Text, rgb(ERROR_RED));
        ImGui::TextUnformatted(update_message_.c_str());
        ImGui::PopStyleColor();
        ImGui::TextDisabled("The installed launcher is unchanged unless this says otherwise; the release page has the "
                            "packages.");
        ImGui::Spacing();
        if (ImGui::Button("Release page")) {
            update_answer(UpdateAnswer::ReleasePage);
        }
        ImGui::SameLine();
        if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            update_answer(UpdateAnswer::Later);
        }
        break;
    }
    ImGui::PopTextWrapPos();
    if (!update_prompt_) {
        ImGui::CloseCurrentPopup();
        update_prompt_open_ = false;
    }
    ImGui::EndPopup();
}

// The System screen's part (only when the description names a repository): the launcher's own, drawn through the
// shared screen's hook (Screens::set_system_extra), so the in-game menu has none of it.
void App::draw_updates(float label_w) {
    Settings &s = settings_.values;
    section_title("Updates");
    ImGui::BeginDisabled(!settings_.writable());
    ImGui::TextDisabled("At start");
    ImGui::SameLine(label_w);
    if (ImGui::Checkbox("Check for updates when the launcher starts", &s.update_check)) {
        screens_.mark_dirty();
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Version");
    ImGui::SameLine(label_w);
    ImGui::Text("%s", update_.current().c_str());
    ImGui::TextDisabled("Last check");
    ImGui::SameLine(label_w);
    std::string when;
    SDL_DateTime dt;
    if (update_.checked_at() != 0 && !update_.checking() && SDL_TimeToDateTime(update_.checked_at(), &dt, true)) {
        char buf[32];
        SDL_snprintf(buf, sizeof(buf), " (at %02d:%02d)", dt.hour, dt.minute);
        when = buf;
    }
    const bool bad = update_.state() == UpdateCheck::State::Failed || update_.state() == UpdateCheck::State::NoCurl;
    const bool newer = update_.state() == UpdateCheck::State::Newer;
    ImGui::PushStyleColor(ImGuiCol_Text,
                          bad ? rgb(ERROR_RED) : newer ? rgb(ACCENT) : ImGui::GetStyle().Colors[ImGuiCol_Text]);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted((update_.summary() + when).c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    if (!s.update_skipped.empty()) {
        ImGui::TextDisabled("Skipped");
        ImGui::SameLine(label_w);
        ImGui::Text("%s (not offered again; a newer release is)", s.update_skipped.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(!settings_.writable());
        if (ImGui::SmallButton("Forget")) {
            s.update_skipped.clear();
            screens_.mark_dirty();
        }
        ImGui::EndDisabled();
    }
    const bool stepping = update_step_ == UpdateStep::Downloading || update_step_ == UpdateStep::Installing;
    if (newer && !update_now_offered()) {
        ImGui::TextDisabled("Update");
        ImGui::SameLine(label_w);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("This build cannot update itself. %s", update_now_unavailable().c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::Indent(label_w);
    if (newer && update_now_offered()) {
        ImGui::BeginDisabled(stepping);
        if (ImGui::Button("Update now")) {
            update_answer(UpdateAnswer::UpdateNow);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(update_.checking() || stepping);
    if (ImGui::Button("Check now")) {
        update_check_now();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Release page")) {
        open_url(update_.release_page());
    }
    ImGui::Unindent(label_w);
}

void App::update_game() {
    if (!run_.running() || run_.poll()) {
        return;
    }
    // It ended: the launcher comes back.
    SDL_ShowWindow(window_);
    SDL_RaiseWindow(window_);
    int code = run_.exit_code();
    std::fprintf(stderr, "launcher: the game %s\n", game_exit_text(code).c_str());
    play_version_ = run_.version();
    if (code != 0) {
        play_error_ = "The game " + game_exit_text(code) + ".";
        const auto &lines = run_.lines();
        size_t from = lines.size() > 40 ? lines.size() - 40 : 0;
        play_log_.assign(lines.begin() + (long)from, lines.end());
        // The crash report it named (docs/PORT.md "Crash report"): its text goes into Copy.
        play_report_path_ = run_.report_path();
        play_report_text_.clear();
        if (!play_report_path_.empty()) {
            std::string err;
            if (!file_read(play_report_path_, &play_report_text_, &err)) {
                play_report_text_ = "(" + err + ")";
            } else if (play_report_text_.size() > 64 * 1024) {
                play_report_text_.resize(64 * 1024);
                play_report_text_ += "\n(truncated)\n";
            }
            std::fprintf(stderr, "launcher: crash report: %s\n", play_report_path_.c_str());
        }
        screen_ = Screen::Play;
    }
}

std::string App::play_copy_text() const {
    std::string text = std::string(LAUNCHER_NAME) + " " + launcher_version + " (" + launcher_commit + ") on " +
                       SDL_GetPlatform() + "\n";
    text += "game: " + game_ + (play_version_.empty() ? "" : " (version " + play_version_ + ")") + "\n";
    text += "command: " + play_command_ + "\n";
    text += "result: " + play_error_ + "\n";
    if (!play_report_path_.empty()) {
        text += "\n--- crash report: " + play_report_path_ + " ---\n" + play_report_text_;
        if (!play_report_text_.empty() && play_report_text_.back() != '\n') {
            text += "\n";
        }
    }
    text += "\n--- the last lines of the game's output ---\n";
    for (const std::string &l : play_log_) {
        text += l + "\n";
    }
    if (!run_.log_path().empty()) {
        text += "(the whole output: " + run_.log_path() + ")\n";
    }
    return text;
}

void App::flush() {
    screens_.flush();
}

const std::string &App::last_error() const {
    return error_.empty() ? screens_.last_error() : error_;
}

void App::step_screen(int delta) {
    int n = (int)Screen::Count;
    screen_ = (Screen)(((int)screen_ + delta + n) % n);
}

void App::frame(const char *screenshot) {
    update_disc_check();
    update_game();
    update_poll();
    update_step_poll();
    {
        std::lock_guard<std::mutex> lock(dialog_mutex_);
        if (dialog_done_) {
            dialog_open_ = dialog_done_ = false;
            if (!dialog_error_.empty()) {
                disc_message_ = "The file dialog is not available (" + dialog_error_ +
                                "). Drag the .cue or .bin onto this window, or type its path.";
                disc_message_ok_ = false;
            } else if (!dialog_file_.empty()) {
                choose_disc(dialog_file_);
            }
        }
    }
    screens_.begin_frame();
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    draw();
    ImGui::Render();
    screens_.end_frame();
    ImGuiIO &io = ImGui::GetIO();
    SDL_SetRenderScale(renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    SDL_SetRenderDrawColorFloat(renderer_, bg.x, bg.y, bg.z, 1.0f);
    SDL_RenderClear(renderer_);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
    if (screenshot != nullptr) {
        SDL_Surface *s = SDL_RenderReadPixels(renderer_, nullptr);
        if (s == nullptr || !SDL_SavePNG(s, screenshot)) {
            error_ = std::string("screenshot ") + screenshot + ": " + SDL_GetError();
        }
        SDL_DestroySurface(s);
    }
    SDL_RenderPresent(renderer_);
}

void App::draw() {
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    // The screens in turn: Ctrl+PageUp/PageDown, the gamepad's shoulder buttons (not while a widget is in use: the
    // shoulders also tweak a slider's speed).
    ImGuiIO &io = ImGui::GetIO();
    if (!ImGui::IsAnyItemActive()) {
        if ((io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) {
            step_screen(1);
        } else if ((io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) ||
                   ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) {
            step_screen(-1);
        }
    }

    const float status_h = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    const float nav_w = 190 * ImGui::GetStyle().FontScaleDpi;
    ImGui::BeginChild("nav", ImVec2(nav_w, -status_h), ImGuiChildFlags_None);
    draw_nav();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("page", ImVec2(0, -status_h), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.45f);
    ImGui::TextUnformatted(screen_name(screen_));
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();
    switch (screen_) {
    case Screen::Play:
        draw_play();
        break;
    case Screen::Disc:
        draw_disc();
        break;
    case Screen::Video:
        screens_.draw_video();
        break;
    case Screen::Graphics:
        screens_.draw_graphics();
        break;
    case Screen::Audio:
        screens_.draw_audio();
        break;
    case Screen::Controls:
        screens_.draw_controls();
        break;
    case Screen::Mods:
        screens_.draw_mods();
        break;
    case Screen::System:
        screens_.draw_system();
        break;
    case Screen::Count:
        break;
    }
    ImGui::EndChild();
    draw_status_bar();
    draw_update_prompt();
    ImGui::End();
}

void App::draw_nav() {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(ACCENT));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
    ImGui::Text("  %s", GAME_ID_UPPER);
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::TextDisabled("  PC port launcher");
    ImGui::Spacing();
    ImGui::Spacing();
    const float h = ImGui::GetFrameHeight() * 1.35f;
    for (int i = 0; i < (int)Screen::Count; i++) {
        char label[64];
        SDL_snprintf(label, sizeof(label), "  %s", SCREEN_NAMES[i]);
        if (ImGui::Selectable(label, (int)screen_ == i, ImGuiSelectableFlags_None, ImVec2(0, h))) {
            screen_ = (Screen)i;
        }
    }
}

void App::draw_status_bar() {
    ImGui::Spacing();
    if (!last_error().empty()) {
        ImGui::TextColored(rgb(ERROR_RED), "%s", last_error().c_str());
        return;
    }
    if (location_.dir.empty()) {
        ImGui::TextColored(rgb(ERROR_RED), "No settings directory: %s", location_.error.c_str());
        return;
    }
    text_fit_left("Settings: ", settings_.path());
}

static std::string disc_status_text(DiscStatus d) {
    switch (d) {
    case DiscStatus::Unset:
        return "not chosen yet";
    case DiscStatus::Missing:
        return "not found";
    case DiscStatus::Unverified:
        return "not checked";
    case DiscStatus::Checking:
        return "checking...";
    case DiscStatus::Verified:
        return disc_labels() + ", checked";
    }
    return "?";
}

void App::draw_play() {
    const Settings &s = settings_.values;
    const DiscStatus disc = disc_status();
    ImGui::TextWrapped("%s", GAME_ABOUT);
    ImGui::Spacing();
    if (fields_begin("play")) {
        const ImVec4 red = rgb(ERROR_RED);
        field("Disc", s.disc_path.empty() ? "not chosen yet" : settings_.resolve(s.disc_path));
        field("", disc_status_text(disc), disc == DiscStatus::Verified ? nullptr : &red);
        for (int i = 0; i < 2; i++) {
            const MemoryCard &c = s.memcard[i];
            field(i == 0 ? "Memory card 1" : "Memory card 2", c.present ? settings_.resolve(c.path) : "none");
        }
        field("Game", game_.empty() ? "not found" : game_, game_.empty() ? &red : nullptr);
        ImGui::EndTable();
    }
    ImGui::Spacing();
    const bool ready = disc == DiscStatus::Verified && !game_.empty() && !run_.running() && settings_.writable() &&
                       !location_.dir.empty();
    ImGui::BeginDisabled(!ready);
    ImGui::PushStyleColor(ImGuiCol_Button, rgb(ACCENT, ready ? 0.85f : 0.35f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgb(ACCENT));
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(0x15171C));
    if (ImGui::Button("Play", ImVec2(200 * ImGui::GetStyle().FontScaleDpi, ImGui::GetFrameHeight() * 1.6f))) {
        play();
    }
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    if (!ready) {
        if (run_.running()) {
            ImGui::TextDisabled("The game is running.");
        } else if (disc != DiscStatus::Verified) {
            ImGui::TextDisabled("Choose and check the disc first (the Disc screen).");
        } else if (game_.empty()) {
            ImGui::TextWrapped("The game's executable (%s) was not found. Put it beside the launcher, or start the "
                               "launcher with --game PATH (or $%s). Looked at:",
                               GAME_EXE, ENV_GAME);
            for (const std::string &t : game_tried_) {
                ImGui::BulletText("%s", t.c_str());
            }
        } else if (location_.dir.empty()) {
            ImGui::TextDisabled("There is no settings directory (the status bar says why).");
        } else {
            ImGui::TextDisabled("The settings file is from a newer launcher: this one does not start the game.");
        }
    }
    draw_play_error();
}

void App::draw_play_error() {
    if (play_error_.empty()) {
        return;
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(ERROR_RED));
    ImGui::TextWrapped("%s", play_error_.c_str());
    ImGui::PopStyleColor();
    if (play_log_.empty()) {
        return;
    }
    if (!play_report_path_.empty()) {
        ImGui::TextWrapped("Crash report: %s", play_report_path_.c_str());
    }
    ImGui::TextDisabled(play_report_path_.empty() ? "Its last lines:" : "Its last lines (Copy takes the report too):");
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) {
        ImGui::SetClipboardText(play_copy_text().c_str());
    }
    if (!play_report_path_.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Open folder")) {
            SDL_OpenURL(path_to_url(path_dir(play_report_path_)).c_str());
        }
    }
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    for (const std::string &l : play_log_) {
        ImGui::TextUnformatted(l.c_str());
    }
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

void App::draw_disc() {
    const Settings &s = settings_.values;
    const DiscStatus disc = disc_status();
    ImGui::TextWrapped("%s Nothing is unpacked or copied.", GAME_DISC_HINT);
    ImGui::Spacing();
    if (fields_begin("disc")) {
        const ImVec4 red = rgb(ERROR_RED);
        field("Disc", s.disc_path.empty() ? "not chosen yet" : settings_.resolve(s.disc_path));
        field("Status", disc_status_text(disc),
              disc == DiscStatus::Verified || disc == DiscStatus::Checking ? nullptr : &red);
        ImGui::EndTable();
    }
    ImGui::Spacing();

    if (check_.state() == DiscCheck::State::Running) {
        char label[64];
        SDL_snprintf(label, sizeof(label), "SHA-1 %.0f%%", check_.progress() * 100.0f);
        ImGui::TextDisabled("Checking %s", check_.path().c_str());
        ImGui::ProgressBar(check_.progress(), ImVec2(-1, 0), label);
        if (ImGui::Button("Cancel")) {
            check_.cancel();
        }
        return;
    }

    bool open_dialog;
    {
        std::lock_guard<std::mutex> lock(dialog_mutex_);
        open_dialog = dialog_open_;
    }
    ImGui::BeginDisabled(open_dialog);
    if (ImGui::Button("Choose a file...")) {
        open_file_dialog();
    }
    ImGui::EndDisabled();
    if (disc == DiscStatus::Unverified || (disc == DiscStatus::Verified && !s.disc_path.empty())) {
        ImGui::SameLine();
        if (ImGui::Button(disc == DiscStatus::Verified ? "Check again" : "Check")) {
            choose_disc(settings_.resolve(s.disc_path));
        }
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Or drag the .cue (or the .bin) onto this window, or type its path:");
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Use this path").x - ImGui::GetStyle().FramePadding.x * 2 -
                            ImGui::GetStyle().ItemSpacing.x);
    const std::string hint = std::string("/path/to/") + GAME_TITLE + ".cue";
    bool enter = ImGui::InputTextWithHint("##path", hint.c_str(), disc_input_, sizeof(disc_input_),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Use this path") || enter) {
        choose_disc(disc_input_);
    }
    if (!disc_message_.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, disc_message_ok_ ? rgb(0x6CC070) : rgb(ERROR_RED));
        ImGui::TextWrapped("%s", disc_message_.c_str());
        ImGui::PopStyleColor();
    }
}

} // namespace psxstack
