// The F10 panel's colours, the HUD's sage green; the console wears them too
#pragma once
#include <utility>

#include <imgui.h>

namespace app {

inline constexpr ImVec4 kPanelBg{0.04f, 0.045f, 0.04f, 0.94f};
inline constexpr ImVec4 kBorder{0.57f, 0.62f, 0.51f, 0.85f};
inline constexpr ImVec4 kText{0.89f, 0.91f, 0.86f, 1.0f};
inline constexpr ImVec4 kHighlight{0.24f, 0.29f, 0.21f, 1.0f};
inline constexpr ImVec4 kHeading{0.62f, 0.67f, 0.53f, 1.0f};
inline constexpr ImVec4 kDim{0.57f, 0.60f, 0.54f, 1.0f};
inline constexpr ImVec4 kTab{0.11f, 0.13f, 0.10f, 1.0f};
inline constexpr ImVec4 kTabHovered{0.18f, 0.22f, 0.16f, 1.0f};
inline constexpr ImVec4 kButton{0.15f, 0.18f, 0.13f, 1.0f};
inline constexpr ImVec4 kButtonHovered{0.32f, 0.38f, 0.27f, 1.0f};  // lighter than the highlight, so it shows on a selected row
inline constexpr ImVec4 kLine{0.62f, 0.67f, 0.53f, 0.35f};

inline constexpr std::pair<ImGuiCol, ImVec4> kPanelColors[] = {
    {ImGuiCol_WindowBg, kPanelBg}, {ImGuiCol_Border, kBorder}, {ImGuiCol_Text, kText},
    {ImGuiCol_Header, kHighlight}, {ImGuiCol_HeaderHovered, kHighlight}, {ImGuiCol_HeaderActive, kHighlight},
    {ImGuiCol_Button, kButton}, {ImGuiCol_ButtonHovered, kButtonHovered}, {ImGuiCol_ButtonActive, kButtonHovered},
    {ImGuiCol_FrameBg, kTab}, {ImGuiCol_FrameBgHovered, kTabHovered}, {ImGuiCol_FrameBgActive, kHighlight},
    {ImGuiCol_ScrollbarGrab, kHighlight}, {ImGuiCol_ScrollbarGrabHovered, kButtonHovered},
    {ImGuiCol_ScrollbarGrabActive, kHeading}, {ImGuiCol_Separator, kLine}, {ImGuiCol_NavHighlight, kHeading},
    {ImGuiCol_TextSelectedBg, kButtonHovered},
};

}  // namespace app
