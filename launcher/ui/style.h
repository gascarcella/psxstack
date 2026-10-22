// The launcher's look (docs/LAUNCHER.md "Launcher"): dark panels, one warm accent. Shared by the launcher's window
// (launcher/src/app.cpp) and the in-game menu (launcher/ui/menu.cpp), so both read the same.
#pragma once

#include "imgui.h"

namespace psxstack {

constexpr unsigned ACCENT = 0xE8873A;
constexpr unsigned ERROR_RED = 0xE5534B;

// The launcher's window at scale 1 (launcher/src/app.cpp): the canvas its screens are laid out for. The in-game menu
// fits it into the game's window (launcher/ui/menu.cpp), never leaving less than the smallest canvas the screens
// still fit, 640x480.
constexpr float CANVAS_W = 960, CANVAS_H = 620;
constexpr float CANVAS_MIN_W = 640, CANVAS_MIN_H = 480;

ImVec4 rgb(unsigned hex, float a = 1.0f);

// Sets the style on the current ImGui context, starting from the dark defaults, at `scale` (1 = 96 dpi; the
// launcher's is the display's content scale, the menu's fits the canvas above into the window). Calling it again with
// another scale replaces the previous one.
void gui_style(float scale);

} // namespace psxstack
