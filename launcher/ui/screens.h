// The screens the launcher's window (launcher/src/app.cpp) and the game's in-game menu (launcher/ui/menu.cpp) share:
// Settings, Controls and Mods, over the settings file (settings.h) and the mods' manifests (mods.h), with the
// controls' "press an input" prompt (input.h InputCapture). Both hosts drive it the same way: load() once,
// handle_event() for every SDL event before ImGui's backend sees it, begin_frame() before ImGui::NewFrame, a draw_*
// per frame, end_frame() after ImGui::Render (the save, once no widget is in use).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "input.h"
#include "mods.h"
#include "settings.h"

namespace psxstack {

class Screens {
public:
    // A binding somewhere in the settings (a hotkey, a mod's binding option): the conflict marks look at all of them.
    struct BindingUse {
        std::string key;   // "hotkey:<action>", "mod:<mod>.<option>"
        std::string label; // "the hotkey Pause", "Fast-forward: Hold"
        Binding binding;
    };

    // The settings file in `location` (read now), the game's mods in `mods_dir` and the user's data mods in
    // `user_mods_dir` ("" for none); `game_found` only words the Mods screen's message when there are none.
    void load(const SettingsDir &location, const std::string &mods_dir, const std::string &user_mods_dir,
              bool game_found);
    // In the game's menu: every screen says that changes take effect at the next start.
    void set_in_game(bool on) { in_game_ = on; }
    // A part of the Settings screen its host draws itself, after "Memory cards" (the launcher's Updates); given the
    // label column's width. The in-game menu sets none.
    void set_settings_extra(std::function<void(float label_w)> draw) { settings_extra_ = std::move(draw); }

    // True when the event belonged to the controls' prompt (it then does not reach ImGui).
    bool handle_event(const SDL_Event &e);
    void begin_frame();
    void end_frame();
    // Saves the settings now if they changed; the last failure stays in last_error().
    void flush();
    void mark_dirty() { dirty_ = true; }
    const std::string &last_error() const { return error_; }
    // True once after a save wrote the file (the in-game menu then applies it live).
    bool take_saved() {
        bool s = saved_;
        saved_ = false;
        return s;
    }

    void draw_settings();
    void draw_controls();
    void draw_mods();

    SettingsFile &settings() { return settings_; }
    const SettingsFile &settings() const { return settings_; }
    // The controls' prompt (the self-test drives it with events).
    InputCapture &capture() { return capture_; }
    void capture_for(int section, const std::string &id); // 0 keyboard, 1 gamepad (button ids), 2 hotkeys (actions)
    void capture_for_mod(const std::string &mod, const std::string &option);
    // The Controls screen's tab ("keyboard", "gamepad" or "hotkeys"), selected at its next frame.
    void show_controls_tab(const std::string &tab) { controls_tab_ = tab; }
    // The mods found: the game's, then the user's data mods.
    const std::vector<ModManifest> &mods() const { return mods_; }
    const std::string &mods_dir() const { return mods_dir_; }
    const std::string &user_mods_dir() const { return user_mods_dir_; }
    // The usable data mods' ids, the game's and the user's (their priority: ModValues::data_order).
    std::vector<std::string> data_mod_ids() const;
    void select_mod(const std::string &id);

private:
    void draw_restart_note();
    void restart_mark(float indent = 0); // "(restart)" after the last item, in the game's menu
    void draw_filter(float label_w);
    void draw_names_table(bool pad);
    void draw_players();
    void draw_hotkeys();
    void draw_capture_popup();
    void draw_data_mod(const ModManifest &m, ModValues &values);
    void draw_mod(const ModManifest &m);
    void draw_mod_combo(const std::vector<const ModManifest *> &list, const std::vector<std::string> &order,
                        ModValues &values);
    bool draw_mod_option(const ModManifest &m, const ModOption &o, ModValues &values,
                         const std::vector<BindingUse> &uses, bool stacked);
    std::vector<BindingUse> binding_uses() const;
    // A binding's chips (x removes one) and its + button (want_capture_ set when pressed). True when changed.
    bool binding_editor(const std::string &key, Binding *b, const std::vector<BindingUse> &uses);
    const ModManifest *find_mod(const std::string &id) const;
    void begin_capture(InputCapture::Kind kind, int section, const std::string &id, const std::string &label);
    void finish_capture();

    SettingsDir location_;
    SettingsFile settings_;
    bool dirty_ = false;
    bool saved_ = false;
    bool in_game_ = false;
    std::string controls_tab_; // show_controls_tab's, until the Controls screen is drawn
    std::function<void(float)> settings_extra_;
    std::string error_; // the last save error

    // The controls: the "press a key" prompt and what it is for (section 0 keyboard, 1 gamepad, 2 hotkeys).
    InputCapture capture_;
    int capture_section_ = 0;
    std::string capture_id_, capture_label_;
    bool capture_popup_ = false;  // the prompt is to be opened this frame
    bool pad_nav_hold_ = false;   // a gamepad press just went to a capture: no gamepad navigation until released
    bool want_capture_ = false;   // binding_editor's + was pressed
    std::string capture_option_;  // section 3 (a mod's binding option): capture_id_ is the mod

    // The mods: the manifests beside the game and the data mods in the settings directory's mods/, the one shown.
    bool game_found_ = false;
    std::string mods_dir_, user_mods_dir_;
    std::vector<ModManifest> mods_;
    std::string mod_selected_;
};

} // namespace psxstack
