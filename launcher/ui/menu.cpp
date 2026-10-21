// The in-game menu (include/psxstack/menu.h; docs/LAUNCHER.md "In-game menu"): Dear ImGui inside the game's window,
// on the present path the window chose (SDL_Renderer, or SDL_GPU for the hardware renderer), shown while the pump
// holds the game paused. Its pages: Game (the title, Resume, Quit) and the launcher's shared screens (screens.h:
// Settings, Controls, Mods) over the settings file the game was started with (--config) and the mods beside the
// game, so a change made here is the same change the launcher would make; it takes effect at the next start. The
// runtime stays C: this file is the C++ behind the C API, compiled into the game only with PSXSTACK_MENU
// (cmake/psxstack.cmake).
//
// <PREFIX>_PORT_MENU_SHOT=PATH writes the window's output with the menu up, once, as a PNG: the second present after
// it is shown (the software renderer: the input self-test's picture; the hardware renderer's present cannot be read
// back with the menu in it, logged). <PREFIX>_PORT_MENU_SCREEN=game|settings|controls|mods picks the page it opens on
// (the picture of a page; default game).
#include "menu.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "psxstack_game_gen.h" // the game's title and env prefix (GAME_CONTRACT.md "The generated header")
#include "input.h"
#include "paths.h"
#include "screens.h"
#include "style.h"

extern "C" {
#include "port_harness.h"
#include "port_runtime.h"
#include "psxstack/settings.h" // the runtime's: the settings file the game was started with
}

namespace {

enum Page { PAGE_GAME, PAGE_SETTINGS, PAGE_CONTROLS, PAGE_MODS, PAGE_COUNT };
const char *const PAGE_NAMES[PAGE_COUNT] = { "Game", "Settings", "Controls", "Mods" };

struct Menu {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr; // the software path
    SDL_GPUDevice *device = nullptr;  // the hardware path
    bool open = false;
    bool shown = false;
    bool frame_built = false; // ImGui::Render ran since the menu was shown: the present may draw it
    bool focus_resume = false; // the next frame puts the keyboard/gamepad focus on Resume
    float scale = 0;
    std::string shot;      // <PREFIX>_PORT_MENU_SHOT
    bool shot_done = false;
    int drawn = 0;         // presents with the menu since it was shown (ImGui sizes a window on its first frame)
    psxstack::Screens screens;
    bool has_settings = false; // the game was started with --config: the shared screens have a file to edit
    bool from_launcher = false; // <PREFIX>_LAUNCHER: the launcher started the game and comes back when it quits
    int page = PAGE_GAME;
    int start_page = PAGE_GAME; // <PREFIX>_PORT_MENU_SCREEN
};

Menu mn;

std::string env_name(const char *suffix) {
    return std::string(PSXSTACK_GAME_ENV_PREFIX) + suffix;
}

bool menu_open_common(SDL_Window *window) {
    if (mn.open) {
        return false;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; // no imgui.ini: the layout is fixed
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.Fonts->AddFontDefaultVector();
    mn.window = window;
    mn.open = true;
    mn.scale = 0;
    if (const char *shot = SDL_getenv(env_name("_PORT_MENU_SHOT").c_str())) {
        mn.shot = shot;
    }
    mn.from_launcher = SDL_getenv(env_name("_LAUNCHER").c_str()) != nullptr;
    if (const char *page = SDL_getenv(env_name("_PORT_MENU_SCREEN").c_str())) {
        for (int i = 0; i < PAGE_COUNT; i++) {
            if (SDL_strcasecmp(page, PAGE_NAMES[i]) == 0) {
                mn.start_page = i;
            }
        }
    }
    // The shared screens: the settings file the game was started with (the runtime's settings.c read it), the
    // game's mods beside the executable (psxstack_add_game copies the manifests there) and the user's data mods
    // (--mods-dir).
    mn.has_settings = port_settings_loaded && port_settings.dir != nullptr;
    psxstack::SettingsDir location;
    if (mn.has_settings) {
        location.dir = psxstack::path_strip_slash(port_settings.dir);
        location.source = psxstack::DirSource::Config;
    }
    std::string exe_dir;
    if (const char *base = SDL_GetBasePath()) {
        exe_dir = psxstack::path_strip_slash(base);
    }
    const char *user_mods = port_mods_data_dir();
    mn.screens.load(location, exe_dir.empty() ? "" : psxstack::path_join(exe_dir, "mods"),
                    user_mods != nullptr ? user_mods : "", true);
    mn.screens.set_in_game(true);
    if (mn.has_settings) {
        port_log("menu: settings %s, mods beside %s%s%s", port_settings.file, exe_dir.c_str(),
                 user_mods != nullptr ? ", data mods in " : "", user_mods != nullptr ? user_mods : "");
    } else {
        port_log("menu: no settings file (started without --config): Resume and Quit only");
    }
    return true;
}

// The style at the window's size: 1 at 480 lines (the launcher's at 96 dpi), larger on a taller window, so the
// menu reads the same on a 640x480 window and a 1280x800 handheld screen.
void menu_style_for_window() {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(mn.window, &w, &h);
    float s = std::clamp(h / 480.0f, 1.0f, 4.0f);
    if (s != mn.scale) {
        mn.scale = s;
        psxstack::gui_style(s);
    }
}

// The Game page: the title, the state, Resume and Quit. Returns the pump's request.
int menu_draw_game() {
    int action = PORT_MENU_NONE;
    const float unit = ImGui::GetStyle().FontScaleDpi;
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.45f);
    ImGui::TextUnformatted(PSXSTACK_GAME_TITLE);
    ImGui::PopFont();
    ImGui::TextDisabled("Paused");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const ImVec2 button(std::min(360 * unit, ImGui::GetContentRegionAvail().x), ImGui::GetFrameHeight() * 1.3f);
    if (mn.focus_resume) {
        ImGui::SetKeyboardFocusHere();
        mn.focus_resume = false;
    }
    ImGui::PushStyleColor(ImGuiCol_Button, psxstack::rgb(psxstack::ACCENT, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, psxstack::rgb(psxstack::ACCENT));
    ImGui::PushStyleColor(ImGuiCol_Text, psxstack::rgb(0x15171C));
    if (ImGui::Button("Resume", button)) {
        action = PORT_MENU_RESUME;
    }
    ImGui::PopStyleColor(3);
    if (ImGui::Button(mn.from_launcher ? "Quit to launcher" : "Quit", button)) {
        action = PORT_MENU_QUIT;
    }
    ImGui::Spacing();
    // The hotkey's own name: the settings' binding, else the default (the screens' Settings know both).
    const psxstack::Binding menu_key = mn.screens.settings().values.hotkey_for("menu");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s returns to the game.", psxstack::binding_label(menu_key).c_str());
    ImGui::PopTextWrapPos();
    if (!mn.has_settings) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("The game was started without a settings file (--config), so there is nothing to change "
                            "here; the launcher starts it with one.");
        ImGui::PopTextWrapPos();
    }
    return action;
}

// The menu: the navigation on the left (the pages; the shoulder buttons step through them, as in the launcher),
// the page on the right, over the game's picture dimmed. Returns the pump's request.
int menu_draw() {
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                  IM_COL32(0, 0, 0, 150));
    const float unit = ImGui::GetStyle().FontScaleDpi;
    const int pages = mn.has_settings ? PAGE_COUNT : 1;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x * 0.96f, vp->Size.y * 0.94f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8 * unit);
    ImGui::Begin("##menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (pages > 1 && !ImGui::IsAnyItemActive()) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ||
            (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_PageDown, false))) {
            mn.page = (mn.page + 1) % pages;
        } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ||
                   (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_PageUp, false))) {
            mn.page = (mn.page + pages - 1) % pages;
        }
    }
    int action = PORT_MENU_NONE;
    if (pages > 1) {
        const float nav_w = 130 * unit;
        ImGui::BeginChild("nav", ImVec2(nav_w, 0), ImGuiChildFlags_None);
        for (int i = 0; i < pages; i++) {
            if (ImGui::Selectable(PAGE_NAMES[i], mn.page == i, 0, ImVec2(0, ImGui::GetFrameHeight() * 1.2f))) {
                mn.page = i;
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
    }
    ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    switch (mn.page) {
    case PAGE_SETTINGS:
        mn.screens.draw_settings();
        break;
    case PAGE_CONTROLS:
        mn.screens.draw_controls();
        break;
    case PAGE_MODS:
        mn.screens.draw_mods();
        break;
    default:
        action = menu_draw_game();
        break;
    }
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar();
    return action;
}

// The file the screens just wrote, applied by the runtime (runtime/live.c).
void menu_apply_saved() {
    if (mn.screens.take_saved()) {
        port_live_apply();
    }
}

bool menu_ready_to_draw() {
    return mn.open && mn.shown && mn.frame_built && ImGui::GetDrawData() != nullptr;
}

void menu_shot_renderer(SDL_Renderer *renderer) {
    if (mn.shot.empty() || mn.shot_done || mn.drawn < 2) {
        return; // the second present: ImGui draws an auto-sized window from its second frame on
    }
    mn.shot_done = true;
    SDL_Surface *s = SDL_RenderReadPixels(renderer, nullptr);
    if (s == nullptr || !SDL_SavePNG(s, mn.shot.c_str())) {
        port_log("menu: shot %s: %s", mn.shot.c_str(), SDL_GetError());
    } else {
        port_log("menu: shot: %dx%d -> %s", s->w, s->h, mn.shot.c_str());
    }
    SDL_DestroySurface(s);
}

} // namespace

extern "C" {

int port_menu_compiled(void) {
    return 1;
}

void port_menu_open_renderer(SDL_Window *window, SDL_Renderer *renderer) {
    if (!menu_open_common(window)) {
        return;
    }
    mn.renderer = renderer;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    port_log("menu: Dear ImGui %s on SDL_Renderer", IMGUI_VERSION);
}

void port_menu_open_gpu(SDL_Window *window, SDL_GPUDevice *device) {
    if (!menu_open_common(window)) {
        return;
    }
    mn.device = device;
    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo info;
    info.Device = device;
    info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
    info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&info);
    port_log("menu: Dear ImGui %s on SDL_GPU", IMGUI_VERSION);
    if (!mn.shot.empty()) {
        port_log("menu: shot: the software renderer's present only; none from the hardware renderer");
        mn.shot_done = true;
    }
}

void port_menu_close(void) {
    if (!mn.open) {
        return;
    }
    mn.screens.flush();
    if (mn.renderer != nullptr) {
        ImGui_ImplSDLRenderer3_Shutdown();
    } else {
        ImGui_ImplSDLGPU3_Shutdown();
    }
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    mn = Menu();
}

int port_menu_shown(void) {
    return mn.open && mn.shown;
}

void port_menu_show(int on) {
    if (!mn.open || mn.shown == (on != 0)) {
        return;
    }
    mn.shown = on != 0;
    mn.frame_built = false;
    mn.drawn = 0;
    mn.focus_resume = mn.shown;
    if (mn.shown) {
        mn.page = mn.has_settings ? mn.start_page : PAGE_GAME;
        // ImGui reads the gamepads itself, every one of them (the runtime ORs them all into the pad too).
        ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);
    } else {
        mn.screens.flush(); // a change still unsaved (a slider let go as the menu closed)
        if (!mn.screens.last_error().empty()) {
            port_log("menu: the settings could not be saved: %s", mn.screens.last_error().c_str());
        }
        menu_apply_saved();
    }
    port_log("menu: %s", mn.shown ? "shown" : "hidden");
}

void port_menu_event(const SDL_Event *e) {
    if (mn.open && mn.shown && !mn.screens.handle_event(*e)) {
        ImGui_ImplSDL3_ProcessEvent(e);
    }
}

int port_menu_frame(void) {
    if (!mn.open || !mn.shown) {
        return PORT_MENU_NONE;
    }
    menu_style_for_window();
    if (mn.renderer != nullptr) {
        ImGui_ImplSDLRenderer3_NewFrame();
    } else {
        ImGui_ImplSDLGPU3_NewFrame();
    }
    mn.screens.begin_frame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    int action = menu_draw();
    ImGui::Render();
    mn.screens.end_frame();
    menu_apply_saved();
    mn.frame_built = true;
    return action;
}

void port_menu_render_renderer(SDL_Renderer *renderer) {
    if (!menu_ready_to_draw() || renderer != mn.renderer) {
        return;
    }
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    mn.drawn++;
    menu_shot_renderer(renderer);
}

void port_menu_render_gpu(SDL_GPUCommandBuffer *cb, SDL_GPUTexture *swapchain) {
    if (!menu_ready_to_draw() || mn.device == nullptr || swapchain == nullptr) {
        return;
    }
    ImDrawData *dd = ImGui::GetDrawData();
    ImGui_ImplSDLGPU3_PrepareDrawData(dd, cb);
    SDL_GPUColorTargetInfo ct;
    SDL_zero(ct);
    ct.texture = swapchain;
    ct.load_op = SDL_GPU_LOADOP_LOAD; // over the game's picture
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cb, &ct, 1, nullptr);
    if (pass != nullptr) {
        ImGui_ImplSDLGPU3_RenderDrawData(dd, cb, pass);
        SDL_EndGPURenderPass(pass);
    }
    mn.drawn++;
}

} // extern "C"
