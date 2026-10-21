// The launcher's look (docs/LAUNCHER.md "Launcher"): dark panels, one warm accent. Shared by the launcher's window
// (launcher/src/app.cpp) and the in-game menu (launcher/ui/menu.cpp), so both read the same.
#pragma once

#include "imgui.h"

namespace psxstack {

constexpr unsigned ACCENT = 0xE8873A;
constexpr unsigned ERROR_RED = 0xE5534B;

ImVec4 rgb(unsigned hex, float a = 1.0f);

// Sets the style on the current ImGui context, starting from the dark defaults, at `scale` (1 = 96 dpi; the
// launcher's is the display's content scale, the menu's follows the window's height). Calling it again with
// another scale replaces the previous one.
void gui_style(float scale);

} // namespace psxstack
