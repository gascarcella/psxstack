// The screens the launcher's window and the game's in-game menu share (screens.h): Settings, Controls and Mods over
// the settings file and the mods' manifests.
#include "screens.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>

#include "paths.h"
#include "style.h"
#include "widgets.h"

namespace psxstack {

// True while any button of an open gamepad is held or an axis is past a third of its range.
static bool any_gamepad_input_held() {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    bool held = false;
    for (int i = 0; i < n && !held; i++) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]);
        if (g == nullptr) {
            continue;
        }
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && !held; b++) {
            held = SDL_GetGamepadButton(g, (SDL_GamepadButton)b);
        }
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT && !held; a++) {
            int v = SDL_GetGamepadAxis(g, (SDL_GamepadAxis)a);
            held = a >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? v > 10000 : (v > 10000 || v < -10000);
        }
    }
    SDL_free(ids);
    return held;
}

// ---- the app


// A setting's label column and its widget's width: the launcher's 150 and 260 points, less on a narrow page (the
// in-game menu over a 640x480 window).
static float label_width() {
    return std::min(150 * ImGui::GetStyle().FontScaleDpi, ImGui::GetContentRegionAvail().x * 0.34f);
}

static float item_width(float label_w) {
    return std::min(260 * ImGui::GetStyle().FontScaleDpi,
                    ImGui::GetContentRegionAvail().x - label_w - ImGui::GetStyle().ItemSpacing.x);
}

static void section_title(const char *title) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(ACCENT));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
}

// The hardware renderer's present filter (video.filter; under the GPU renderer only: the Software one shows the
// picture unfiltered): a combo in FILTER_NAMES' order, the filter's sliders (video.crt), a line of help for the choice.
void Screens::draw_filter(float label_w) {
    static const char *const labels[] = { "None (the PS1's pixels)", "Sharp", "Scanlines", "CRT", "Smooth (2D)" };
    static const char *const help[] = {
        "Each pixel of the picture as a block of whole window pixels, as it is.",
        "Sharp bilinear: every pixel the same size where the window is not a whole multiple of the picture (a "
        "Resolution that does not divide the window), with a one-pixel blend at the edges between them.",
        "Sharp, with each of the PS1's lines darker towards its edges, as on a TV. Needs a window at least twice as "
        "tall as the picture's lines.",
        "A TV: the lines as glowing beams, the screen's red, green and blue stripes, and a curved screen if you want "
        "one. Softer than the others.",
        "Rounds the stair steps of the pixel art (xBR). It always smooths the x1 picture, so the Resolution does not "
        "apply to it.",
    };
    static_assert(SDL_arraysize(labels) == SDL_arraysize(FILTER_NAMES) && SDL_arraysize(help) == SDL_arraysize(labels),
                  "a label and a help line per filter");
    Settings &s = settings_.values;
    int current = 0;
    for (int i = 0; i < (int)SDL_arraysize(FILTER_NAMES); i++) {
        if (s.filter == FILTER_NAMES[i]) {
            current = i;
        }
    }
    ImGui::TextDisabled("Filter");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(item_width(label_w));
    if (ImGui::BeginCombo("##filter", labels[current])) {
        for (int i = 0; i < (int)SDL_arraysize(FILTER_NAMES); i++) {
            if (ImGui::Selectable(labels[i], i == current)) {
                s.filter = FILTER_NAMES[i];
                dirty_ = true;
            }
        }
        ImGui::EndCombo();
    }
    // video.crt's sliders, for the filters that read them
    const bool lines = s.filter == "scanlines" || s.filter == "crt", crt = s.filter == "crt";
    struct {
        bool shown;
        const char *label, *id;
        int *value;
    } sliders[] = { { lines, "Scanlines", "##crt_scanlines", &s.crt_scanlines },
                    { crt, "Mask", "##crt_mask", &s.crt_mask },
                    { crt, "Curvature", "##crt_curvature", &s.crt_curvature } };
    for (auto &sl : sliders) {
        if (sl.shown) {
            ImGui::TextDisabled("%s", sl.label);
            ImGui::SameLine(label_w);
            ImGui::SetNextItemWidth(item_width(label_w));
            dirty_ |= ImGui::SliderInt(sl.id, sl.value, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
        }
    }
    ImGui::Indent(label_w);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", help[current]);
    ImGui::PopTextWrapPos();
    ImGui::Unindent(label_w);
}

void Screens::draw_settings() {
    Settings &s = settings_.values;
    const float label_w = label_width();
    draw_restart_note();
    ImGui::BeginDisabled(!settings_.writable());

    section_title("Video");
    ImGui::TextDisabled("Window size");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(item_width(label_w));
    char size[64];
    SDL_snprintf(size, sizeof(size), "x%d  (%d x %d)", s.scale, 320 * s.scale, 240 * s.scale);
    dirty_ |= ImGui::SliderInt("##scale", &s.scale, 1, 16, size, ImGuiSliderFlags_AlwaysClamp);
    ImGui::TextDisabled("Fullscreen");
    ImGui::SameLine(label_w);
    dirty_ |= ImGui::Checkbox("##fullscreen", &s.fullscreen);
    ImGui::SameLine();
    ImGui::TextDisabled("(F11 in the game, by default)");
    // The rates the game offers (brand.h GAME_RATES): the nominal one named by its TV standard; one rate: no choice.
    if (GAME_RATE_COUNT > 1) {
        ImGui::TextDisabled("Refresh");
        ImGui::SameLine(label_w);
        for (int i = 0; i < GAME_RATE_COUNT; i++) {
            const int rate = GAME_RATES[i];
            char label[32];
            SDL_snprintf(label, sizeof(label), "%d Hz%s", rate,
                         rate != GAME_RATE ? "" : rate == 50 ? " (PAL)" : " (NTSC)");
            if (i > 0) {
                ImGui::SameLine();
            }
            if (ImGui::RadioButton(label, s.refresh == rate)) {
                s.refresh = rate;
                dirty_ = true;
            }
        }
        restart_mark();
    }
    ImGui::TextDisabled("Renderer");
    ImGui::SameLine(label_w);
    if (ImGui::RadioButton("Software", s.renderer == "software")) {
        s.renderer = "software";
        dirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("GPU (experimental)", s.renderer == "gpu")) {
        s.renderer = "gpu";
        dirty_ = true;
    }
    restart_mark();
    if (s.renderer == "gpu") {
        char res[64];
        ImGui::TextDisabled("Resolution");
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(item_width(label_w));
        SDL_snprintf(res, sizeof(res), "x%d  (%d x %d)", s.internal_scale, 320 * s.internal_scale,
                     240 * s.internal_scale);
        dirty_ |= ImGui::SliderInt("##internal_scale", &s.internal_scale, 1, 8, res, ImGuiSliderFlags_AlwaysClamp);
        restart_mark();
        ImGui::Indent(label_w);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("SDL_GPU (Vulkan; Direct3D 12 on Windows). x1 is the Software picture, pixel for pixel; above it the game is drawn "
                            "at that resolution, without dithering, in 8-bit colour. Without a usable GPU the game "
                            "falls back to Software.");
        ImGui::PopTextWrapPos();
        ImGui::Unindent(label_w);
    }
    if (s.renderer == "gpu" && s.internal_scale > 1) {
        // video.subpixel (docs/LAUNCHER.md): the 3D at the GTE's sub-pixel positions, above x1 only.
        ImGui::TextDisabled("3D vertices");
        ImGui::SameLine(label_w);
        if (ImGui::RadioButton("Sub-pixel", s.subpixel == "on")) {
            s.subpixel = "on";
            dirty_ = true;
        }
        ImGui::SameLine();
        ImGui::SameLine();
        if (ImGui::RadioButton("Perspective", s.subpixel == "perspective")) {
            s.subpixel = "perspective";
            dirty_ = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("PS1 pixels", s.subpixel == "off")) {
            s.subpixel = "off";
            dirty_ = true;
        }
        ImGui::Indent(label_w);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Sub-pixel: the 3D moves smoothly. Perspective: also textured perspective-correct (not the "
                            "PS1's look). PS1 pixels: the 3D steps a whole PS1 pixel at a time.");
        ImGui::PopTextWrapPos();
        ImGui::Unindent(label_w);
    }
    if (s.renderer == "gpu") {
        draw_filter(label_w);
    }
    if (s.refresh != GAME_RATE && GAME_RATE_NOTE[0] != '\0') {
        ImGui::Indent(label_w);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", GAME_RATE_NOTE);
        ImGui::PopTextWrapPos();
        ImGui::Unindent(label_w);
    }

    section_title("Audio");
    ImGui::TextDisabled("Mute");
    ImGui::SameLine(label_w);
    dirty_ |= ImGui::Checkbox("##mute", &s.mute);

    section_title("Memory cards");
    if (in_game_) {
        ImGui::TextDisabled("(restart)");
        ImGui::SetItemTooltip("Takes effect when the game starts again");
    }
    for (int i = 0; i < 2; i++) {
        MemoryCard &c = s.memcard[i];
        ImGui::PushID(i);
        ImGui::TextDisabled("Slot %d", i + 1);
        ImGui::SameLine(label_w);
        dirty_ |= ImGui::Checkbox("##in", &c.present);
        ImGui::SameLine();
        ImGui::BeginDisabled(!c.present);
        char name[512];
        SDL_strlcpy(name, c.path.c_str(), sizeof(name));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##file", name, sizeof(name))) {
            if (name[0] != '\0') {
                c.path = name;
                dirty_ = true;
            }
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::Indent(label_w);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("A .mcd card image, relative to the settings directory; the game creates a formatted card when "
                        "the file is missing. Unticked: no card in the slot.");
    ImGui::PopTextWrapPos();
    ImGui::Unindent(label_w);
    ImGui::EndDisabled();

    section_title("Settings file");
    if (fields_begin("where")) {
        const ImVec4 red = rgb(ERROR_RED);
        field("Directory", location_.dir.empty() ? "(none)" : location_.dir);
        field("Chosen by", dir_source_name(location_.source));
        switch (settings_.state()) {
        case SettingsFile::State::New:
            field("File", "not written yet (the defaults)");
            break;
        case SettingsFile::State::Loaded:
            field("File", "read");
            break;
        case SettingsFile::State::Broken:
            field("File", "unreadable: the defaults are used", &red);
            break;
        case SettingsFile::State::Newer:
            field("File", "from a newer launcher: read only", &red);
            break;
        }
        ImGui::EndTable();
    }
    for (const std::string &m : settings_.messages()) {
        ImGui::Bullet();
        ImGui::TextWrapped("%s", m.c_str());
    }
    if (!location_.dir.empty() && ImGui::Button("Open the directory")) {
        SDL_OpenURL(path_to_url(location_.dir).c_str());
    }
}

// ---- the mods

const ModManifest *Screens::find_mod(const std::string &id) const {
    for (const ModManifest &m : mods_) {
        if (m.id == id) {
            return &m;
        }
    }
    return nullptr;
}

void Screens::select_mod(const std::string &id) {
    mod_selected_ = id;
}

void Screens::capture_for_mod(const std::string &mod, const std::string &option) {
    const ModManifest *m = find_mod(mod);
    const ModOption *o = m != nullptr ? m->option(option) : nullptr;
    if (o == nullptr || o->type != ModOption::Type::Binding) {
        return;
    }
    begin_capture(InputCapture::Kind::Binding, 3, mod, m->name + ": " + o->name);
    capture_option_ = option;
}

std::vector<std::string> Screens::data_mod_ids() const {
    std::vector<std::string> ids;
    for (const ModManifest &m : mods_) {
        if (m.kind == "data" && m.error.empty()) {
            ids.push_back(m.id);
        }
    }
    return ids;
}

void Screens::draw_mods() {
    draw_restart_note();
    if (mods_.empty()) {
        ImGui::TextWrapped("%s", !game_found_ && user_mods_dir_.empty()
                                     ? "The game was not found, so neither were its mods (the Play screen says where "
                                       "the launcher looked)."
                                     : ("No mods were found" + (!game_found_ ? "" : " in " + mods_dir_) +
                                        (user_mods_dir_.empty() ? "" : "; data mods (texture packs) go in " +
                                                                           user_mods_dir_) + ".")
                                           .c_str());
        return;
    }
    ModValues values(&settings_.doc);
    // The list: the mods that are not usable data mods (built-in ones, broken ones) as found, then the data mods in
    // their priority order.
    std::vector<const ModManifest *> list;
    for (const ModManifest &m : mods_) {
        if (m.kind != "data" || !m.error.empty()) {
            list.push_back(&m);
        }
    }
    const std::vector<std::string> order = values.data_order(data_mod_ids());
    for (const std::string &id : order) {
        list.push_back(find_mod(id));
    }
    const float list_w = std::min(230 * ImGui::GetStyle().FontScaleDpi, ImGui::GetContentRegionAvail().x * 0.4f);
    ImGui::BeginChild("modlist", ImVec2(list_w, 0), ImGuiChildFlags_Borders);
    ImGui::BeginDisabled(!settings_.writable());
    for (const ModManifest *mp : list) {
        const ModManifest &m = *mp;
        if (!order.empty() && &m == find_mod(order[0])) {
            ImGui::SeparatorText("Data mods");
            ImGui::SetItemTooltip("Texture packs, the first in this list winning where two replace the same "
                                  "texture (Earlier/Later on a mod's page). In %s",
                                  user_mods_dir_.c_str());
        }
        ImGui::PushID(m.id.c_str());
        bool on = values.enabled(m.id);
        ImGui::BeginDisabled(!m.error.empty());
        if (ImGui::Checkbox("##on", &on)) {
            values.set_enabled(m.id, on);
            dirty_ = true;
        }
        ImGui::SetItemTooltip(on ? "On: the game starts with this mod" : "Off");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Selectable(m.name.c_str(), mod_selected_ == m.id)) {
            mod_selected_ = m.id;
        }
        if (!m.error.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(rgb(ERROR_RED), "!");
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("mod", ImVec2(0, 0));
    if (const ModManifest *m = find_mod(mod_selected_)) {
        draw_mod(*m);
    }
    ImGui::EndChild();
    draw_capture_popup();
}

// A data mod's page below its name and switch: what it holds, its priority among the data mods, where it is.
void Screens::draw_data_mod(const ModManifest &m, ModValues &values) {
    const std::vector<std::string> ids = data_mod_ids();
    const std::vector<std::string> order = values.data_order(ids);
    const size_t at = (size_t)(std::find(order.begin(), order.end(), m.id) - order.begin());
    ImGui::Spacing();
    ImGui::TextWrapped("A texture pack: the PNGs in %s/ replace the game's textures (%s filtering). Only the GPU "
                       "renderer (Video) draws them.",
                       m.textures_dir.c_str(), m.textures_filter.c_str());
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Priority %zu of %zu", at + 1, order.size());
    ImGui::SetItemTooltip("Where two data mods replace the same texture, the earlier one wins.");
    ImGui::BeginDisabled(!settings_.writable());
    ImGui::SameLine();
    ImGui::BeginDisabled(at == 0);
    if (ImGui::Button("Earlier")) {
        values.move(ids, m.id, -1);
        dirty_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(at + 1 >= order.size());
    if (ImGui::Button("Later")) {
        values.move(ids, m.id, 1);
        dirty_ = true;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", m.dir.c_str());
    ImGui::PopStyleColor();
}

void Screens::draw_mod(const ModManifest &m) {
    ModValues values(&settings_.doc);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2f);
    ImGui::TextUnformatted(m.name.c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("%s%s", m.version.empty() ? "" : "v", m.version.c_str());
    if (!m.description.empty()) {
        ImGui::TextWrapped("%s", m.description.c_str());
    }
    if (!m.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, rgb(ERROR_RED));
        ImGui::TextWrapped("This mod cannot be used: %s", m.error.c_str());
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("%s", path_join(m.dir, "mod.json").c_str());
        ImGui::PopStyleColor();
        return;
    }
    bool on = values.enabled(m.id);
    ImGui::BeginDisabled(!settings_.writable());
    if (ImGui::Checkbox("Enabled", &on)) {
        values.set_enabled(m.id, on);
        dirty_ = true;
    }
    ImGui::EndDisabled();
    if (m.restart) {
        restart_mark();
    }
    if (m.kind == "data") {
        draw_data_mod(m, values);
        return;
    }
    if (m.options.empty()) {
        return;
    }
    // The presets: a button each, the one whose values are all in place shown pressed.
    if (!m.presets.empty()) {
        ImGui::BeginDisabled(!settings_.writable());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Presets");
        for (const ModPreset &p : m.presets) {
            ImGui::SameLine();
            const bool active = values.preset_active(m, p);
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            }
            if (ImGui::Button(p.name.c_str())) {
                values.apply_preset(m, p);
                dirty_ = true;
            }
            if (active) {
                ImGui::PopStyleColor();
            }
            if (!p.description.empty()) {
                ImGui::SetItemTooltip("%s", p.description.c_str());
            }
        }
        ImGui::EndDisabled();
    }
    // The options: the ungrouped ones first, then each group in the order the manifest names it.
    std::vector<std::string> groups = { "" };
    for (const ModOption &o : m.options) {
        if (std::find(groups.begin(), groups.end(), o.group) == groups.end()) {
            groups.push_back(o.group);
        }
    }
    const std::vector<BindingUse> uses = binding_uses();
    ImGui::BeginDisabled(!settings_.writable());
    for (const std::string &g : groups) {
        bool any = false;
        for (const ModOption &o : m.options) {
            any |= o.group == g;
        }
        if (!any) {
            continue;
        }
        ImGui::Spacing();
        if (!g.empty()) {
            ImGui::SeparatorText(g.c_str());
        } else {
            ImGui::Separator();
        }
        if (!ImGui::BeginTable(("opts" + g).c_str(), 3, ImGuiTableFlags_SizingFixedFit)) {
            continue;
        }
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("default", ImGuiTableColumnFlags_WidthFixed);
        for (const ModOption &o : m.options) {
            if (o.group == g && draw_mod_option(m, o, values, uses)) {
                dirty_ = true;
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    // Values in the file that the manifest rejects (a hand edit, an older manifest): said once, the default shown.
    for (const ModOption &o : m.options) {
        std::string err;
        if (values.stored(m, o, &err) == nullptr && !err.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, rgb(ERROR_RED));
            ImGui::TextWrapped("%s", err.c_str());
            ImGui::PopStyleColor();
        }
    }
    if (m.requires_port > 0) {
        ImGui::Spacing();
        ImGui::TextDisabled("Built into the game (mod interface %d).", m.requires_port);
    }
}

// A typed number's range, shown beside it ("" for a slider or an unbounded number).
static std::string typed_range(const ModOption &o, bool typed) {
    char text[64] = "";
    if (typed && o.has_min && o.has_max) {
        SDL_snprintf(text, sizeof(text), "%g to %g", o.min, o.max);
    }
    return text;
}

bool Screens::draw_mod_option(const ModManifest &m, const ModOption &o, ModValues &values,
                          const std::vector<BindingUse> &uses) {
    bool changed = false;
    ImGui::PushID(o.id.c_str());
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(o.name.c_str());
    if (!o.description.empty()) {
        ImGui::SetItemTooltip("%s", o.description.c_str());
    }
    if (o.restart) {
        ImGui::SameLine();
        ImGui::TextDisabled("(restart)");
        ImGui::SetItemTooltip("Changing it in the game takes effect at the next start");
    }
    ImGui::TableNextColumn();
    const Json v = values.value(m, o);
    const float w = std::min(260 * ImGui::GetStyle().FontScaleDpi, ImGui::GetContentRegionAvail().x); // in the column
    // A number's width, leaving room for its range beside it when it is typed.
    auto number_width = [&](const std::string &range) {
        return range.empty() ? w : w - ImGui::CalcTextSize(range.c_str()).x - ImGui::GetStyle().ItemSpacing.x;
    };
    switch (o.type) {
    case ModOption::Type::Bool: {
        bool b = v.as_bool(false);
        if (ImGui::Checkbox("##v", &b)) {
            values.set(m, o, Json::boolean(b));
            changed = true;
        }
        break;
    }
    case ModOption::Type::Int: {
        // With an input_toggle: a slider up to slider_max while it is off, a plain typed number while it is on.
        const bool typed = values.typed(m, o), slider = o.has_min && o.has_max && !typed;
        const double top = slider && o.has_slider_max ? o.slider_max : o.max;
        int x = (int)values.effective(m, o).as_number(0);
        const std::string range = typed_range(o, typed);
        ImGui::SetNextItemWidth(number_width(range));
        bool edited = slider ? ImGui::SliderInt("##v", &x, (int)o.min, (int)top, "%d", ImGuiSliderFlags_AlwaysClamp)
                             : ImGui::InputInt("##v", &x, typed ? 0 : o.step > 0 ? (int)o.step : 1);
        if (!range.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", range.c_str());
        }
        if (edited) {
            if (o.has_min && x < o.min) {
                x = (int)o.min;
            }
            if (o.has_max && x > top) {
                x = (int)top;
            }
            values.set(m, o, Json::number(x));
            changed = true;
        }
        break;
    }
    case ModOption::Type::Float: {
        const bool typed = values.typed(m, o), slider = o.has_min && o.has_max && !typed;
        const double top = slider && o.has_slider_max ? o.slider_max : o.max;
        float x = (float)values.effective(m, o).as_number(0);
        const std::string range = typed_range(o, typed);
        ImGui::SetNextItemWidth(number_width(range));
        const char *fmt = o.step >= 1 ? "%.0f" : o.step >= 0.1 ? "%.1f" : o.step >= 0.01 ? "%.2f" : "%.3f";
        bool edited = slider ? ImGui::SliderFloat("##v", &x, (float)o.min, (float)top, fmt, ImGuiSliderFlags_AlwaysClamp)
                      : ImGui::InputFloat("##v", &x, typed ? 0 : (float)o.step, typed ? 0 : (float)o.step * 10, fmt);
        if (!range.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", range.c_str());
        }
        if (edited) {
            double d = x;
            if (o.step > 0) {
                d = (o.has_min ? o.min : 0) + std::round((d - (o.has_min ? o.min : 0)) / o.step) * o.step;
                d = std::round(d * 1e9) / 1e9; // 1 + 15 * 0.1 is 2.5, not 2.5000000000000004, in the file
            }
            if (o.has_min && d < o.min) {
                d = o.min;
            }
            if (o.has_max && d > top) {
                d = top;
            }
            values.set(m, o, Json::number(d));
            changed = true;
        }
        break;
    }
    case ModOption::Type::Enum: {
        const std::string cur = v.as_string("");
        std::string label = cur;
        for (const ModOption::Value &e : o.values) {
            if (e.id == cur) {
                label = e.label;
            }
        }
        ImGui::SetNextItemWidth(w);
        if (ImGui::BeginCombo("##v", label.c_str())) {
            for (const ModOption::Value &e : o.values) {
                if (ImGui::Selectable(e.label.c_str(), e.id == cur)) {
                    values.set(m, o, Json::string(e.id));
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        break;
    }
    case ModOption::Type::Binding: {
        Binding b = mod_binding(values, m, o);
        if (binding_editor("mod:" + m.id + "." + o.id, &b, uses)) {
            values.set(m, o, binding_to_json(b));
            changed = true;
        }
        if (want_capture_) {
            capture_for_mod(m.id, o.id);
        }
        break;
    }
    }
    ImGui::TableNextColumn();
    if (values.is_set(m.id, o.id) && ImGui::SmallButton("Default")) {
        values.reset(m.id, o.id);
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

// ---- the controls

void Screens::capture_for(int section, const std::string &id) {
    std::string label = id;
    for (const PadButton &b : pad_buttons()) {
        if (id == b.id) {
            label = b.label;
        }
    }
    for (const HotkeyAction &a : hotkey_actions()) {
        if (id == a.id) {
            label = a.label;
        }
    }
    begin_capture(section == 0 ? InputCapture::Kind::Key
                  : section == 1 ? InputCapture::Kind::Pad
                                 : InputCapture::Kind::Binding,
                  section, id, label);
}

void Screens::begin_capture(InputCapture::Kind kind, int section, const std::string &id, const std::string &label) {
    capture_.begin(kind);
    capture_section_ = section;
    capture_id_ = id;
    capture_label_ = label;
    capture_popup_ = true;
}

void Screens::finish_capture() {
    Trigger t;
    if (!capture_.take_done(&t) || t.empty()) {
        return;
    }
    Settings &s = settings_.values;
    if (capture_section_ == 3) {
        const ModManifest *m = find_mod(capture_id_);
        const ModOption *o = m != nullptr ? m->option(capture_option_) : nullptr;
        if (o == nullptr) {
            return;
        }
        ModValues values(&settings_.doc);
        Binding b = mod_binding(values, *m, *o);
        if (std::find(b.begin(), b.end(), t) == b.end() && b.size() < BINDING_MAX_TRIGGERS) {
            b.push_back(t);
        }
        values.set(*m, *o, binding_to_json(b));
    } else if (capture_section_ == 2) {
        Binding b = s.hotkey_for(capture_id_);
        if (std::find(b.begin(), b.end(), t) == b.end()) {
            b.push_back(t);
        }
        s.hotkeys[capture_id_] = b;
    } else {
        std::vector<std::string> names = capture_section_ == 0 ? s.keys_for(capture_id_) : s.pad_for(capture_id_);
        if (std::find(names.begin(), names.end(), t[0]) == names.end()) {
            names.push_back(t[0]);
        }
        (capture_section_ == 0 ? s.keyboard : s.gamepad)[capture_id_] = names;
    }
    dirty_ = true;
}

// Everything bound to a binding (the hotkeys, the mods' binding options), for the conflict marks.
std::vector<Screens::BindingUse> Screens::binding_uses() const {
    std::vector<BindingUse> uses;
    const Settings &s = settings_.values;
    for (const HotkeyAction &a : hotkey_actions()) {
        uses.push_back({ std::string("hotkey:") + a.id, std::string("the hotkey ") + a.label, s.hotkey_for(a.id) });
    }
    ModValues values(const_cast<Json *>(&settings_.doc));
    for (const ModManifest &m : mods_) {
        if (!m.error.empty()) {
            continue;
        }
        for (const ModOption &o : m.options) {
            if (o.type == ModOption::Type::Binding) {
                uses.push_back({ "mod:" + m.id + "." + o.id, m.name + ": " + o.name, mod_binding(values, m, o) });
            }
        }
    }
    return uses;
}

// The other buttons and bindings using `input` (a key name, or a gamepad name with `pad`), except `except` (a button
// id, or a BindingUse key).
static std::string uses_of(const Settings &s, const std::vector<Screens::BindingUse> &uses, bool pad,
                           const std::string &input, const std::string &except) {
    std::string out;
    for (const PadButton &b : pad_buttons()) {
        std::vector<std::string> names = pad ? s.pad_for(b.id) : s.keys_for(b.id);
        if (b.id != except && std::find(names.begin(), names.end(), input) != names.end()) {
            out += (out.empty() ? "" : ", ") + std::string(b.label);
        }
    }
    const std::string full = pad ? "pad:" + input : input;
    for (const Screens::BindingUse &u : uses) {
        for (const Trigger &t : u.binding) {
            if (u.key != except && t.size() == 1 && t[0] == full) {
                out += (out.empty() ? "" : ", ") + u.label;
                break;
            }
        }
    }
    return out;
}

// One input as a chip; true when its remove button was pressed.
static bool chip(const std::string &text, const std::string &conflict, const char *id) {
    ImGui::PushID(id);
    if (!conflict.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Button, rgb(0x7A3B2E));
    }
    ImGui::SmallButton(text.c_str());
    if (!conflict.empty()) {
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Also bound to %s", conflict.c_str());
    }
    ImGui::SameLine(0, 2);
    bool removed = ImGui::SmallButton("x");
    ImGui::SetItemTooltip("Remove %s", text.c_str());
    ImGui::PopID();
    ImGui::SameLine();
    return removed;
}

void Screens::draw_controls() {
    draw_restart_note();
    ImGui::BeginDisabled(!settings_.writable());
    if (ImGui::BeginTabBar("controls")) {
        if (ImGui::BeginTabItem("Keyboard")) {
            draw_names_table(false);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Gamepad")) {
            draw_names_table(true);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Hotkeys")) {
            draw_hotkeys();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndDisabled();
    draw_capture_popup();
}

void Screens::draw_names_table(bool pad) {
    Settings &s = settings_.values;
    auto &map = pad ? s.gamepad : s.keyboard;
    ImGui::TextDisabled(pad ? "Every gamepad drives the PS1 pad (positions: south is the PlayStation cross)."
                            : "Keys by their place on the keyboard, whatever its layout.");
    ImGui::SameLine();
    ImGui::BeginDisabled(map.empty());
    if (ImGui::SmallButton(pad ? "Reset the gamepad" : "Reset the keyboard")) {
        map.clear();
        dirty_ = true;
    }
    ImGui::EndDisabled();
    if (!ImGui::BeginTable(pad ? "pad" : "keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        return;
    }
    const std::vector<BindingUse> uses = binding_uses();
    ImGui::TableSetupColumn("Button", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Inputs", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthFixed);
    for (const PadButton &b : pad_buttons()) {
        ImGui::PushID(b.id);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(b.label);
        ImGui::TableNextColumn();
        std::vector<std::string> names = pad ? s.pad_for(b.id) : s.keys_for(b.id);
        for (size_t i = 0; i < names.size(); i++) {
            const std::string label = pad ? pad_input_label(names[i]) : names[i];
            if (chip(label, uses_of(s, uses, pad, names[i], b.id), std::to_string(i).c_str())) {
                names.erase(names.begin() + (long)i);
                map[b.id] = names;
                dirty_ = true;
                break;
            }
        }
        if (names.empty()) {
            ImGui::TextDisabled("unbound");
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("+")) {
            capture_for(pad ? 1 : 0, b.id);
        }
        ImGui::SetItemTooltip(pad ? "Add a gamepad input" : "Add a key");
        ImGui::TableNextColumn();
        if (map.count(b.id) != 0) {
            if (ImGui::SmallButton("Default")) {
                map.erase(b.id);
                dirty_ = true;
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

bool Screens::binding_editor(const std::string &key, Binding *b, const std::vector<BindingUse> &uses) {
    const Settings &s = settings_.values;
    bool changed = false;
    want_capture_ = false;
    for (size_t i = 0; i < b->size(); i++) {
        std::string conflict;
        if ((*b)[i].size() == 1) {
            const std::string &in = (*b)[i][0];
            bool pad = in.compare(0, 4, "pad:") == 0;
            conflict = uses_of(s, uses, pad, pad ? in.substr(4) : in, key);
        }
        if (chip(binding_label({ (*b)[i] }), conflict, std::to_string(i).c_str())) {
            b->erase(b->begin() + (long)i);
            changed = true;
            break;
        }
    }
    if (b->empty()) {
        ImGui::TextDisabled("unbound");
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(b->size() >= BINDING_MAX_TRIGGERS);
    want_capture_ = ImGui::SmallButton("+");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Add a key, a gamepad input or a chord (up to %d inputs held together)",
                          (int)CHORD_MAX_INPUTS);
    return changed;
}

void Screens::draw_hotkeys() {
    Settings &s = settings_.values;
    ImGui::TextWrapped("The port's own actions. A hotkey never reaches the game's pad. A chord: hold its inputs "
                       "together, then let go. The mods' bindings are on the Mods screen.");
    if (!ImGui::BeginTable("hotkeys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        return;
    }
    const std::vector<BindingUse> uses = binding_uses();
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthFixed);
    for (const HotkeyAction &a : hotkey_actions()) {
        ImGui::PushID(a.id);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(a.label);
        ImGui::SetItemTooltip("%s", a.description);
        ImGui::TableNextColumn();
        Binding b = s.hotkey_for(a.id);
        if (binding_editor(std::string("hotkey:") + a.id, &b, uses)) {
            s.hotkeys[a.id] = b;
            dirty_ = true;
        }
        if (want_capture_) {
            capture_for(2, a.id);
        }
        ImGui::TableNextColumn();
        if (s.hotkeys.count(a.id) != 0) {
            if (ImGui::SmallButton("Default")) {
                s.hotkeys.erase(a.id);
                dirty_ = true;
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void Screens::draw_capture_popup() {
    if (capture_popup_) {
        ImGui::OpenPopup("Press an input");
        capture_popup_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Press an input", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    if (!capture_.active()) {
        ImGui::CloseCurrentPopup();
    }
    const char *what = capture_section_ == 0   ? "a key"
                       : capture_section_ == 3 ? "a key, a gamepad input, or several together (a chord)"
                       : capture_section_ == 1 ? "a gamepad button, trigger or stick direction"
                                               : "a key, a gamepad input, or several together (a chord)";
    ImGui::Text("%s: press %s.", capture_label_.c_str(), what);
    if (!capture_.held().empty()) {
        ImGui::TextColored(rgb(ACCENT), "%s", binding_label({ capture_.held() }).c_str());
    }
    ImGui::TextDisabled("Escape cancels.");
    if (ImGui::Button("Cancel")) {
        capture_.cancel();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---- what the window and the menu drive

void Screens::load(const SettingsDir &location, const std::string &mods_dir, const std::string &user_mods_dir,
                   bool game_found) {
    location_ = location;
    game_found_ = game_found;
    mods_dir_ = mods_dir;
    user_mods_dir_ = user_mods_dir;
    mods_.clear();
    if (!location_.dir.empty()) {
        settings_.load(location_.dir);
    }
    if (!mods_dir_.empty()) {
        mods_ = mods_scan(mods_dir_);
    }
    // The user's data mods (docs/LAUNCHER.md "Data mods"): a mod with a game mod's id is listed broken.
    if (!user_mods_dir_.empty()) {
        for (ModManifest &m : mods_scan(user_mods_dir_, true)) {
            if (m.error.empty() && find_mod(m.id) != nullptr) {
                m.error = "the game has a mod with this id";
            }
            mods_.push_back(m);
        }
    }
    mod_selected_ = mods_.empty() ? "" : mods_[0].id;
}

bool Screens::handle_event(const SDL_Event &e) {
    // The controls' prompt takes its keys and gamepad inputs before ImGui (no Enter or Escape reaches a widget).
    if (capture_.active() && capture_.feed(e)) {
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
            pad_nav_hold_ = true;
        }
        return true;
    }
    return false;
}

void Screens::begin_frame() {
    finish_capture();
    // ImGui reads the gamepads' state itself: after a gamepad press that went to the prompt, its navigation waits
    // until every button is released (else the same press would also activate the focused widget).
    ImGuiIO &io = ImGui::GetIO();
    if (pad_nav_hold_ && !capture_.active() && !any_gamepad_input_held()) {
        pad_nav_hold_ = false;
    }
    if (pad_nav_hold_ || capture_.active()) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    }
}

void Screens::end_frame() {
    // Saved once no widget is being dragged or typed into: one write per change, not one per frame.
    if (dirty_ && !ImGui::IsAnyItemActive()) {
        flush();
    }
}

void Screens::flush() {
    if (!dirty_ || !settings_.writable() || location_.dir.empty()) {
        return;
    }
    std::string err;
    if (settings_.save(&err)) {
        error_.clear();
        saved_ = true;
    } else {
        error_ = err;
    }
    dirty_ = false;
}

void Screens::draw_restart_note() {
    if (in_game_) {
        ImGui::PushStyleColor(ImGuiCol_Text, rgb(ACCENT));
        ImGui::TextWrapped("Changes are saved as you make them and apply at once, except those marked (restart), which "
                           "take effect when the game starts again.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
}

// "(restart)" after a label, in the game, for a setting the runtime cannot apply live.
void Screens::restart_mark() {
    if (in_game_) {
        ImGui::SameLine();
        ImGui::TextDisabled("(restart)");
        ImGui::SetItemTooltip("Takes effect when the game starts again");
    }
}

} // namespace psxstack
