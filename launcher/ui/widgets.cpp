#include "widgets.h"

#include "style.h"

namespace psxstack {

// A two-column list of labels and values (values wrap).
bool fields_begin(const char *id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) {
        return false;
    }
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void field(const char *label, const std::string &value, const ImVec4 *color) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    ImGui::TableNextColumn();
    if (color != nullptr) {
        ImGui::PushStyleColor(ImGuiCol_Text, *color);
    }
    ImGui::TextWrapped("%s", value.c_str());
    if (color != nullptr) {
        ImGui::PopStyleColor();
    }
}

// `prefix` + `text`, the text shortened from its start ("...") to fit the remaining width.
void text_fit_left(const char *prefix, const std::string &text) {
    const float avail = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(prefix).x;
    size_t start = 0;
    while (start < text.size() && ImGui::CalcTextSize(("..." + text.substr(start)).c_str()).x > avail &&
           ImGui::CalcTextSize(text.c_str()).x > avail) {
        start++;
    }
    std::string shown = start == 0 ? text : "..." + text.substr(start);
    ImGui::TextDisabled("%s%s", prefix, shown.c_str());
}

void section_title(const char *title) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(ACCENT));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
}

} // namespace psxstack
