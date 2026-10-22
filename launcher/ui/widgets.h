// Small ImGui pieces the launcher's window and the in-game menu share.
#pragma once

#include <string>

#include "imgui.h"

namespace psxstack {

// A two-column list of labels and values (values wrap): fields_begin opens the table (false: nothing to draw), a
// field per row, ImGui::EndTable closes it.
bool fields_begin(const char *id);
void field(const char *label, const std::string &value, const ImVec4 *color = nullptr);
// `prefix` then `text`, the text's left end cut with an ellipsis when the line does not fit.
void text_fit_left(const char *prefix, const std::string &text);
// `text` cut at its end with an ellipsis to fit `width` (all of it when it fits).
std::string text_fit_right(const std::string &text, float width);
// A section's title, in the accent colour.
void section_title(const char *title);
// A button's width for `label` (SmallButton's too: the same horizontal padding).
float button_width(const char *label);
// On a line of items, after ImGui::SameLine: a new line when the next item, `width` wide, no longer fits, starting
// at `indent` (from the window's left, as ImGui::SameLine's: a settings row's value column).
void wrap_for(float width, float indent = 0);
// A radio button's width for `label`.
float radio_width(const char *label);

} // namespace psxstack
