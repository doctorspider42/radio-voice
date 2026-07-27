#include "gui/Theme.h"

#include <windows.h>

#include <filesystem>

#include "core/Log.h"

namespace rv::gui {
namespace {

/// Segoe UI is present on every supported Windows version and is what the rest
/// of the system draws with, so the application looks native rather than like a
/// game overlay. The bundled ImGui font is only a fallback.
std::filesystem::path systemFont(const wchar_t* fileName)
{
    wchar_t windows[MAX_PATH] = {};
    if (::GetWindowsDirectoryW(windows, MAX_PATH) == 0)
        return {};

    std::filesystem::path path = std::filesystem::path(windows) / L"Fonts" / fileName;
    std::error_code ec;
    return std::filesystem::exists(path, ec) ? path : std::filesystem::path{};
}

ImFont* addFont(ImGuiIO& io, const std::filesystem::path& path, float sizePixels)
{
    if (path.empty())
        return nullptr;

    // ImGui takes a UTF-8 path.
    const std::string utf8 = path.string();

    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 1;
    config.PixelSnapH  = true;

    return io.Fonts->AddFontFromFileTTF(utf8.c_str(), sizePixels, &config,
                                        io.Fonts->GetGlyphRangesDefault());
}

} // namespace

Fonts loadFonts(float dpiScale)
{
    ImGuiIO& io = ImGui::GetIO();

    const auto regularPath  = systemFont(L"segoeui.ttf");
    const auto semiboldPath = systemFont(L"seguisb.ttf");

    Fonts fonts;
    fonts.regular = addFont(io, regularPath, 16.0f * dpiScale);
    fonts.medium  = addFont(io, semiboldPath.empty() ? regularPath : semiboldPath,
                            17.0f * dpiScale);
    fonts.small   = addFont(io, regularPath, 13.0f * dpiScale);
    fonts.heading = addFont(io, semiboldPath.empty() ? regularPath : semiboldPath,
                            20.0f * dpiScale);

    if (!fonts.regular) {
        RV_WARN("Segoe UI is unavailable; falling back to the built-in font");
        fonts.regular = io.Fonts->AddFontDefault();
        fonts.medium = fonts.small = fonts.heading = fonts.regular;
    } else {
        io.FontDefault = fonts.regular;
    }

    return fonts;
}

void applyTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowPadding     = ImVec2(14, 12);
    style.FramePadding      = ImVec2(10, 6);
    style.CellPadding       = ImVec2(8, 5);
    style.ItemSpacing       = ImVec2(10, 8);
    style.ItemInnerSpacing  = ImVec2(7, 5);
    style.IndentSpacing     = 20;
    style.ScrollbarSize     = 12;
    style.GrabMinSize       = 10;

    style.WindowBorderSize  = 0;
    style.ChildBorderSize   = 1;
    style.PopupBorderSize   = 1;
    style.FrameBorderSize   = 1;

    style.WindowRounding    = 8;
    style.ChildRounding     = 8;
    style.FrameRounding     = 5;
    style.PopupRounding     = 6;
    style.ScrollbarRounding = 6;
    style.GrabRounding      = 4;
    style.TabRounding       = 6;

    style.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
    style.SeparatorTextBorderSize = 1;
    style.SeparatorTextPadding    = ImVec2(16, 4);

    ImVec4* colours = style.Colors;

    colours[ImGuiCol_WindowBg]        = theme::toVec4(theme::kBackground);
    colours[ImGuiCol_ChildBg]         = theme::toVec4(theme::kPanel);
    colours[ImGuiCol_PopupBg]         = theme::toVec4(theme::kPanelRaised);
    colours[ImGuiCol_Border]          = theme::toVec4(theme::kBorder);
    colours[ImGuiCol_BorderShadow]    = ImVec4(0, 0, 0, 0);

    colours[ImGuiCol_Text]            = theme::toVec4(theme::kText);
    colours[ImGuiCol_TextDisabled]    = theme::toVec4(theme::kTextFaint);

    colours[ImGuiCol_FrameBg]         = theme::toVec4(theme::kPanelRaised);
    colours[ImGuiCol_FrameBgHovered]  = ImVec4(0.18f, 0.20f, 0.25f, 1.0f);
    colours[ImGuiCol_FrameBgActive]   = ImVec4(0.21f, 0.24f, 0.29f, 1.0f);

    colours[ImGuiCol_TitleBg]         = theme::toVec4(theme::kPanel);
    colours[ImGuiCol_TitleBgActive]   = theme::toVec4(theme::kPanel);
    colours[ImGuiCol_TitleBgCollapsed]= theme::toVec4(theme::kPanel);
    colours[ImGuiCol_MenuBarBg]       = theme::toVec4(theme::kPanel);

    colours[ImGuiCol_ScrollbarBg]     = ImVec4(0, 0, 0, 0);
    colours[ImGuiCol_ScrollbarGrab]   = theme::toVec4(theme::kBorderBright);
    colours[ImGuiCol_ScrollbarGrabHovered] = theme::toVec4(theme::kAccentDim);
    colours[ImGuiCol_ScrollbarGrabActive]  = theme::toVec4(theme::kAccent);

    colours[ImGuiCol_CheckMark]       = theme::toVec4(theme::kAccent);
    colours[ImGuiCol_SliderGrab]      = theme::toVec4(theme::kAccent);
    colours[ImGuiCol_SliderGrabActive]= theme::toVec4(theme::kAccent);

    colours[ImGuiCol_Button]          = theme::toVec4(theme::kPanelRaised);
    colours[ImGuiCol_ButtonHovered]   = ImVec4(0.16f, 0.32f, 0.31f, 1.0f);
    colours[ImGuiCol_ButtonActive]    = theme::toVec4(theme::kAccentDim);

    colours[ImGuiCol_Header]          = ImVec4(0.16f, 0.32f, 0.31f, 0.60f);
    colours[ImGuiCol_HeaderHovered]   = ImVec4(0.16f, 0.32f, 0.31f, 0.85f);
    colours[ImGuiCol_HeaderActive]    = theme::toVec4(theme::kAccentDim);

    colours[ImGuiCol_Separator]       = theme::toVec4(theme::kBorder);
    colours[ImGuiCol_SeparatorHovered]= theme::toVec4(theme::kAccentDim);
    colours[ImGuiCol_SeparatorActive] = theme::toVec4(theme::kAccent);

    colours[ImGuiCol_ResizeGrip]      = ImVec4(0, 0, 0, 0);
    colours[ImGuiCol_ResizeGripHovered] = theme::toVec4(theme::kAccentDim);
    colours[ImGuiCol_ResizeGripActive]  = theme::toVec4(theme::kAccent);

    colours[ImGuiCol_Tab]             = theme::toVec4(theme::kPanel);
    colours[ImGuiCol_TabHovered]      = theme::toVec4(theme::kAccentDim);
    colours[ImGuiCol_TabSelected]     = theme::toVec4(theme::kPanelRaised);

    colours[ImGuiCol_PlotLines]       = theme::toVec4(theme::kAccent);
    colours[ImGuiCol_PlotHistogram]   = theme::toVec4(theme::kAccent);

    colours[ImGuiCol_TableHeaderBg]   = theme::toVec4(theme::kPanelRaised);
    colours[ImGuiCol_TableBorderStrong] = theme::toVec4(theme::kBorderBright);
    colours[ImGuiCol_TableBorderLight]  = theme::toVec4(theme::kBorder);
    colours[ImGuiCol_TableRowBg]        = ImVec4(0, 0, 0, 0);
    colours[ImGuiCol_TableRowBgAlt]     = ImVec4(1, 1, 1, 0.022f);

    colours[ImGuiCol_DragDropTarget]  = theme::toVec4(theme::kAccent);
    colours[ImGuiCol_NavCursor]       = theme::toVec4(theme::kAccent);
    colours[ImGuiCol_ModalWindowDimBg]= ImVec4(0.02f, 0.02f, 0.03f, 0.72f);
}

} // namespace rv::gui
