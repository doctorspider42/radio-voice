#include "gui/Theme.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <filesystem>

#include "core/Log.h"

namespace rv::gui {
namespace {

/// Segoe UI is present on every supported Windows version and is what the rest
/// of the system draws with, so the application looks native rather than like a
/// game overlay. The bundled ImGui font is only a fallback.
std::filesystem::path systemFont(const wchar_t* fileName)
{
#ifdef _WIN32
    wchar_t windows[MAX_PATH] = {};
    if (::GetWindowsDirectoryW(windows, MAX_PATH) == 0)
        return {};

    std::filesystem::path path = std::filesystem::path(windows) / L"Fonts" / fileName;
#else
    // System fonts are read in place; no Apple font is redistributed.
    std::filesystem::path path = "/System/Library/Fonts/Supplemental/Arial.ttf";
    const std::wstring name(fileName);
    if (name == L"seguisb.ttf" || name == L"segoeuib.ttf")
        path = "/System/Library/Fonts/Supplemental/Arial Bold.ttf";
    else if (name == L"consola.ttf")
        path = "/System/Library/Fonts/Menlo.ttc";
    else if (name == L"consolab.ttf")
        path = "/System/Library/Fonts/Menlo.ttc";
#endif
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
    const auto boldPath     = systemFont(L"segoeuib.ttf");
    // Consolas ships with every supported Windows version. Monospaced digits are
    // the point: a meter readout in a proportional face shuffles sideways every
    // time a 1 turns into an 8.
    const auto monoPath     = systemFont(L"consola.ttf");
    const auto monoBoldPath = systemFont(L"consolab.ttf");

    const auto orElse = [](const std::filesystem::path& wanted,
                           const std::filesystem::path& fallback) {
        return wanted.empty() ? fallback : wanted;
    };

    Fonts fonts;
    fonts.regular   = addFont(io, regularPath, 15.0f * dpiScale);
    fonts.medium    = addFont(io, orElse(semiboldPath, regularPath), 16.0f * dpiScale);
    fonts.small     = addFont(io, regularPath, 12.5f * dpiScale);
    fonts.heading   = addFont(io, orElse(boldPath, regularPath), 17.0f * dpiScale);
    fonts.caps      = addFont(io, orElse(boldPath, regularPath), 11.0f * dpiScale);
    fonts.title     = addFont(io, orElse(boldPath, regularPath), 14.0f * dpiScale);
    fonts.mono      = addFont(io, orElse(monoPath, regularPath), 13.0f * dpiScale);
    fonts.monoLarge = addFont(io, orElse(monoBoldPath, orElse(monoPath, regularPath)),
                              28.0f * dpiScale);

    if (!fonts.regular) {
        RV_WARN("System font is unavailable; falling back to the built-in font");
        fonts.regular = io.Fonts->AddFontDefault();
        fonts.medium = fonts.small = fonts.heading = fonts.regular;
        fonts.caps = fonts.title = fonts.mono = fonts.monoLarge = fonts.regular;
    } else {
        io.FontDefault = fonts.regular;
    }

    return fonts;
}

void applyTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowPadding     = ImVec2(14, 12);
    style.FramePadding      = ImVec2(10, 7);
    style.CellPadding       = ImVec2(8, 5);
    style.ItemSpacing       = ImVec2(10, 8);
    style.ItemInnerSpacing  = ImVec2(7, 5);
    style.IndentSpacing     = 20;
    style.ScrollbarSize     = 10;
    style.GrabMinSize       = 10;

    style.WindowBorderSize  = 1;
    style.ChildBorderSize   = 1;
    style.PopupBorderSize   = 1;
    style.FrameBorderSize   = 1;

    style.WindowRounding    = 12;
    style.ChildRounding     = 12;
    style.FrameRounding     = 8;
    style.PopupRounding     = 10;
    style.ScrollbarRounding = 6;
    style.GrabRounding      = 6;
    style.TabRounding       = 8;

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
    colours[ImGuiCol_FrameBgHovered]  = theme::toVec4(theme::kControl);
    colours[ImGuiCol_FrameBgActive]   = theme::toVec4(theme::kBorderBright);

    colours[ImGuiCol_TitleBg]         = theme::toVec4(theme::kPanel);
    colours[ImGuiCol_TitleBgActive]   = theme::toVec4(theme::kPanelRaised);
    colours[ImGuiCol_TitleBgCollapsed]= theme::toVec4(theme::kPanel);
    colours[ImGuiCol_MenuBarBg]       = theme::toVec4(theme::kPanel);

    colours[ImGuiCol_ScrollbarBg]     = ImVec4(0, 0, 0, 0);
    colours[ImGuiCol_ScrollbarGrab]   = theme::toVec4(theme::kBorderBright);
    colours[ImGuiCol_ScrollbarGrabHovered] = theme::toVec4(theme::kAccentDim);
    colours[ImGuiCol_ScrollbarGrabActive]  = theme::toVec4(theme::kAccent);

    colours[ImGuiCol_CheckMark]       = theme::toVec4(theme::kAccentLight);
    colours[ImGuiCol_SliderGrab]      = theme::toVec4(theme::kAccent);
    colours[ImGuiCol_SliderGrabActive]= theme::toVec4(theme::kAccentLight);

    colours[ImGuiCol_Button]          = theme::toVec4(theme::kPanelRaised);
    colours[ImGuiCol_ButtonHovered]   = theme::toVec4(theme::kControl);
    colours[ImGuiCol_ButtonActive]    = theme::toVec4(theme::kAccentFaint);

    colours[ImGuiCol_Header]          = theme::toVec4(theme::kAccentFaint);
    colours[ImGuiCol_HeaderHovered]   = theme::toVec4(theme::kControl);
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
