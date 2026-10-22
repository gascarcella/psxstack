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
// A section's title, in the accent colour.
void section_title(const char *title);

} // namespace psxstack
