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

// The launcher's own version stamp (launcher/CMakeLists.txt, cmake/version.cmake): the Copy text's header.
extern "C" const char launcher_version[];
extern "C" const char launcher_commit[];

namespace psxstack {

static const char *const SCREEN_NAMES[] = { "Play", "Disc", "Settings", "Controls", "Mods" };
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
    *window = SDL_CreateWindow(WINDOW_TITLE, (int)(960 * scale), (int)(620 * scale),
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
    case Screen::Settings:
        screens_.draw_settings();
        break;
    case Screen::Controls:
        screens_.draw_controls();
        break;
    case Screen::Mods:
        screens_.draw_mods();
        break;
    case Screen::Count:
        break;
    }
    ImGui::EndChild();
    draw_status_bar();
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
