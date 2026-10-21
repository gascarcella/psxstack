#include "style.h"

namespace psxstack {

ImVec4 rgb(unsigned hex, float a) {
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, a);
}

void gui_style(float scale) {
    ImGuiStyle &style = ImGui::GetStyle();
    style = ImGuiStyle(); // the defaults again: ScaleAllSizes is cumulative
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 0;
    style.ChildRounding = 6;
    style.FrameRounding = 5;
    style.GrabRounding = 5;
    style.PopupRounding = 6;
    style.WindowPadding = ImVec2(16, 14);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 8);
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 0;
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    ImVec4 *c = style.Colors;
    c[ImGuiCol_Text] = rgb(0xE8E6E3);
    c[ImGuiCol_TextDisabled] = rgb(0x8A8F98);
    c[ImGuiCol_WindowBg] = rgb(0x15171C);
    c[ImGuiCol_ChildBg] = rgb(0x1C1F26);
    c[ImGuiCol_PopupBg] = rgb(0x22262E);
    c[ImGuiCol_Border] = rgb(0x2E333D);
    c[ImGuiCol_FrameBg] = rgb(0x262A33);
    c[ImGuiCol_FrameBgHovered] = rgb(0x2F3440);
    c[ImGuiCol_FrameBgActive] = rgb(0x363C4A);
    c[ImGuiCol_Button] = rgb(0x2A2F39);
    c[ImGuiCol_ButtonHovered] = rgb(0x353B47);
    c[ImGuiCol_ButtonActive] = rgb(ACCENT, 0.85f);
    c[ImGuiCol_Header] = rgb(ACCENT, 0.22f);
    c[ImGuiCol_HeaderHovered] = rgb(ACCENT, 0.32f);
    c[ImGuiCol_HeaderActive] = rgb(ACCENT, 0.45f);
    c[ImGuiCol_CheckMark] = rgb(ACCENT);
    c[ImGuiCol_CheckboxSelectedBg] = rgb(ACCENT, 0.22f);
    c[ImGuiCol_SliderGrab] = rgb(ACCENT, 0.85f);
    c[ImGuiCol_SliderGrabActive] = rgb(ACCENT);
    c[ImGuiCol_Separator] = rgb(0x2E333D);
    c[ImGuiCol_NavCursor] = rgb(ACCENT);
    c[ImGuiCol_TextSelectedBg] = rgb(ACCENT, 0.35f);
    c[ImGuiCol_Tab] = rgb(0x262A33);
    c[ImGuiCol_TabHovered] = rgb(ACCENT, 0.45f);
    c[ImGuiCol_TabSelected] = rgb(ACCENT, 0.30f);
    c[ImGuiCol_TabSelectedOverline] = rgb(ACCENT);
    c[ImGuiCol_TabDimmed] = rgb(0x262A33);
    c[ImGuiCol_TabDimmedSelected] = rgb(ACCENT, 0.22f);
    c[ImGuiCol_TitleBg] = rgb(0x22262E);
    c[ImGuiCol_TitleBgActive] = rgb(0x2A2F39);
    c[ImGuiCol_ScrollbarGrab] = rgb(0x353B47);
    c[ImGuiCol_ScrollbarBg] = rgb(0x1C1F26);
    c[ImGuiCol_ModalWindowDimBg] = rgb(0x000000, 0.45f);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.FontSizeBase = 17.0f;
}

} // namespace psxstack
