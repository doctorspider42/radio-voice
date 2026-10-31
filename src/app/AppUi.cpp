// The window itself: header, rack, module views, footer and the device screen.
//
// Everything that is not drawing - the engine, the chain, the configuration -
// lives in App.cpp. This file only turns that state into pixels and clicks back
// into it.

#include "app/App.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/Log.h"
#include "core/Strings.h"
#include "dsp/Compressor.h"
#include "dsp/GraphicEq.h"
#include "dsp/NoiseSuppressor.h"
#include "dsp/Smoothing.h"
#include "gui/EqEditor.h"
#include "gui/Widgets.h"

namespace rv::app {
namespace {

using namespace rv::gui;

/// Width of gui::toggleSwitch, for right-aligning one on a row.
float switchWidth() { return ImGui::GetFrameHeight() * 0.72f * 1.85f; }

/// Pushes a font at the size it was loaded at. ImGui 1.92 keeps the current
/// size when given zero, which would draw every font at the body size.
void pushFont(ImFont* font)
{
    ImGui::PushFont(font, font->LegacySize);
}

std::string upper(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

/// `text` cut down with an ellipsis until it fits `maxWidth` in the current font.
std::string fitted(const std::string& text, float maxWidth)
{
    if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth)
        return text;

    std::string shortened = text;
    while (!shortened.empty() && ImGui::CalcTextSize((shortened + "...").c_str()).x > maxWidth)
        shortened.pop_back();
    return shortened + "...";
}

ImU32 loadColour(float load)
{
    if (load > 0.85f)
        return theme::kDanger;
    if (load > 0.6f)
        return theme::kWarning;
    return theme::kAccentLight;
}

/// Rounds away the negative zero that "%.1f" prints for a reduction of -0.01.
double tidy(float value)
{
    return std::abs(value) < 0.05f ? 0.0 : static_cast<double>(value);
}

float toDb(float linear)
{
    return linear > 1.0e-6f ? dsp::gainToDb(linear) : -120.0f;
}

void dashedLine(ImDrawList* draw, ImVec2 a, ImVec2 b, ImU32 colour, float dash, float gap,
                float thickness = 1.0f)
{
    const float length = std::hypot(b.x - a.x, b.y - a.y);
    if (length <= 0.0f)
        return;
    const ImVec2 dir((b.x - a.x) / length, (b.y - a.y) / length);
    for (float d = 0.0f; d < length; d += dash + gap) {
        const float e = std::min(length, d + dash);
        draw->AddLine(ImVec2(a.x + dir.x * d, a.y + dir.y * d),
                      ImVec2(a.x + dir.x * e, a.y + dir.y * e), colour, thickness);
    }
}

/// The compressor's static curve, as Compressor.cpp computes it: unity below
/// the knee, the ratio above it, and a quadratic blend across it.
float compressorCurve(float inputDb, float thresholdDb, float ratio, float kneeDb)
{
    const float over = inputDb - thresholdDb;
    if (kneeDb > 0.0f && 2.0f * over > -kneeDb && 2.0f * over < kneeDb) {
        const float x = over + kneeDb * 0.5f;
        return inputDb + (1.0f / ratio - 1.0f) * x * x / (2.0f * kneeDb);
    }
    if (2.0f * over <= -kneeDb)
        return inputDb;
    return thresholdDb + over / ratio;
}

/// Width chipButton will take for `label`.
float chipWidth(ImFont* caps, const char* label)
{
    pushFont(caps);
    const float width = spacedTextWidth(label, caps->LegacySize * 0.12f) +
                        ImGui::GetFrameHeight() * 0.86f;
    ImGui::PopFont();
    return width;
}

/// Lays header controls out from the right edge leftwards, centred on one line.
struct RightAligner {
    float x;
    float centreY;
    float gap;

    void next(float width, float height)
    {
        x -= width;
        ImGui::SetCursorScreenPos(ImVec2(x, centreY - height * 0.5f));
        x -= gap;
    }
};

/// The body of a module view, between its header strip and its footer strip.
struct ModuleFrame {
    ImVec2       bodyMin;
    ImVec2       bodyMax;
    RightAligner controls;
    ImVec2       footerText;

    float width() const { return bodyMax.x - bodyMin.x; }
    float height() const { return bodyMax.y - bodyMin.y; }
};

/// Draws a module's faceplate: the lit header strip with its name, the body,
/// and the footer strip. Controls in the header are placed by the caller with
/// `controls`, right to left.
ModuleFrame drawModuleFrame(const Fonts& fonts, ImVec2 origin, ImVec2 size, const char* name,
                            const char* subtitle, bool ownModule, float scale)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(origin.x + size.x, origin.y + size.y);
    const float  rounding = 18.0f * scale;
    const float  headerH  = 58.0f * scale;
    const float  footerH  = 32.0f * scale;
    const float  pad      = 18.0f * scale;

    // A wide, faint glow under the whole plate: the open module is the thing
    // the window is about.
    for (int i = 3; i >= 1; --i) {
        const float g = static_cast<float>(i) * 6.0f * scale;
        draw->AddRect(ImVec2(origin.x - g * 0.5f, origin.y - g * 0.5f),
                      ImVec2(max.x + g * 0.5f, max.y + g * 0.5f),
                      theme::withAlpha(theme::kAccent, 0.035f * static_cast<float>(4 - i)),
                      rounding + g * 0.5f, g);
    }

    drawFaceplate(draw, origin, max, rounding);

    // Header strip.
    const ImVec2 headerMax(max.x, origin.y + headerH);
    draw->AddRectFilledMultiColor(ImVec2(origin.x + 1, origin.y + rounding),
                                  ImVec2(max.x - 1, headerMax.y), theme::kStripTop,
                                  theme::kStripTop, theme::kStripBottom, theme::kStripBottom);
    draw->AddRectFilled(ImVec2(origin.x + 1, origin.y + 1), ImVec2(max.x - 1, origin.y + rounding + 1),
                        theme::kStripTop, rounding - 1, ImDrawFlags_RoundTop);
    draw->AddLine(ImVec2(origin.x + 1, headerMax.y), ImVec2(max.x - 1, headerMax.y),
                  IM_COL32(0x07, 0x09, 0x0C, 0xFF), 1.0f);
    draw->AddLine(ImVec2(origin.x + 1, headerMax.y + 1), ImVec2(max.x - 1, headerMax.y + 1),
                  IM_COL32(0xFF, 0xFF, 0xFF, 0x0A), 1.0f);

    const float centreY = origin.y + headerH * 0.5f;
    float x = origin.x + pad;

    // Mark: the logo for our own modules, a neutral box for a plugin.
    const float mark = 28.0f * scale;
    if (ownModule) {
        drawLogo(draw, ImVec2(x, centreY - mark * 0.5f), mark, false);
    } else {
        const ImVec2 a(x, centreY - mark * 0.5f), b(x + mark, centreY + mark * 0.5f);
        draw->AddRectFilled(a, b, theme::kChrome, 7.0f * scale);
        draw->AddRect(a, b, theme::kBorderBright, 7.0f * scale);
        const ImVec2 c(x + mark * 0.5f, centreY);
        const float  r = mark * 0.26f;
        draw->AddQuad(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y - r * 0.4f), ImVec2(c.x, c.y + r * 0.2f),
                      ImVec2(c.x - r, c.y - r * 0.4f), theme::kTextDim, 1.4f);
        draw->AddLine(ImVec2(c.x - r, c.y - r * 0.4f), ImVec2(c.x - r, c.y + r * 0.6f), theme::kTextDim, 1.4f);
        draw->AddLine(ImVec2(c.x + r, c.y - r * 0.4f), ImVec2(c.x + r, c.y + r * 0.6f), theme::kTextDim, 1.4f);
        draw->AddLine(ImVec2(c.x - r, c.y + r * 0.6f), ImVec2(c.x, c.y + r * 1.2f), theme::kTextDim, 1.4f);
        draw->AddLine(ImVec2(c.x + r, c.y + r * 0.6f), ImVec2(c.x, c.y + r * 1.2f), theme::kTextDim, 1.4f);
        draw->AddLine(ImVec2(c.x, c.y + r * 0.2f), ImVec2(c.x, c.y + r * 1.2f), theme::kTextDim, 1.4f);
    }
    x += mark + 12.0f * scale;

    pushFont(fonts.title);
    const float titleSpacing = fonts.title->LegacySize * 0.24f;
    const float titleY = centreY - fonts.title->LegacySize * 0.5f - fonts.caps->LegacySize * 0.55f - 1.0f;
    float tx = x;
    if (ownModule) {
        drawSpacedText(draw, ImVec2(tx, titleY), theme::kAccent, "RV", titleSpacing);
        tx += spacedTextWidth("RV", titleSpacing) + titleSpacing * 2.0f;
    }
    drawSpacedText(draw, ImVec2(tx, titleY), theme::kText, name, titleSpacing);
    ImGui::PopFont();

    pushFont(fonts.caps);
    drawSpacedText(draw, ImVec2(x, titleY + fonts.title->LegacySize + 4.0f * scale), theme::kTextFaint,
                   subtitle, fonts.caps->LegacySize * 0.18f);
    ImGui::PopFont();

    // Footer strip.
    const ImVec2 footerMin(origin.x + 1, max.y - footerH);
    draw->AddRectFilled(footerMin, ImVec2(max.x - 1, max.y - 1), IM_COL32(0x0B, 0x0D, 0x11, 0xFF),
                        rounding - 1, ImDrawFlags_RoundBottom);
    draw->AddLine(footerMin, ImVec2(max.x - 1, footerMin.y), IM_COL32(0x07, 0x09, 0x0C, 0xFF), 1.0f);

    ModuleFrame frame;
    frame.bodyMin    = ImVec2(origin.x + pad, origin.y + headerH + pad);
    frame.bodyMax    = ImVec2(max.x - pad, max.y - footerH - pad);
    frame.controls   = RightAligner{max.x - pad, centreY, 10.0f * scale};
    frame.footerText = ImVec2(origin.x + pad, max.y - footerH * 0.5f);
    return frame;
}

/// One line of mono text vertically centred on `centre`, in the footer strip.
void footerText(const Fonts& fonts, ImVec2 centre, const char* text)
{
    pushFont(fonts.mono);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(centre.x, centre.y - fonts.mono->LegacySize * 0.5f), theme::kTextFaint, text);
    ImGui::PopFont();
}

/// Caption in the top-left corner of a display well.
void displayCaption(const Fonts& fonts, ImVec2 at, const char* text)
{
    pushFont(fonts.caps);
    drawSpacedText(ImGui::GetWindowDrawList(), at, theme::kTextFaint, text,
                   fonts.caps->LegacySize * 0.18f);
    ImGui::PopFont();
}

/// Height a gui::knob of diameter `d` takes, label and readout included.
float knobHeight(const Fonts& fonts, float d)
{
    return fonts.caps->LegacySize + d * 0.08f * 2.0f + d + fonts.mono->LegacySize + 8.0f;
}

/// A large two-state button for the footer, Mute and Bypass.
bool bigButton(const Fonts& fonts, const char* label, bool lit, ImU32 litColour, ImVec2 size)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool pressed  = ImGui::InvisibleButton(label, size);
    const bool hovered  = ImGui::IsItemHovered();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(origin.x + size.x, origin.y + size.y);
    const float  rounding = size.y * 0.26f;

    if (lit) {
        draw->AddRectFilled(ImVec2(origin.x - 3, origin.y - 3), ImVec2(max.x + 3, max.y + 3),
                            theme::withAlpha(litColour, 0.14f), rounding + 3);
        draw->AddRectFilled(origin, max, theme::withAlpha(litColour, 0.16f), rounding);
        draw->AddRect(origin, max, litColour, rounding, 1.2f);
    } else {
        draw->AddRectFilledMultiColor(ImVec2(origin.x, origin.y + rounding), ImVec2(max.x, max.y - rounding),
                                      theme::kStripTop, theme::kStripTop, theme::kPanelRaised,
                                      theme::kPanelRaised);
        draw->AddRectFilled(origin, ImVec2(max.x, origin.y + rounding * 2), theme::kStripTop, rounding,
                            ImDrawFlags_RoundTop);
        draw->AddRectFilled(ImVec2(origin.x, max.y - rounding * 2), max, theme::kPanelRaised, rounding,
                            ImDrawFlags_RoundBottom);
        draw->AddRect(origin, max, hovered ? theme::kTextGhost : theme::kBorderBright, rounding);
        draw->AddLine(ImVec2(origin.x + rounding, origin.y + 1), ImVec2(max.x - rounding, origin.y + 1),
                      theme::kHighlight);
    }

    pushFont(fonts.caps);
    const float spacing = fonts.caps->LegacySize * 0.2f;
    const float width   = spacedTextWidth(label, spacing);
    drawSpacedText(draw,
                   ImVec2(origin.x + (size.x - width) * 0.5f,
                          origin.y + (size.y - fonts.caps->LegacySize) * 0.5f),
                   lit ? litColour : theme::kText, label, spacing);
    ImGui::PopFont();

    return pressed;
}

/// Filled area under a history trace, drawn as one quad per step because the
/// outline is not convex.
template <typename Value, typename Base>
void fillHistory(ImDrawList* draw, int count, Value value, Base base, float left, float width,
                 ImU32 colour)
{
    for (int i = 0; i + 1 < count; ++i) {
        const float x0 = left + width * static_cast<float>(i) / static_cast<float>(count - 1);
        const float x1 = left + width * static_cast<float>(i + 1) / static_cast<float>(count - 1);
        draw->AddQuadFilled(ImVec2(x0, value(i)), ImVec2(x1, value(i + 1)), ImVec2(x1, base(i + 1)),
                            ImVec2(x0, base(i)), colour);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void App::sampleHistory()
{
    // Thirty samples a second: six seconds across the displays, independent of
    // how fast the window happens to be drawing.
    const double now = ImGui::GetTime();
    if (now - lastHistorySample_ < 1.0 / 30.0)
        return;
    lastHistorySample_ = now;

    inputHistory_.push(toDb(meters_.inputPeak.load()));
    outputHistory_.push(toDb(meters_.outputPeak.load()));
    gateHistory_.push(meters_.gateReductionDb.load());
    gateOpenHistory_.push(meters_.gateOpen.load() ? 1.0f : 0.0f);
    compHistory_.push(meters_.compressorReductionDb.load());
    limiterHistory_.push(meters_.limiterReductionDb.load());
    speechHistory_.push(meters_.speechProbability.load());
}

void App::syncEmbeddedEditor()
{
    if (embeddedPlugin_ && embeddedPlugin_ != embeddedWanted_)
        embeddedPlugin_->hideEmbeddedEditor();
    embeddedPlugin_ = embeddedWanted_;
}

void App::render()
{
    devices_.refreshIfStale(2000);
    if (engine_)
        engine_->updateSlowMeters();
    engine_->chain().collectGarbage();

    // Both act on what the widgets left in config_ last frame, so they run
    // before anything is drawn again.
    syncMonitor();
    recoverInvalidatedDevice();
    applyPendingDeviceChange();

    sampleHistory();
    embeddedWanted_ = nullptr;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(4);

    renderHeader();
    renderNotices();

    const float footerHeight = px(76.0f);
    const float bodyHeight   = std::max(px(200.0f), ImGui::GetContentRegionAvail().y - footerHeight);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(20.0f), px(18.0f)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##body", ImVec2(0.0f, bodyHeight), ImGuiChildFlags_AlwaysUseWindowPadding,
                      showDevices_ ? ImGuiWindowFlags_None
                                   : ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    if (showDevices_) {
        renderDevicesScreen();
    } else {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float  rackWidth = std::clamp(available.x * 0.27f, px(300.0f), px(400.0f));
        const float  gap       = px(20.0f);

        renderRack(ImVec2(rackWidth, available.y));
        ImGui::SameLine(0.0f, gap);
        renderModuleView(ImVec2(available.x - rackWidth - gap, available.y));
    }

    ImGui::EndChild();

    renderFooter();

    ImGui::End();

    if (showPluginBrowser_)
        renderPluginBrowser();
    if (showLog_)
        renderLogWindow();

    syncEmbeddedEditor();
    saveIfDirty();
}

// ---------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------

void App::renderHeader()
{
    const auto  status  = engine_->status();
    const bool  running = engine_->isRunning();
    const float height  = px(66.0f);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  width  = ImGui::GetContentRegionAvail().x;
    const ImVec2 max(origin.x + width, origin.y + height);
    const float  centreY = origin.y + height * 0.5f;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilledMultiColor(origin, max, IM_COL32(0x16, 0x1A, 0x21, 0xFF),
                                  IM_COL32(0x16, 0x1A, 0x21, 0xFF), IM_COL32(0x0F, 0x12, 0x16, 0xFF),
                                  IM_COL32(0x0F, 0x12, 0x16, 0xFF));
    draw->AddLine(ImVec2(origin.x, max.y - 1), ImVec2(max.x, max.y - 1), theme::kInset);

    float x = origin.x + px(20.0f);
    auto place = [&](float w, float h) {
        ImGui::SetCursorScreenPos(ImVec2(x, centreY - h * 0.5f));
        x += w;
    };

    // --- identity ---------------------------------------------------------
    const float logo = px(36.0f);
    drawLogo(draw, ImVec2(x, centreY - logo * 0.5f), logo);
    x += logo + px(12.0f);

    pushFont(fonts_.heading);
    const float wordSpacing = fonts_.heading->LegacySize * 0.2f;
    const float radioWidth  = spacedTextWidth("RADIO", wordSpacing);
    const float wordY       = centreY - fonts_.heading->LegacySize * 0.5f;
    drawSpacedText(draw, ImVec2(x, wordY), theme::kText, "RADIO", wordSpacing);
    drawSpacedText(draw, ImVec2(x + radioWidth + wordSpacing, wordY), theme::kAccent, "VOICE",
                   wordSpacing);
    x += radioWidth + wordSpacing + spacedTextWidth("VOICE", wordSpacing) + px(10.0f);
    ImGui::PopFont();

    pushFont(fonts_.mono);
    char version[32];
    std::snprintf(version, sizeof(version), "v%s", RV_VERSION);
    draw->AddText(ImVec2(x, centreY - fonts_.mono->LegacySize * 0.5f + 1.0f), theme::kTextFaint, version);
    x += ImGui::CalcTextSize(version).x + px(18.0f);
    ImGui::PopFont();

    // --- state ------------------------------------------------------------
    {
        const float pillHeight = ImGui::GetFrameHeight() * 0.86f;
        ImGui::SetCursorScreenPos(ImVec2(x, centreY - pillHeight * 0.5f));
        if (running)
            statusPill("LIVE", theme::kAccent);
        else if (!startupError_.empty())
            statusPill("ERROR", theme::kDanger);
        else
            statusPill("STOPPED", theme::kTextFaint);
        x = ImGui::GetItemRectMax().x + px(16.0f);
    }

    // --- right-hand controls, laid out first so the middle knows its room --
    const float frame    = ImGui::GetFrameHeight();
    const float powerSz  = px(40.0f);
    const float gearSz   = frame * 1.15f;
    const float chipH    = frame * 0.86f;
    const float gap      = px(10.0f);
    const float restartW = chipWidth(fonts_.caps, "RESTART");
    const float monitorW = chipWidth(fonts_.caps, "MONITOR");
    const float rightW   = monitorW + powerSz + restartW + gearSz + gap * 3 + px(20.0f);
    const float rightX   = max.x - rightW;

    // --- the route, which opens the device screen ---------------------------
    {
        pushFont(fonts_.regular);
        const float nameRoom = px(150.0f);
        auto rate = [](int hz) {
            char text[16];
            std::snprintf(text, sizeof(text), "%dk", (hz + 500) / 1000);
            return std::string(text);
        };
        const std::string inName  = config_.input.deviceName.empty()
                                        ? std::string("no microphone")
                                        : fitted(config_.input.deviceName, nameRoom);
        const std::string outName = config_.output.deviceName.empty()
                                        ? std::string("no output")
                                        : fitted(config_.output.deviceName, nameRoom);
        const int inRate  = status.running ? static_cast<int>(status.inputSampleRate)
                                           : config_.input.sampleRate;
        const int outRate = status.running ? static_cast<int>(status.outputSampleRate)
                                           : config_.output.sampleRate;
        const std::string inRateText  = rate(inRate);
        const std::string outRateText = rate(outRate);

        const float textW  = ImGui::CalcTextSize(inName.c_str()).x + ImGui::CalcTextSize(outName.c_str()).x;
        ImGui::PopFont();
        pushFont(fonts_.mono);
        const float rateW = ImGui::CalcTextSize(inRateText.c_str()).x + ImGui::CalcTextSize(outRateText.c_str()).x;
        ImGui::PopFont();

        const float chipHeight = frame * 1.25f;
        const float w = px(12.0f) + textW + rateW + px(6.0f) * 2 + px(28.0f) + px(30.0f);

        if (x + w < rightX - gap) {
            place(w, chipHeight);
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton("##route", ImVec2(w, chipHeight));
            const bool hovered = ImGui::IsItemHovered();
            if (clicked)
                showDevices_ = true;
            if (hovered)
                ImGui::SetTooltip("Audio devices - where the voice comes from and where it goes");

            const ImVec2 b(a.x + w, a.y + chipHeight);
            drawInset(draw, a, b, 10.0f);
            if (hovered)
                draw->AddRect(a, b, theme::withAlpha(theme::kAccent, 0.45f), 10.0f);

            float cx = a.x + px(12.0f);
            const float ty = a.y + (chipHeight - ImGui::GetTextLineHeight()) * 0.5f;
            const float my = a.y + (chipHeight - fonts_.mono->LegacySize) * 0.5f;

            draw->AddText(ImVec2(cx, ty), theme::kText, inName.c_str());
            cx += ImGui::CalcTextSize(inName.c_str()).x + px(6.0f);
            pushFont(fonts_.mono);
            draw->AddText(ImVec2(cx, my), theme::kTextFaint, inRateText.c_str());
            cx += ImGui::CalcTextSize(inRateText.c_str()).x + px(8.0f);
            ImGui::PopFont();

            // An arrow drawn rather than typed: the font carries Latin only.
            const float ay = a.y + chipHeight * 0.5f;
            draw->AddLine(ImVec2(cx, ay), ImVec2(cx + px(12.0f), ay), theme::kAccent, 1.6f);
            draw->AddTriangleFilled(ImVec2(cx + px(14.0f), ay), ImVec2(cx + px(9.0f), ay - px(4.0f)),
                                    ImVec2(cx + px(9.0f), ay + px(4.0f)), theme::kAccent);
            cx += px(20.0f);

            draw->AddText(ImVec2(cx, ty), theme::kText, outName.c_str());
            cx += ImGui::CalcTextSize(outName.c_str()).x + px(6.0f);
            pushFont(fonts_.mono);
            draw->AddText(ImVec2(cx, my), theme::kTextFaint, outRateText.c_str());
            ImGui::PopFont();

            const float chevX = b.x - px(14.0f);
            draw->AddLine(ImVec2(chevX - px(3.0f), ay - px(4.0f)), ImVec2(chevX + px(1.0f), ay),
                          theme::kTextFaint, 1.5f);
            draw->AddLine(ImVec2(chevX + px(1.0f), ay), ImVec2(chevX - px(3.0f), ay + px(4.0f)),
                          theme::kTextFaint, 1.5f);
            x += px(14.0f);
        }
    }

    // --- live figures -------------------------------------------------------
    if (running) {
        struct Stat {
            const char* label;
            char        value[32];
            ImU32       colour;
        };
        Stat stats[4];
        const float load  = meters_.cpuLoad.load();
        const u32   xruns = meters_.inputXruns.load() + meters_.outputXruns.load();

        stats[0].label = "LAT";
        std::snprintf(stats[0].value, sizeof(stats[0].value), "%.1f ms",
                      static_cast<double>(meters_.latencyMs.load()));
        stats[0].colour = theme::kAccentLight;
        stats[1].label = "DSP";
        std::snprintf(stats[1].value, sizeof(stats[1].value), "%.0f%%",
                      static_cast<double>(load * 100.0f));
        stats[1].colour = loadColour(load);
        stats[2].label = "DROP";
        std::snprintf(stats[2].value, sizeof(stats[2].value), "%u", xruns);
        stats[2].colour = xruns > 0 ? theme::kWarning : theme::kAccentLight;
        stats[3].label = "DRIFT";
        std::snprintf(stats[3].value, sizeof(stats[3].value), "%+.0f",
                      static_cast<double>(meters_.driftPpm.load()));
        stats[3].colour = theme::kAccentLight;

        pushFont(fonts_.mono);
        const float pad = px(10.0f);
        float widths[4];
        float total = 0.0f;
        for (int i = 0; i < 4; ++i) {
            widths[i] = ImGui::CalcTextSize(stats[i].label).x + px(5.0f) +
                        ImGui::CalcTextSize(stats[i].value).x + pad * 2.0f;
            total += widths[i];
        }
        const float chipHeight = frame * 1.25f;

        if (x + total < rightX - gap) {
            const ImVec2 a(x, centreY - chipHeight * 0.5f);
            drawInset(draw, a, ImVec2(a.x + total, a.y + chipHeight), 10.0f);

            float sx = a.x;
            for (int i = 0; i < 4; ++i) {
                ImGui::SetCursorScreenPos(ImVec2(sx, a.y));
                ImGui::PushID(i);
                ImGui::InvisibleButton("##stat", ImVec2(widths[i], chipHeight));
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();

                const float ty = a.y + (chipHeight - fonts_.mono->LegacySize) * 0.5f;
                draw->AddText(ImVec2(sx + pad, ty), theme::kTextFaint, stats[i].label);
                draw->AddText(ImVec2(sx + pad + ImGui::CalcTextSize(stats[i].label).x + px(5.0f), ty),
                              stats[i].colour, stats[i].value);
                if (i > 0)
                    draw->AddLine(ImVec2(sx, a.y + px(8.0f)), ImVec2(sx, a.y + chipHeight - px(8.0f)),
                                  IM_COL32(0x14, 0x19, 0x20, 0xFF));
                sx += widths[i];

                if (!hovered)
                    continue;

                // A single number never explains itself, and here one term
                // almost always dominates the rest - usually the clock bridge,
                // which no amount of adding or removing plugins will move.
                if (i == 0) {
                    const auto parts = engine_->latencyBreakdown();
                    ImGui::SetTooltip(
                        "input device    %5.1f ms\n"
                        "clock bridge    %5.1f ms\n"
                        "chain           %5.1f ms\n"
                        "limiter         %5.1f ms\n"
                        "output device   %5.1f ms\n"
                        "                ------\n"
                        "total           %5.1f ms\n"
                        "\n"
                        "The clock bridge is the buffer that lets the capture and\n"
                        "playback devices run on their own clocks. It is sized from\n"
                        "the device period, so exclusive mode - or a smaller period in\n"
                        "Windows sound settings - is what shrinks it.\n"
                        "\n"
                        "Shared-mode drivers often report zero device latency, so the\n"
                        "true figure is somewhat higher than this.",
                        static_cast<double>(parts.inputDevice),
                        static_cast<double>(parts.bridge),
                        static_cast<double>(parts.chain),
                        static_cast<double>(parts.limiter),
                        static_cast<double>(parts.outputDevice),
                        static_cast<double>(parts.total()));
                } else if (i == 1) {
                    ImGui::SetTooltip("Share of each audio period spent processing.");
                } else if (i == 2) {
                    ImGui::SetTooltip("Dropouts - periods the devices had to wait for.");
                } else {
                    ImGui::SetTooltip(
                        "Correction applied to keep the capture and playback clocks aligned,\n"
                        "in ppm. A steady few dozen is normal for two separate devices.");
                }
            }
        }
        ImGui::PopFont();
    }

    // --- right-hand controls ------------------------------------------------
    RightAligner right{max.x - px(20.0f), centreY, gap};

    // Everything set once and then forgotten - the log, the version, and where
    // the window goes when it is dismissed - lives behind the cog.
    {
        const int  problems = log::problemCount();
        const auto update   = updater_.status();
        const bool open     = ImGui::IsPopupOpen("##options");

        // Two things can want attention through one cog, so they are ranked
        // rather than blended: something went wrong beats something is
        // available, and the tooltip says which it is either way.
        const bool waitingUpdate = update.state == Updater::State::Available ||
                                   update.state == Updater::State::Downloading ||
                                   update.state == Updater::State::Ready;

        const ImU32 gearTint = problems > 0    ? theme::kWarning
                               : waitingUpdate ? theme::kAccent
                                               : theme::kTextDim;

        right.next(gearSz, gearSz);
        if (gearButton("##options-button", gearSz, gearTint, open))
            ImGui::OpenPopup("##options");

        if (ImGui::IsItemHovered()) {
            char note[192];
            int  written = std::snprintf(note, sizeof(note),
                                         "Options - the log, the version, and where "
                                         "the window goes.");
            if (problems > 0) {
                written += std::snprintf(note + written, sizeof(note) - written,
                                         "\n%d problem%s logged.", problems,
                                         problems == 1 ? "" : "s");
            }
            if (update.state == Updater::State::Ready) {
                std::snprintf(note + written, sizeof(note) - written,
                              "\nVersion %s is ready to install.", update.version.c_str());
            } else if (update.state == Updater::State::Available) {
                std::snprintf(note + written, sizeof(note) - written,
                              "\nVersion %s has been released.", update.version.c_str());
            }
            ImGui::SetTooltip("%s", note);
        }

        // Bounded so that on a small screen at 200% scale the popup scrolls
        // rather than running off the bottom.
        const float maxHeight =
            std::max(px(240.0f), ImGui::GetMainViewport()->WorkSize.y - px(90.0f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(px(330.0f), 0.0f),
                                            ImVec2(px(420.0f), maxHeight));
        if (ImGui::BeginPopup("##options")) {
            renderOptionsMenu();
            ImGui::EndPopup();
        }
    }

    // Always present, not only when a setting changed. Restarting the stream is
    // the first thing to try for anything that sounds wrong.
    right.next(restartW, chipH);
    if (chipButton("RESTART", false))
        restartEngine();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Stops and reopens both audio devices.");

    right.next(powerSz, powerSz);
    if (powerButton("##engine", running, powerSz)) {
        if (running)
            stopEngine();
        else
            startEngine();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(running ? "Stop the audio engine" : "Start the audio engine");

    // Monitoring is the one routing decision an operator changes mid-take, so
    // it belongs next to the power button rather than on the device screen.
    {
        const bool lit    = config_.monitorEnabled;
        const bool failed = lit && running && !status.monitorError.empty();

        right.next(monitorW, chipH);
        if (chipButton("MONITOR", lit, failed ? theme::kDanger : theme::kAccent)) {
            config_.monitorEnabled = !config_.monitorEnabled;
            if (config_.monitorEnabled)
                ensureMonitorDevice();
            markDirty();
        }

        if (ImGui::IsItemHovered()) {
            if (failed) {
                ImGui::SetTooltip("Monitoring is on but the device could not be "
                                  "opened:\n%s", status.monitorError.c_str());
            } else if (config_.monitor.deviceName.empty()) {
                ImGui::SetTooltip("Hear the processed signal on a second device.\n"
                                  "Pick one on the device screen.");
            } else {
                ImGui::SetTooltip("Hear the processed signal on \"%s\".\n"
                                  "Takes effect immediately - no restart.",
                                  config_.monitor.deviceName.c_str());
            }
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, max.y));
    ImGui::Dummy(ImVec2(width, 0.0f));
}

void App::renderNotices()
{
    // Things the user has to know about, in one slim band under the header,
    // present only while there is something to say.
    struct Notice {
        ImU32       colour;
        std::string text;
        bool        copyable;
    };
    std::vector<Notice> notices;

    if (!startupError_.empty())
        notices.push_back({theme::kDanger, startupError_, true});

    const bool running = engine_->isRunning();
    if (running && meters_.inputStarved.load()) {
        notices.push_back(
            {theme::kWarning,
             "\"" + config_.input.deviceName +
                 "\" is open but is not delivering any audio. Check that the device is not "
                 "muted or disabled in Windows sound settings, and - for a virtual "
                 "microphone - that the application driving it is actually running.",
             false});
    }

    // Reports rather than instructs: the restart is already on its way.
    if (running && deviceSelectionDiffers())
        notices.push_back({theme::kTextDim, "Device settings changed - reopening the stream...", false});

    if (notices.empty())
        return;

    const float pad = px(20.0f);
    ImGui::Dummy(ImVec2(0.0f, px(8.0f)));
    for (const auto& notice : notices) {
        ImGui::SetCursorPosX(pad);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - pad);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(notice.colour));
        ImGui::TextWrapped("%s", notice.text.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();

        // The error message is the thing most worth pasting somewhere, and
        // ImGui text cannot be selected with the mouse.
        if (notice.copyable && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("click to copy");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                ImGui::SetClipboardText(notice.text.c_str());
        }
    }
}

// ---------------------------------------------------------------------------
// Rack
// ---------------------------------------------------------------------------

void App::renderRack(ImVec2 size)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(12.0f), px(12.0f)));
    ImGui::BeginChild("##rack", size, ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    {
        const ImVec2 a = ImGui::GetWindowPos();
        const ImVec2 b(a.x + ImGui::GetWindowSize().x, a.y + ImGui::GetWindowSize().y);
        // The plate stays put while its contents scroll.
        draw->PushClipRectFullScreen();
        drawFaceplate(draw, a, b, px(16.0f), IM_COL32(0x14, 0x18, 0x1E, 0xFF),
                      IM_COL32(0x0C, 0x0E, 0x12, 0xFF));
        draw->PopClipRect();
    }

    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8.0f), px(6.0f)));

    // --- heading ------------------------------------------------------------
    {
        const float rowH  = ImGui::GetFrameHeight() * 0.86f;
        const ImVec2 start = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(start.x + px(4.0f), start.y + (rowH - fonts_.caps->LegacySize) * 0.5f));
        capsLabel("SIGNAL CHAIN", theme::kTextDim);
        const float addW = chipWidth(fonts_.caps, "+ VST3");
        ImGui::SetCursorScreenPos(ImVec2(start.x + width - addW, start.y));
        if (chipButton("+ VST3", false)) {
            showPluginBrowser_ = true;
            pluginFilter_[0] = '\0';
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Add a VST3 plugin to the end of the chain");
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + rowH + px(8.0f)));
    }

    // --- level rows framing the chain ---------------------------------------
    auto levelRow = [&](const char* label, const History& history, bool warmTop) {
        const float rowH = px(22.0f);
        const ImVec2 a = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(width, rowH));

        pushFont(fonts_.caps);
        drawSpacedText(draw, ImVec2(a.x + px(8.0f), a.y + (rowH - fonts_.caps->LegacySize) * 0.5f),
                       theme::kTextFaint, label, 2.0f);
        ImGui::PopFont();

        const float barX = a.x + px(48.0f);
        const float barW = width - px(48.0f) - px(60.0f);
        const float barY = a.y + rowH * 0.5f;
        const float t    = std::clamp((history.latest() + 60.0f) / 60.0f, 0.0f, 1.0f);
        drawInset(draw, ImVec2(barX, barY - px(3.0f)), ImVec2(barX + barW, barY + px(3.0f)), px(3.0f));
        if (t > 0.0f) {
            const ImU32 end = warmTop ? levelGradient(t) : theme::kAccent;
            draw->AddRectFilledMultiColor(ImVec2(barX + 1, barY - px(2.0f)),
                                          ImVec2(barX + 1 + (barW - 2) * t, barY + px(2.0f)),
                                          theme::kAccentDim, end, end, theme::kAccentDim);
        }

        char text[16];
        if (history.latest() > -90.0f)
            std::snprintf(text, sizeof(text), "%+.1f", static_cast<double>(history.latest()));
        else
            std::snprintf(text, sizeof(text), "-inf");
        pushFont(fonts_.mono);
        const float tw = ImGui::CalcTextSize(text).x;
        draw->AddText(ImVec2(a.x + width - tw - px(4.0f), a.y + (rowH - fonts_.mono->LegacySize) * 0.5f),
                      theme::kTextDim, text);
        ImGui::PopFont();
    };

    levelRow("IN", inputHistory_, false);

    // --- units --------------------------------------------------------------
    // As tall as the design wants when there is room, shorter when the chain
    // is long or the window low, so the whole path stays in view.
    const float units     = static_cast<float>(chainNodes_.size() + 1);
    const float reserved  = px(22.0f) * 2.0f + px(36.0f) + px(16.0f);
    const float fitHeight = (ImGui::GetContentRegionAvail().y - reserved) / units - ImGui::GetStyle().ItemSpacing.y;
    const float rowHeight = std::clamp(fitHeight, px(48.0f), px(66.0f));
    const float rowPitch  = rowHeight + ImGui::GetStyle().ItemSpacing.y;
    const float listTop   = ImGui::GetCursorScreenPos().y;

    const bool dragging = dragNodeId_ != 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (dragNodeId_ != 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        dragNodeId_ = 0;

    size_t moveFrom = SIZE_MAX, moveTo = SIZE_MAX;

    // What a unit's little display shows: a trace from the history (0..1 from
    // the bottom, or from the top for gain reduction), and a readout.
    struct Face {
        std::string name;
        std::string kind;
        bool        enabled = true;
        const History* history = nullptr;
        float (*map)(float) = nullptr;
        bool        fromTop = false;
        bool        eqCurve = false;
        ImU32       colour  = theme::kAccentLight;
        char        readout[24] = {};
        ImU32       readoutColour = theme::kAccentLight;
    };

    auto drawUnit = [&](u64 id, const Face& face, bool selectable, bool* toggle, size_t index) {
        ImGui::PushID(static_cast<int>(id & 0x7FFFFFFF));
        const bool selected = selectedUnit_ == id;
        const bool isDragged = dragging && id == dragNodeId_;

        const ImVec2 a = ImGui::GetCursorScreenPos();
        const ImVec2 b(a.x + width, a.y + rowHeight);
        const float  powerArea = px(46.0f);

        ImGui::InvisibleButton("##unit", ImVec2(width - powerArea, rowHeight));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked() && selectable) {
            selectedUnit_ = id;
            showDevices_  = false;
        }
        if (index != SIZE_MAX && dragNodeId_ == 0 && ImGui::IsItemActive() &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.0f)) {
            dragNodeId_      = id;
            dragGrabOffsetY_ = ImGui::GetIO().MouseClickedPos[0].y - a.y;
        }
        if (index != SIZE_MAX && !dragging && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("Click to open - drag to move it in the chain");

        const float rounding = px(11.0f);
        const float dim      = face.enabled ? 1.0f : 0.5f;
        auto alpha = [&](ImU32 c) { return theme::withAlpha(c, theme::toVec4(c).w * dim); };

        // Lifted while it is being carried: a soft shadow under it, offset
        // downwards, so it reads as above the units sliding out of its way.
        if (isDragged) {
            for (int i = 1; i <= 4; ++i) {
                const float g = px(3.0f) * static_cast<float>(i);
                draw->AddRectFilled(ImVec2(a.x - g, a.y - g + px(8.0f)), ImVec2(b.x + g, b.y + g + px(8.0f)),
                                    IM_COL32(0, 0, 0, 0x22), rounding + g);
            }
        }

        if (selected || isDragged) {
            draw->AddRectFilled(ImVec2(a.x - 3, a.y - 3), ImVec2(b.x + 3, b.y + 3),
                                theme::withAlpha(theme::kAccent, 0.10f), rounding + 3);
            draw->AddRectFilledMultiColor(ImVec2(a.x, a.y + rounding), ImVec2(b.x, b.y - rounding),
                                          IM_COL32(0x16, 0x27, 0x2A, 0xFF), IM_COL32(0x16, 0x27, 0x2A, 0xFF),
                                          IM_COL32(0x0F, 0x18, 0x1A, 0xFF), IM_COL32(0x0F, 0x18, 0x1A, 0xFF));
            draw->AddRectFilled(a, ImVec2(b.x, a.y + rounding * 2), IM_COL32(0x16, 0x27, 0x2A, 0xFF), rounding,
                                ImDrawFlags_RoundTop);
            draw->AddRectFilled(ImVec2(a.x, b.y - rounding * 2), b, IM_COL32(0x0F, 0x18, 0x1A, 0xFF), rounding,
                                ImDrawFlags_RoundBottom);
            draw->AddRect(a, b, theme::kAccent, rounding, 1.2f);
        } else {
            drawFaceplate(draw, a, b, rounding, hovered ? IM_COL32(0x1E, 0x23, 0x2C, 0xFF) : IM_COL32(0x1A, 0x1E, 0x26, 0xFF),
                          IM_COL32(0x12, 0x15, 0x19, 0xFF));
        }

        // Status bar on the left edge.
        draw->AddRectFilled(ImVec2(a.x + px(10.0f), a.y + px(12.0f)), ImVec2(a.x + px(13.0f), b.y - px(12.0f)),
                            face.enabled ? theme::kAccent : theme::kBorderBright, 2.0f);
        if (face.enabled) {
            draw->AddRectFilled(ImVec2(a.x + px(8.0f), a.y + px(10.0f)), ImVec2(a.x + px(15.0f), b.y - px(10.0f)),
                                theme::withAlpha(theme::kAccent, 0.18f), 4.0f);
        }

        // Display well on the right.
        const float dispW = std::min(px(116.0f), width * 0.36f);
        const ImVec2 d0(b.x - powerArea - dispW - px(4.0f), a.y + px(13.0f));
        const ImVec2 d1(d0.x + dispW, b.y - px(13.0f));

        // Name and kind.
        const float textX = a.x + px(24.0f);
        const float textRoom = d0.x - textX - px(8.0f);
        pushFont(fonts_.title);
        const float nameSpacing = fonts_.title->LegacySize * 0.14f;
        std::string name = face.name;
        while (name.size() > 3 && spacedTextWidth(name.c_str(), nameSpacing) > textRoom)
            name.pop_back();
        if (name.size() < face.name.size())
            name += "..";
        drawSpacedText(draw, ImVec2(textX, a.y + rowHeight * 0.5f - fonts_.title->LegacySize - px(1.0f)),
                       alpha(selected ? theme::kText : IM_COL32(0xD9, 0xDE, 0xE6, 0xFF)), name.c_str(), nameSpacing);
        ImGui::PopFont();
        pushFont(fonts_.caps);
        std::string kind = face.kind;
        while (kind.size() > 3 && spacedTextWidth(kind.c_str(), 1.4f) > textRoom)
            kind.pop_back();
        drawSpacedText(draw, ImVec2(textX, a.y + rowHeight * 0.5f + px(4.0f)), alpha(theme::kTextFaint),
                       kind.c_str(), 1.4f);
        ImGui::PopFont();

        drawInset(draw, d0, d1, px(6.0f));
        draw->PushClipRect(ImVec2(d0.x + 1, d0.y + 1), ImVec2(d1.x - 1, d1.y - 1), true);
        const float dw = d1.x - d0.x - 2.0f;
        const float dh = d1.y - d0.y - 2.0f;
        if (face.eqCurve && face.enabled) {
            ImVec2 points[48];
            const float sr = static_cast<float>(currentSampleRate());
            for (int i = 0; i < 48; ++i) {
                const float t  = static_cast<float>(i) / 47.0f;
                const float hz = 20.0f * std::pow(1000.0f, t);
                const float db = dsp::GraphicEq::responseDb(params_, hz, sr);
                points[i] = ImVec2(d0.x + 1 + dw * t,
                                   d0.y + 1 + dh * 0.5f - std::clamp(db / 18.0f, -1.0f, 1.0f) * dh * 0.45f);
            }
            for (int i = 0; i + 1 < 48; ++i) {
                draw->AddQuadFilled(points[i], points[i + 1], ImVec2(points[i + 1].x, d0.y + 1 + dh * 0.5f),
                                    ImVec2(points[i].x, d0.y + 1 + dh * 0.5f),
                                    theme::withAlpha(face.colour, 0.16f));
            }
            draw->AddPolyline(points, 48, face.colour, 1.4f);
        } else if (face.history && face.map && face.enabled) {
            constexpr int kPoints = 58;
            ImVec2 points[kPoints];
            for (int i = 0; i < kPoints; ++i) {
                const int   index = History::kLength - kPoints + i;
                const float v     = std::clamp(face.map(face.history->at(index)), 0.0f, 1.0f);
                points[i] = ImVec2(d0.x + 1 + dw * static_cast<float>(i) / (kPoints - 1),
                                   face.fromTop ? d0.y + 1 + v * dh : d1.y - 1 - v * dh);
            }
            const float base = face.fromTop ? d0.y + 1 : d1.y - 1;
            for (int i = 0; i + 1 < kPoints; ++i) {
                draw->AddQuadFilled(points[i], points[i + 1], ImVec2(points[i + 1].x, base),
                                    ImVec2(points[i].x, base), theme::withAlpha(face.colour, 0.22f));
            }
            draw->AddPolyline(points, kPoints, face.colour, 1.4f);
        } else {
            draw->AddLine(ImVec2(d0.x + 2, d1.y - 3), ImVec2(d1.x - 2, d1.y - 3), theme::kBorderBright);
        }
        draw->PopClipRect();

        pushFont(fonts_.mono);
        const char* readout = face.enabled ? face.readout : "OFF";
        const ImVec2 rs = ImGui::CalcTextSize(readout);
        draw->AddRectFilled(ImVec2(d1.x - rs.x - px(8.0f), d1.y - rs.y - px(3.0f)), ImVec2(d1.x - 1, d1.y - 1),
                            theme::withAlpha(theme::kInset, 0.75f), px(4.0f));
        draw->AddText(ImVec2(d1.x - rs.x - px(5.0f), d1.y - rs.y - px(2.0f)),
                      face.enabled ? face.readoutColour : theme::kTextFaint, readout);
        ImGui::PopFont();

        // Power.
        draw->AddLine(ImVec2(b.x - powerArea, a.y + px(8.0f)), ImVec2(b.x - powerArea, b.y - px(8.0f)),
                      IM_COL32(0x07, 0x09, 0x0C, 0xFF));
        const float powerSize = px(26.0f);
        ImGui::SetCursorScreenPos(ImVec2(b.x - powerArea * 0.5f - powerSize * 0.5f, a.y + (rowHeight - powerSize) * 0.5f));
        if (powerButton("##power", toggle ? *toggle : face.enabled, powerSize) && toggle)
            *toggle = !*toggle;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(face.enabled ? "On - click to switch it off" : "Switched off");

        ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + ImGui::GetStyle().ItemSpacing.y));
        ImGui::PopID();

    };

    auto faceFor = [&](dsp::ProcessorNode* node) {
        Face face;
        face.enabled = node->isEnabled() && !params_.bypassAll.load();

        switch (node->kind()) {
            case dsp::NodeKind::NoiseSuppressor:
                face.name    = "SUPPRESSOR";
                face.kind    = "NEURAL - BUILT-IN";
                face.history = &speechHistory_;
                face.map     = [](float v) { return 0.08f + v * 0.84f; };
                std::snprintf(face.readout, sizeof(face.readout), "%.0f%%",
                              static_cast<double>(params_.denoiseAmount.load() * 100.0f));
                break;
            case dsp::NodeKind::Gate:
                face.name    = "GATE";
                face.kind    = "DYNAMICS - BUILT-IN";
                face.history = &gateOpenHistory_;
                face.map     = [](float v) { return 0.12f + v * 0.66f; };
                face.colour  = theme::kAccent;
                std::snprintf(face.readout, sizeof(face.readout), "%s",
                              meters_.gateOpen.load() ? "OPEN" : "SHUT");
                break;
            case dsp::NodeKind::Equalizer:
                face.name    = "EQUALIZER";
                face.kind    = "TONE - BUILT-IN";
                face.eqCurve = true;
                std::snprintf(face.readout, sizeof(face.readout), "10 BD");
                break;
            case dsp::NodeKind::Compressor:
                face.name    = "COMPRESSOR";
                face.kind    = "DYNAMICS - BUILT-IN";
                face.history = &compHistory_;
                face.map     = [](float v) { return 0.06f + std::min(1.0f, -v / 18.0f) * 0.9f; };
                face.fromTop = true;
                face.colour  = theme::kReduction;
                face.readoutColour = theme::kReductionLight;
                std::snprintf(face.readout, sizeof(face.readout), "%.1f",
                              tidy(meters_.compressorReductionDb.load()));
                break;
            case dsp::NodeKind::Vst3Plugin: {
                auto* plugin = static_cast<host::Vst3Plugin*>(node);
                face.name    = upper(plugin->name());
                face.kind    = plugin->descriptor().vendor.empty()
                                   ? std::string("VST3 PLUGIN")
                                   : "VST3 - " + upper(plugin->descriptor().vendor);
                face.history = &outputHistory_;
                face.map     = [](float v) { return std::clamp((v + 60.0f) / 60.0f, 0.0f, 1.0f) * 0.8f; };
                face.colour  = theme::kTextFaint;
                face.readoutColour = theme::kTextDim;
                std::snprintf(face.readout, sizeof(face.readout), "VST3");
                break;
            }
        }
        return face;
    };

    // Units sit at animated positions rather than wherever the layout puts
    // them: when the chain reorders under a drag, the others slide into their
    // new slots instead of jumping, and the carried unit follows the pointer
    // on the vertical axis only. It is drawn last, so it passes over the rest.
    const float  listX = ImGui::GetCursorScreenPos().x;
    const size_t count = chainNodes_.size();
    const float  ease  = 1.0f - std::exp(-std::min(ImGui::GetIO().DeltaTime, 0.05f) * 18.0f);

    std::vector<size_t> drawOrder;
    drawOrder.reserve(count);
    size_t carried = SIZE_MAX;
    for (size_t i = 0; i < count; ++i) {
        if (dragging && chainNodes_[i]->id() == dragNodeId_)
            carried = i;
        else
            drawOrder.push_back(i);
    }
    if (carried != SIZE_MAX)
        drawOrder.push_back(carried);

    for (size_t i : drawOrder) {
        auto& node = chainNodes_[i];
        const Face face = faceFor(node.get());

        const float target = static_cast<float>(i) * rowPitch;
        float& y = unitY_.try_emplace(node->id(), target).first->second;
        if (i == carried) {
            y = std::clamp(ImGui::GetIO().MousePos.y - dragGrabOffsetY_ - listTop, 0.0f,
                           static_cast<float>(count - 1) * rowPitch);
        } else {
            y += (target - y) * ease;
            if (std::abs(target - y) < 0.5f)
                y = target;
        }
        ImGui::SetCursorScreenPos(ImVec2(listX, listTop + y));

        bool enabled = node->isEnabled();
        const bool before = enabled;
        drawUnit(node->id(), face, true, &enabled, i);
        if (enabled != before) {
            node->setEnabled(enabled);
            markDirty();
        }

        // The slot under the carried unit's centre is where it belongs.
        if (i == carried) {
            const auto slot = static_cast<size_t>(std::clamp(
                static_cast<int>(std::lround(y / rowPitch)), 0, static_cast<int>(count) - 1));
            if (slot != i) {
                moveFrom = i;
                moveTo   = slot;
            }
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(listX, listTop + static_cast<float>(count) * rowPitch));

    // The limiter sits after the chain, not in it, and is drawn as the last
    // unit so the rack reads as the whole signal path.
    {
        Face face;
        face.name    = "LIMITER";
        face.kind    = "ALWAYS LAST";
        face.enabled = params_.limiterEnabled.load();
        face.history = &limiterHistory_;
        face.map     = [](float v) { return std::min(1.0f, -v / 12.0f) * 0.9f; };
        face.fromTop = true;
        face.colour  = theme::kReduction;
        face.readoutColour = theme::kReductionLight;
        std::snprintf(face.readout, sizeof(face.readout), "%.1f",
                      tidy(meters_.limiterReductionDb.load()));

        bool enabled = face.enabled;
        drawUnit(kLimiterUnit, face, true, &enabled, SIZE_MAX);
        if (enabled != face.enabled) {
            params_.limiterEnabled.store(enabled);
            markDirty();
        }
    }

    levelRow("OUT", outputHistory_, true);

    ImGui::PopStyleVar();
    ImGui::EndChild();

    if (moveFrom != SIZE_MAX)
        moveNode(moveFrom, moveTo);
}

// ---------------------------------------------------------------------------
// Module view
// ---------------------------------------------------------------------------

void App::renderModuleView(ImVec2 size)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##module", size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // A little in from the child's edges, so the plate's glow is not clipped.
    const float  inset  = px(6.0f);
    const ImVec2 origin(ImGui::GetCursorScreenPos().x + inset, ImGui::GetCursorScreenPos().y + inset);
    const ImVec2 area(size.x - inset * 2.0f, size.y - inset * 2.0f);

    dsp::ProcessorNode* node = nullptr;
    if (selectedUnit_ != kLimiterUnit) {
        for (const auto& candidate : chainNodes_) {
            if (candidate->id() == selectedUnit_)
                node = candidate.get();
        }
        // The unit that was open has gone - a plugin removed - so open the
        // first one rather than nothing.
        if (!node && !chainNodes_.empty()) {
            node          = chainNodes_.front().get();
            selectedUnit_ = node->id();
        }
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8.0f), px(6.0f)));

    if (!node) {
        renderLimiterView(origin, area);
    } else {
        switch (node->kind()) {
            case dsp::NodeKind::NoiseSuppressor: renderSuppressorView(origin, area); break;
            case dsp::NodeKind::Gate:            renderGateView(origin, area);       break;
            case dsp::NodeKind::Equalizer:       renderEqualizerView(origin, area);  break;
            case dsp::NodeKind::Compressor:      renderCompressorView(origin, area); break;
            case dsp::NodeKind::Vst3Plugin:
                renderPluginView(static_cast<host::Vst3Plugin*>(node), origin, area);
                break;
        }
    }

    ImGui::PopStyleVar();
    ImGui::EndChild();
}

void App::renderCompressorView(ImVec2 origin, ImVec2 size)
{
    dsp::ProcessorNode* node = nullptr;
    for (const auto& n : chainNodes_)
        if (n->kind() == dsp::NodeKind::Compressor)
            node = n.get();

    ModuleFrame frame = drawModuleFrame(fonts_, origin, size, "COMPRESSOR",
                                        "BUILT-IN DYNAMICS", true, dpiScale_);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // --- header controls ------------------------------------------------------
    {
        bool enabled = params_.compEnabled.load();
        const float powerSize = px(36.0f);
        frame.controls.next(powerSize, powerSize);
        if (powerButton("##compPower", enabled, powerSize)) {
            if (node)
                node->setEnabled(!enabled);
            else
                params_.compEnabled.store(!enabled);
            params_.touch();
            markDirty();
        }

        const bool autoMakeup = params_.compAutoMakeup.load();
        frame.controls.next(chipWidth(fonts_.caps, "AUTO MAKEUP"), ImGui::GetFrameHeight() * 0.86f);
        if (chipButton("AUTO MAKEUP", autoMakeup)) {
            params_.compAutoMakeup.store(!autoMakeup);
            params_.touch();
            markDirty();
        }

        const char* modes[] = {"RMS", "PEAK"};
        int mode = params_.compRmsDetection.load() ? 0 : 1;
        pushFont(fonts_.caps);
        const float segW = spacedTextWidth("RMS", fonts_.caps->LegacySize * 0.12f) +
                           spacedTextWidth("PEAK", fonts_.caps->LegacySize * 0.12f) +
                           ImGui::GetFrameHeight() * 0.55f * 4.0f + 8.0f;
        ImGui::PopFont();
        frame.controls.next(segW, ImGui::GetFrameHeight());
        if (segmented("##detector", modes, 2, &mode)) {
            params_.compRmsDetection.store(mode == 0);
            params_.touch();
            markDirty();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("RMS follows loudness and sounds transparent on speech.\n"
                              "Peak catches every transient and reacts harder.");
        }
    }

    // --- layout -------------------------------------------------------------
    const float W    = frame.width();
    const float H    = frame.height();
    const float gap  = px(14.0f);
    const float d    = std::clamp(W / 12.5f, px(52.0f), px(74.0f));
    const float big  = d * 1.35f;
    const float rowH = knobHeight(fonts_, big);
    const float dispH = std::max(px(120.0f), H - rowH - gap);

    const float needleW   = std::clamp(W * 0.19f, px(150.0f), px(210.0f));
    const float transferW = std::min(dispH, W * 0.28f);
    const float histW     = W - transferW - needleW - gap * 2.0f;

    const float threshold = params_.compThresholdDb.load();
    const float ratio     = params_.compRatio.load();
    const float knee      = params_.compKneeDb.load();
    const bool  on        = params_.compEnabled.load() && !params_.bypassAll.load();

    // --- history ------------------------------------------------------------
    {
        const ImVec2 a = frame.bodyMin;
        const ImVec2 b(a.x + histW, a.y + dispH);
        drawInset(draw, a, b, px(10.0f));
        draw->PushClipRect(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, b.y - 1), true);

        const float w = b.x - a.x, h = b.y - a.y;
        auto levelY = [&](float db) { return a.y + std::clamp(-db / 72.0f, 0.0f, 1.0f) * h; };

        for (float db = -12.0f; db >= -60.0f; db -= 12.0f)
            draw->AddLine(ImVec2(a.x, levelY(db)), ImVec2(b.x, levelY(db)), theme::kGrid);
        for (int i = 1; i < 8; ++i)
            draw->AddLine(ImVec2(a.x + w * i / 8.0f, a.y), ImVec2(a.x + w * i / 8.0f, b.y), theme::kGrid);

        constexpr int N = History::kLength;
        fillHistory(draw, N, [&](int i) { return levelY(inputHistory_.at(i)); }, [&](int) { return b.y; },
                    a.x, w, theme::withAlpha(IM_COL32(0x5F, 0x68, 0x78, 0xFF), 0.35f));
        fillHistory(draw, N, [&](int i) { return a.y + std::clamp(-compHistory_.at(i) / 24.0f, 0.0f, 1.0f) * h * 0.5f; },
                    [&](int) { return a.y; }, a.x, w, theme::withAlpha(theme::kReduction, 0.55f));

        std::vector<ImVec2> out(N);
        for (int i = 0; i < N; ++i)
            out[i] = ImVec2(a.x + w * i / (N - 1.0f), levelY(outputHistory_.at(i)));
        drawGlowLine(draw, out.data(), N, theme::kAccentLight, 1.6f);

        dashedLine(draw, ImVec2(a.x, levelY(threshold)), ImVec2(b.x, levelY(threshold)), theme::kWarning, 6.0f, 4.0f);
        draw->PopClipRect();

        displayCaption(fonts_, ImVec2(a.x + px(12.0f), a.y + px(10.0f)), "LEVEL - GAIN REDUCTION");

        pushFont(fonts_.mono);
        char thr[24];
        std::snprintf(thr, sizeof(thr), "THR %.0f", static_cast<double>(threshold));
        draw->AddText(ImVec2(a.x + px(12.0f), levelY(threshold) - fonts_.mono->LegacySize - px(3.0f)),
                      theme::kWarning, thr);
        for (float db = -12.0f; db >= -60.0f; db -= 12.0f) {
            char label[8];
            std::snprintf(label, sizeof(label), "%.0f", static_cast<double>(db));
            const float lw = ImGui::CalcTextSize(label).x;
            draw->AddText(ImVec2(b.x - lw - px(8.0f), levelY(db) - fonts_.mono->LegacySize * 0.5f),
                          theme::kTextGhost, label);
        }
        ImGui::PopFont();

        // Legend.
        pushFont(fonts_.caps);
        float lx = b.x - px(12.0f);
        const float ly = a.y + px(10.0f);
        const struct { const char* text; ImU32 colour; bool line; } legend[] = {
            {"GR", theme::kReduction, false}, {"OUT", theme::kAccentLight, true}, {"IN", IM_COL32(0x5F, 0x68, 0x78, 0xFF), false}};
        for (const auto& item : legend) {
            const float tw = spacedTextWidth(item.text, 1.2f);
            lx -= tw;
            drawSpacedText(draw, ImVec2(lx, ly), theme::kTextFaint, item.text, 1.2f);
            lx -= px(14.0f);
            const float cy = ly + fonts_.caps->LegacySize * 0.5f;
            if (item.line)
                draw->AddLine(ImVec2(lx, cy), ImVec2(lx + px(9.0f), cy), item.colour, 2.0f);
            else
                draw->AddRectFilled(ImVec2(lx, cy - px(4.0f)), ImVec2(lx + px(8.0f), cy + px(4.0f)), item.colour, 2.0f);
            lx -= px(12.0f);
        }
        ImGui::PopFont();
    }

    // --- transfer curve -----------------------------------------------------
    {
        const ImVec2 a(frame.bodyMin.x + histW + gap, frame.bodyMin.y);
        const ImVec2 b(a.x + transferW, a.y + dispH);
        drawInset(draw, a, b, px(10.0f));
        draw->PushClipRect(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, b.y - 1), true);

        const float w = b.x - a.x, h = b.y - a.y;
        auto X = [&](float db) { return a.x + (db + 72.0f) / 72.0f * w; };
        auto Y = [&](float db) { return b.y - (db + 72.0f) / 72.0f * h; };

        for (float db = -12.0f; db > -72.0f; db -= 12.0f) {
            draw->AddLine(ImVec2(X(db), a.y), ImVec2(X(db), b.y), theme::kGrid);
            draw->AddLine(ImVec2(a.x, Y(db)), ImVec2(b.x, Y(db)), theme::kGrid);
        }
        dashedLine(draw, ImVec2(a.x, b.y), ImVec2(b.x, a.y), theme::kGridStrong, 3.0f, 4.0f);

        draw->AddRectFilled(ImVec2(X(threshold - knee * 0.5f), a.y), ImVec2(X(threshold + knee * 0.5f), b.y),
                            theme::withAlpha(theme::kWarning, 0.07f));
        dashedLine(draw, ImVec2(X(threshold), a.y), ImVec2(X(threshold), b.y), theme::kWarning, 5.0f, 4.0f);

        ImVec2 curve[121];
        for (int i = 0; i <= 120; ++i) {
            const float in = -72.0f + 0.6f * static_cast<float>(i);
            curve[i] = ImVec2(X(in), Y(on ? compressorCurve(in, threshold, ratio, knee) : in));
        }
        drawGlowLine(draw, curve, 121, theme::kAccent, 2.2f);

        const float now = inputHistory_.latest();
        if (now > -72.0f) {
            const ImVec2 dot(X(now), Y(on ? compressorCurve(now, threshold, ratio, knee) : now));
            dashedLine(draw, ImVec2(dot.x, b.y), dot, theme::withAlpha(theme::kAccentLight, 0.35f), 2.0f, 3.0f);
            dashedLine(draw, ImVec2(a.x, dot.y), dot, theme::withAlpha(theme::kAccentLight, 0.35f), 2.0f, 3.0f);
            draw->AddCircleFilled(dot, px(9.0f), theme::withAlpha(theme::kAccentLight, 0.18f), 20);
            draw->AddCircleFilled(dot, px(4.5f), theme::kAccentLight, 16);
        }
        draw->PopClipRect();

        displayCaption(fonts_, ImVec2(a.x + px(12.0f), a.y + px(10.0f)), "TRANSFER");
        pushFont(fonts_.mono);
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f : 1 - knee %.0f", static_cast<double>(ratio),
                      static_cast<double>(knee));
        const float tw = ImGui::CalcTextSize(text).x;
        draw->AddText(ImVec2(b.x - tw - px(10.0f), b.y - fonts_.mono->LegacySize - px(8.0f)),
                      theme::kTextFaint, text);
        ImGui::PopFont();
    }

    // --- gain reduction -----------------------------------------------------
    {
        const ImVec2 a(frame.bodyMax.x - needleW, frame.bodyMin.y);
        const float  needleH = dispH * 0.58f;
        ImGui::SetCursorScreenPos(a);
        needleMeter("##compNeedle", meters_.compressorReductionDb.load(), -20.0f, ImVec2(needleW, needleH));
        displayCaption(fonts_, ImVec2(a.x + px(12.0f), a.y + px(10.0f)), "GAIN REDUCTION");

        float maxGr = 0.0f;
        for (int i = 0; i < History::kLength; ++i)
            maxGr = std::min(maxGr, compHistory_.at(i));

        const ImVec2 r0(a.x, a.y + needleH + gap);
        const ImVec2 r1(a.x + needleW, a.y + dispH);
        drawInset(draw, r0, r1, px(10.0f));
        char now[16], sub[32];
        std::snprintf(now, sizeof(now), "%.1f", tidy(meters_.compressorReductionDb.load()));
        std::snprintf(sub, sizeof(sub), "dB - max %.1f", tidy(maxGr));
        pushFont(fonts_.monoLarge);
        const ImVec2 ns = ImGui::CalcTextSize(now);
        const float  cy = (r0.y + r1.y) * 0.5f;
        draw->AddText(ImVec2((r0.x + r1.x - ns.x) * 0.5f, cy - ns.y * 0.62f), theme::kReductionLight, now);
        ImGui::PopFont();
        pushFont(fonts_.mono);
        const ImVec2 ss = ImGui::CalcTextSize(sub);
        draw->AddText(ImVec2((r0.x + r1.x - ss.x) * 0.5f, cy + ns.y * 0.42f), theme::kTextFaint, sub);
        ImGui::PopFont();
    }

    // --- knobs --------------------------------------------------------------
    {
        const float rowTop = frame.bodyMax.y - rowH;
        const int   slots  = 8;
        const float slotW  = W / slots;
        int slot = 0;

        auto place = [&](float diameter) {
            const float h = knobHeight(fonts_, diameter);
            ImGui::SetCursorScreenPos(ImVec2(frame.bodyMin.x + slotW * (static_cast<float>(slot) + 0.5f) -
                                                 std::max(diameter, px(64.0f)) * 0.5f,
                                             rowTop + rowH - h));
            ++slot;
        };
        auto param = [&](const char* label, std::atomic<float>& target, float minimum, float maximum,
                         const char* format, float def, bool logarithmic, float diameter) {
            place(diameter);
            float value = target.load();
            const bool changed = logarithmic
                                     ? knobLog(label, &value, minimum, maximum, format, def, diameter)
                                     : knob(label, &value, minimum, maximum, format, def, diameter);
            if (changed) {
                target.store(value);
                params_.touch();
                markDirty();
            }
        };

        param("THRESHOLD", params_.compThresholdDb, -60.0f, 0.0f, "%.0f dB", -18.0f, false, big);
        param("RATIO", params_.compRatio, 1.0f, 20.0f, "%.1f : 1", 3.0f, true, big);
        param("KNEE", params_.compKneeDb, 0.0f, 24.0f, "%.0f dB", 6.0f, false, d);
        param("ATTACK", params_.compAttackMs, 0.1f, 200.0f, "%.1f ms", 8.0f, true, d);
        param("RELEASE", params_.compReleaseMs, 10.0f, 2000.0f, "%.0f ms", 120.0f, true, d);

        // With auto make-up on, the knob shows what the compressor computed
        // rather than a stale manual value it is not using.
        if (params_.compAutoMakeup.load()) {
            place(d);
            float computed = dsp::Compressor::autoMakeupDb(params_);
            ImGui::BeginDisabled();
            knob("MAKEUP", &computed, -12.0f, 24.0f, "%+.1f dB", 0.0f, d);
            ImGui::EndDisabled();
        } else {
            param("MAKEUP", params_.compMakeupDb, -12.0f, 24.0f, "%+.1f dB", 0.0f, false, d);
        }

        param("SC HP", params_.compSidechainHpfHz, 20.0f, 1000.0f, "%.0f Hz", 100.0f, true, d);
        param("LOOK", params_.compLookaheadMs, 0.0f, 20.0f, "%.1f ms", 2.0f, false, d);
    }

    char footer[96];
    std::snprintf(footer, sizeof(footer), "IN %+.1f dB    OUT %+.1f dB    SIDECHAIN HP %.0f Hz",
                  static_cast<double>(std::max(-99.9f, inputHistory_.latest())),
                  static_cast<double>(std::max(-99.9f, outputHistory_.latest())),
                  static_cast<double>(params_.compSidechainHpfHz.load()));
    footerText(fonts_, frame.footerText, footer);
}

void App::renderGateView(ImVec2 origin, ImVec2 size)
{
    dsp::ProcessorNode* node = nullptr;
    for (const auto& n : chainNodes_)
        if (n->kind() == dsp::NodeKind::Gate)
            node = n.get();

    ModuleFrame frame = drawModuleFrame(fonts_, origin, size, "GATE", "BUILT-IN DYNAMICS", true, dpiScale_);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    {
        const bool enabled = params_.gateEnabled.load();
        const float powerSize = px(36.0f);
        frame.controls.next(powerSize, powerSize);
        if (powerButton("##gatePower", enabled, powerSize)) {
            if (node)
                node->setEnabled(!enabled);
            else
                params_.gateEnabled.store(!enabled);
            params_.touch();
            markDirty();
        }
    }

    const float W     = frame.width();
    const float H     = frame.height();
    const float gap   = px(14.0f);
    const float d     = std::clamp(W / 12.5f, px(52.0f), px(74.0f));
    const float big   = d * 1.35f;
    const float rowH  = knobHeight(fonts_, big);
    const float dispH = std::max(px(120.0f), H - rowH - gap);
    const float sideW = std::clamp(W * 0.2f, px(160.0f), px(210.0f));
    const float dispW = W - sideW - gap;

    const float threshold  = params_.gateThresholdDb.load();
    const float hysteresis = params_.gateHysteresisDb.load();
    const bool  open       = meters_.gateOpen.load();

    // --- envelope -----------------------------------------------------------
    {
        const ImVec2 a = frame.bodyMin;
        const ImVec2 b(a.x + dispW, a.y + dispH);
        const float  stripH = px(22.0f);
        drawInset(draw, a, b, px(10.0f));

        const ImVec2 e1(b.x, b.y - stripH);
        draw->PushClipRect(ImVec2(a.x + 1, a.y + 1), ImVec2(e1.x - 1, e1.y), true);
        const float w = b.x - a.x, h = e1.y - a.y, c = a.y + h * 0.5f;
        auto amp = [&](float db) { return std::clamp((db + 80.0f) / 80.0f, 0.0f, 1.0f) * (h * 0.5f - px(6.0f)); };

        for (float db : {-20.0f, -40.0f, -60.0f}) {
            draw->AddLine(ImVec2(a.x, c - amp(db)), ImVec2(b.x, c - amp(db)), theme::kGrid);
            draw->AddLine(ImVec2(a.x, c + amp(db)), ImVec2(b.x, c + amp(db)), theme::kGrid);
        }
        draw->AddLine(ImVec2(a.x, c), ImVec2(b.x, c), theme::kGrid);

        constexpr int N = History::kLength;
        fillHistory(draw, N, [&](int i) { return c - amp(inputHistory_.at(i)); },
                    [&](int i) { return c + amp(inputHistory_.at(i)); }, a.x, w,
                    theme::withAlpha(IM_COL32(0x3A, 0x41, 0x4E, 0xFF), 0.6f));
        auto outDb = [&](int i) { return inputHistory_.at(i) + gateHistory_.at(i); };
        fillHistory(draw, N, [&](int i) { return c - amp(outDb(i)); }, [&](int i) { return c + amp(outDb(i)); },
                    a.x, w, theme::withAlpha(theme::kAccent, 0.45f));

        std::vector<ImVec2> top(N), bottom(N);
        for (int i = 0; i < N; ++i) {
            const float x = a.x + w * i / (N - 1.0f);
            top[i]    = ImVec2(x, c - amp(outDb(i)));
            bottom[i] = ImVec2(x, c + amp(outDb(i)));
        }
        drawGlowLine(draw, top.data(), N, theme::kAccentLight, 1.2f);
        drawGlowLine(draw, bottom.data(), N, theme::kAccentLight, 1.2f);

        for (int side : {-1, 1}) {
            dashedLine(draw, ImVec2(a.x, c + side * amp(threshold)), ImVec2(b.x, c + side * amp(threshold)),
                       theme::kWarning, 6.0f, 4.0f);
            dashedLine(draw, ImVec2(a.x, c + side * amp(threshold - hysteresis)),
                       ImVec2(b.x, c + side * amp(threshold - hysteresis)),
                       theme::withAlpha(theme::kWarning, 0.45f), 2.0f, 4.0f);
        }
        draw->PopClipRect();

        displayCaption(fonts_, ImVec2(a.x + px(12.0f), a.y + px(10.0f)), "ENVELOPE");
        pushFont(fonts_.mono);
        char label[32];
        std::snprintf(label, sizeof(label), "OPEN %.0f", static_cast<double>(threshold));
        float lw = ImGui::CalcTextSize(label).x;
        draw->AddText(ImVec2(b.x - lw - px(10.0f), c - amp(threshold) - fonts_.mono->LegacySize - px(3.0f)),
                      theme::kWarning, label);
        std::snprintf(label, sizeof(label), "CLOSE %.0f", static_cast<double>(threshold - hysteresis));
        lw = ImGui::CalcTextSize(label).x;
        draw->AddText(ImVec2(b.x - lw - px(10.0f), c - amp(threshold - hysteresis) + px(3.0f)),
                      theme::withAlpha(theme::kWarning, 0.7f), label);
        ImGui::PopFont();

        // Open and shut over the same six seconds.
        draw->AddRectFilled(ImVec2(a.x + 1, e1.y), ImVec2(b.x - 1, b.y - 1), IM_COL32(0x08, 0x0B, 0x0F, 0xFF), px(9.0f),
                            ImDrawFlags_RoundBottom);
        int start = 0;
        for (int i = 1; i <= N; ++i) {
            if (i == N || (gateOpenHistory_.at(i) > 0.5f) != (gateOpenHistory_.at(start) > 0.5f)) {
                const bool   isOpen = gateOpenHistory_.at(start) > 0.5f;
                const ImVec2 s0(a.x + w * start / static_cast<float>(N) + 1.0f, e1.y + px(6.0f));
                const ImVec2 s1(a.x + w * i / static_cast<float>(N) - 1.0f, b.y - px(6.0f));
                if (s1.x > s0.x) {
                    if (isOpen)
                        draw->AddRectFilled(ImVec2(s0.x, s0.y - 2), ImVec2(s1.x, s1.y + 2),
                                            theme::withAlpha(theme::kAccent, 0.25f), 4.0f);
                    draw->AddRectFilled(s0, s1, isOpen ? theme::kAccent : IM_COL32(0x1A, 0x1F, 0x27, 0xFF), 3.0f);
                }
                start = i;
            }
        }
    }

    // --- state lamp and reduction ---------------------------------------------
    {
        const ImVec2 a(frame.bodyMax.x - sideW, frame.bodyMin.y);
        const float  lampH = dispH - px(72.0f) - gap;
        const ImVec2 b(a.x + sideW, a.y + lampH);
        drawInset(draw, a, b, px(10.0f));

        const ImVec2 c((a.x + b.x) * 0.5f, a.y + lampH * 0.42f);
        const float  r = std::min(sideW, lampH) * 0.24f;
        if (open) {
            draw->AddCircleFilled(c, r * 1.6f, theme::withAlpha(theme::kAccent, 0.08f), 40);
            draw->AddCircle(c, r * 1.45f, theme::withAlpha(theme::kAccent, 0.5f), 40, 2.0f);
            for (int i = 0; i < 8; ++i) {
                const float s = static_cast<float>(i) / 7.0f;
                draw->AddCircleFilled(ImVec2(c.x - r * 0.15f * s, c.y - r * 0.2f * s), r * (1.0f - 0.7f * s),
                                      ImGui::ColorConvertFloat4ToU32(ImVec4(0.11f + 0.68f * s, 0.29f + 0.71f * s,
                                                                             0.28f + 0.69f * s, 1.0f)),
                                      32);
            }
            draw->AddCircleFilled(c, r * 1.15f, theme::withAlpha(theme::kAccent, 0.15f), 32);
        } else {
            draw->AddCircle(c, r * 1.45f, theme::kBorderBright, 40, 2.0f);
            draw->AddCircleFilled(c, r, IM_COL32(0x1A, 0x1E, 0x25, 0xFF), 32);
            draw->AddCircleFilled(ImVec2(c.x - r * 0.15f, c.y - r * 0.2f), r * 0.5f, IM_COL32(0x2A, 0x30, 0x3A, 0xFF), 24);
        }

        pushFont(fonts_.heading);
        const char* word    = open ? "OPEN" : "SHUT";
        const float spacing = fonts_.heading->LegacySize * 0.3f;
        const float ww      = spacedTextWidth(word, spacing);
        drawSpacedText(draw, ImVec2(c.x - ww * 0.5f, c.y + r * 1.75f), open ? theme::kAccentLight : theme::kTextFaint,
                       word, spacing);
        ImGui::PopFont();

        const ImVec2 g0(a.x, a.y + lampH + gap);
        const ImVec2 g1(a.x + sideW, a.y + dispH);
        drawInset(draw, g0, g1, px(10.0f));
        displayCaption(fonts_, ImVec2(g0.x + px(12.0f), (g0.y + g1.y) * 0.5f - fonts_.caps->LegacySize * 0.5f), "GR");
        char grText[16];
        std::snprintf(grText, sizeof(grText), "%.1f", tidy(meters_.gateReductionDb.load()));
        pushFont(fonts_.monoLarge);
        const ImVec2 gs = ImGui::CalcTextSize(grText);
        draw->AddText(ImVec2(g1.x - gs.x - px(14.0f), (g0.y + g1.y - gs.y) * 0.5f), theme::kReductionLight, grText);
        ImGui::PopFont();
    }

    // --- knobs --------------------------------------------------------------
    {
        const float rowTop = frame.bodyMax.y - rowH;
        const float slotW  = W / 8.0f;
        int slot = 0;
        auto param = [&](const char* label, std::atomic<float>& target, float minimum, float maximum,
                         const char* format, float def, bool logarithmic, float diameter) {
            const float h = knobHeight(fonts_, diameter);
            ImGui::SetCursorScreenPos(ImVec2(frame.bodyMin.x + slotW * (static_cast<float>(slot) + 0.5f) -
                                                 std::max(diameter, px(64.0f)) * 0.5f,
                                             rowTop + rowH - h));
            ++slot;
            float value = target.load();
            const bool changed = logarithmic
                                     ? knobLog(label, &value, minimum, maximum, format, def, diameter)
                                     : knob(label, &value, minimum, maximum, format, def, diameter);
            if (changed) {
                target.store(value);
                params_.touch();
                markDirty();
            }
        };

        param("THRESHOLD", params_.gateThresholdDb, -80.0f, 0.0f, "%.0f dB", -45.0f, false, big);
        param("RANGE", params_.gateRangeDb, -90.0f, -6.0f, "%.0f dB", -60.0f, false, big);
        param("HYST", params_.gateHysteresisDb, 0.0f, 24.0f, "%.0f dB", 6.0f, false, d);
        param("SC HP", params_.gateSidechainHpfHz, 20.0f, 1000.0f, "%.0f Hz", 120.0f, true, d);
        param("ATTACK", params_.gateAttackMs, 0.1f, 50.0f, "%.1f ms", 2.0f, true, d);
        param("HOLD", params_.gateHoldMs, 0.0f, 1000.0f, "%.0f ms", 120.0f, false, d);
        param("RELEASE", params_.gateReleaseMs, 5.0f, 2000.0f, "%.0f ms", 180.0f, true, d);
        param("LOOK", params_.gateLookaheadMs, 0.0f, 20.0f, "%.1f ms", 3.0f, false, d);
    }

    char footer[96];
    std::snprintf(footer, sizeof(footer), "IN %+.1f dB    STATE %s    CLOSES AT %.0f dB",
                  static_cast<double>(std::max(-99.9f, inputHistory_.latest())), open ? "OPEN" : "SHUT",
                  static_cast<double>(threshold - hysteresis));
    footerText(fonts_, frame.footerText, footer);
}

void App::renderEqualizerView(ImVec2 origin, ImVec2 size)
{
    dsp::ProcessorNode* node = nullptr;
    for (const auto& n : chainNodes_)
        if (n->kind() == dsp::NodeKind::Equalizer)
            node = n.get();

    ModuleFrame frame = drawModuleFrame(fonts_, origin, size, "EQUALIZER", "10-BAND PARAMETRIC", true, dpiScale_);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    {
        const bool enabled = params_.eqEnabled.load();
        const float powerSize = px(36.0f);
        frame.controls.next(powerSize, powerSize);
        if (powerButton("##eqPower", enabled, powerSize)) {
            if (node)
                node->setEnabled(!enabled);
            else
                params_.eqEnabled.store(!enabled);
            params_.touch();
            markDirty();
        }

        const float chipH = ImGui::GetFrameHeight() * 0.86f;
        const bool lpf = params_.lpfEnabled.load();
        frame.controls.next(chipWidth(fonts_.caps, "LOW-PASS"), chipH);
        if (chipButton("LOW-PASS", lpf)) {
            params_.lpfEnabled.store(!lpf);
            params_.touch();
            markDirty();
        }
        const bool hpf = params_.hpfEnabled.load();
        frame.controls.next(chipWidth(fonts_.caps, "HIGH-PASS"), chipH);
        if (chipButton("HIGH-PASS", hpf)) {
            params_.hpfEnabled.store(!hpf);
            params_.touch();
            markDirty();
        }
    }

    const float W      = frame.width();
    const float H      = frame.height();
    const float gap    = px(14.0f);
    const float tilesH = px(62.0f);
    const float sideW  = std::clamp(W * 0.2f, px(170.0f), px(220.0f));
    const float dispH  = H - tilesH - gap;

    // --- curve --------------------------------------------------------------
    ImGui::SetCursorScreenPos(frame.bodyMin);
    if (eqEditor("##eqCurve", params_, engine_->inputSpectrum(), static_cast<float>(currentSampleRate()),
                 ImVec2(W - sideW - gap, dispH), &selectedBand_)) {
        markDirty();
    }

    // --- selected band ------------------------------------------------------
    {
        const ImVec2 a(frame.bodyMax.x - sideW, frame.bodyMin.y);
        const ImVec2 b(frame.bodyMax.x, frame.bodyMin.y + dispH);
        drawInset(draw, a, b, px(10.0f));

        const int band = std::clamp(selectedBand_, 0, kEqBands - 1);
        char title[16];
        std::snprintf(title, sizeof(title), "BAND %d", band + 1);
        displayCaption(fonts_, ImVec2(a.x + px(14.0f), a.y + px(12.0f)), title);

        pushFont(fonts_.monoLarge);
        const std::string hz = formatHz(kEqCenters[band]);
        draw->AddText(ImVec2(a.x + px(14.0f), a.y + px(12.0f) + fonts_.caps->LegacySize + px(6.0f)),
                      theme::kAccentLight, hz.c_str());
        ImGui::PopFont();

        const float d     = std::clamp(sideW * 0.38f, px(56.0f), px(76.0f));
        const float knobH = knobHeight(fonts_, d);
        const float top   = a.y + px(12.0f) + fonts_.caps->LegacySize + px(6.0f) + fonts_.monoLarge->LegacySize + px(14.0f);
        const float room  = b.y - top - px(12.0f);
        const bool  stack = room >= knobH * 2.0f + px(10.0f);
        const float cx    = (a.x + b.x) * 0.5f;

        ImGui::PushID(band);
        float gain = params_.eqGainDb[band].load();
        ImGui::SetCursorScreenPos(stack ? ImVec2(cx - std::max(d, px(64.0f)) * 0.5f, top)
                                        : ImVec2(a.x + sideW * 0.25f - std::max(d, px(64.0f)) * 0.5f, top));
        if (knob("GAIN", &gain, -18.0f, 18.0f, "%+.1f dB", 0.0f, d)) {
            params_.eqGainDb[band].store(gain);
            params_.touch();
            markDirty();
        }
        float q = params_.eqQ[band].load();
        ImGui::SetCursorScreenPos(stack ? ImVec2(cx - std::max(d, px(64.0f)) * 0.5f, top + knobH + px(10.0f))
                                        : ImVec2(a.x + sideW * 0.75f - std::max(d, px(64.0f)) * 0.5f, top));
        if (knobLog("Q", &q, 0.2f, 8.0f, "%.2f", 1.0f, d)) {
            params_.eqQ[band].store(q);
            params_.touch();
            markDirty();
        }
        ImGui::PopID();
    }

    // --- band tiles ---------------------------------------------------------
    {
        const float tileGap = px(6.0f);
        const float tileW   = (W - tileGap * (kEqBands - 1)) / kEqBands;
        const float y       = frame.bodyMax.y - tilesH;

        for (int band = 0; band < kEqBands; ++band) {
            ImGui::PushID(band);
            const ImVec2 a(frame.bodyMin.x + band * (tileW + tileGap), y);
            const ImVec2 b(a.x + tileW, y + tilesH);
            ImGui::SetCursorScreenPos(a);
            if (ImGui::InvisibleButton("##tile", ImVec2(tileW, tilesH)))
                selectedBand_ = band;
            const bool hovered = ImGui::IsItemHovered();
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                params_.eqGainDb[band].store(0.0f);
                params_.touch();
                markDirty();
            }

            const bool  selected = selectedBand_ == band;
            const float gain     = params_.eqGainDb[band].load();
            const bool  engaged  = std::abs(gain) > 0.05f;

            if (selected) {
                draw->AddRectFilled(ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2),
                                    theme::withAlpha(theme::kAccent, 0.12f), px(11.0f));
                draw->AddRectFilledMultiColor(a, b, IM_COL32(0x14, 0x30, 0x2E, 0xFF), IM_COL32(0x14, 0x30, 0x2E, 0xFF),
                                              IM_COL32(0x0E, 0x1A, 0x1B, 0xFF), IM_COL32(0x0E, 0x1A, 0x1B, 0xFF));
                draw->AddRect(a, b, theme::kAccent, px(9.0f), 1.2f);
            } else {
                drawFaceplate(draw, a, b, px(9.0f), hovered ? IM_COL32(0x1E, 0x23, 0x2C, 0xFF) : IM_COL32(0x16, 0x1A, 0x21, 0xFF),
                              IM_COL32(0x10, 0x13, 0x18, 0xFF));
            }

            pushFont(fonts_.mono);
            char number[4];
            std::snprintf(number, sizeof(number), "%d", band + 1);
            draw->AddText(ImVec2(a.x + px(9.0f), a.y + px(7.0f)), selected ? theme::kAccentLight : theme::kTextFaint, number);
            draw->AddCircleFilled(ImVec2(b.x - px(10.0f), a.y + px(13.0f)), px(3.0f),
                                  engaged ? theme::kAccent : theme::kBorderBright, 12);

            const std::string hz = formatHz(kEqCenters[band]);
            const float fy = a.y + tilesH * 0.5f - px(2.0f);
            draw->AddText(ImVec2(a.x + px(9.0f), fy), theme::kText, hz.c_str());
            char gainText[16];
            std::snprintf(gainText, sizeof(gainText), "%+.1f", static_cast<double>(gain));
            draw->AddText(ImVec2(a.x + px(9.0f), fy + fonts_.mono->LegacySize + px(2.0f)),
                          engaged ? theme::kAccentLight : theme::kTextGhost, gainText);
            ImGui::PopFont();
            ImGui::PopID();
        }
    }

    char footer[128];
    char hp[24], lp[24];
    std::snprintf(hp, sizeof(hp), params_.hpfEnabled.load() ? "%s" : "off",
                  formatHz(params_.hpfHz.load()).c_str());
    std::snprintf(lp, sizeof(lp), params_.lpfEnabled.load() ? "%s" : "off",
                  formatHz(params_.lpfHz.load()).c_str());
    std::snprintf(footer, sizeof(footer), "HP %s    LP %s    drag a node - scroll sets Q - double-click resets", hp, lp);
    footerText(fonts_, frame.footerText, footer);
}

void App::renderSuppressorView(ImVec2 origin, ImVec2 size)
{
    const dsp::NoiseSuppressor* module = nullptr;
    dsp::ProcessorNode* node = nullptr;
    for (const auto& n : chainNodes_) {
        if (n->kind() == dsp::NodeKind::NoiseSuppressor) {
            node   = n.get();
            module = static_cast<const dsp::NoiseSuppressor*>(n.get());
        }
    }
    const bool unavailable = module && !module->isActive();

    ModuleFrame frame = drawModuleFrame(fonts_, origin, size, "SUPPRESSOR", "NEURAL NOISE REMOVAL", true, dpiScale_);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    {
        const bool enabled = params_.denoiseEnabled.load();
        const float powerSize = px(36.0f);
        frame.controls.next(powerSize, powerSize);
        ImGui::BeginDisabled(unavailable);
        if (powerButton("##supPower", enabled, powerSize)) {
            if (node)
                node->setEnabled(!enabled);
            else
                params_.denoiseEnabled.store(!enabled);
            markDirty();
        }
        ImGui::EndDisabled();

        // Four starting points for the one control: quicker to land near the
        // right amount than to drag there from scratch.
        const char*  names[]   = {"LIGHT", "NATURAL", "STRONG", "MAX"};
        const float  amounts[] = {0.4f, 0.6f, 0.8f, 1.0f};
        const float  amount    = params_.denoiseAmount.load();
        int preset = -1;
        for (int i = 0; i < 4; ++i)
            if (std::abs(amount - amounts[i]) < 0.005f)
                preset = i;

        pushFont(fonts_.caps);
        float segW = 8.0f;
        for (const char* n : names)
            segW += spacedTextWidth(n, fonts_.caps->LegacySize * 0.12f) + ImGui::GetFrameHeight() * 0.55f * 2.0f + 2.0f;
        ImGui::PopFont();
        frame.controls.next(segW, ImGui::GetFrameHeight());
        if (segmented("##presets", names, 4, &preset) && preset >= 0) {
            params_.denoiseAmount.store(amounts[preset]);
            markDirty();
        }
    }

    const float W     = frame.width();
    const float H     = frame.height();
    const float gap   = px(14.0f);
    const float sideW = std::clamp(W * 0.27f, px(220.0f), px(290.0f));
    const float vadH  = px(86.0f);
    const float topH  = H - vadH - gap;
    const float leftW = W - sideW - gap;

    // --- spectrum -----------------------------------------------------------
    {
        const ImVec2 a = frame.bodyMin;
        const ImVec2 b(a.x + leftW, a.y + topH);
        drawInset(draw, a, b, px(10.0f));
        draw->PushClipRect(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, b.y - 1), true);

        auto& in  = engine_->inputSpectrum();
        auto& out = engine_->outputSpectrum();
        in.update();
        out.update();

        const float w = b.x - a.x, h = b.y - a.y;
        const float sr = static_cast<float>(currentSampleRate());
        auto xAt = [&](float hz) { return a.x + std::log(hz / 20.0f) / std::log(1000.0f) * w; };
        auto yAt = [&](float db) { return b.y - std::clamp((db + 96.0f) / 90.0f, 0.0f, 1.0f) * h; };

        for (float hz : {50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f})
            draw->AddLine(ImVec2(xAt(hz), a.y), ImVec2(xAt(hz), b.y), theme::kGrid);
        for (float db : {-30.0f, -50.0f, -70.0f, -90.0f})
            draw->AddLine(ImVec2(a.x, yAt(db)), ImVec2(b.x, yAt(db)), theme::kGrid);

        auto sample = [&](dsp::SpectrumAnalyzer& s, float x) {
            const int bins = s.binCount();
            if (bins < 3)
                return -120.0f;
            const float hz0 = 20.0f * std::pow(1000.0f, (x - a.x) / w);
            const float hz1 = 20.0f * std::pow(1000.0f, (x + 2.0f - a.x) / w);
            const int i0 = std::clamp(static_cast<int>(hz0 * dsp::SpectrumAnalyzer::kFftSize / sr), 0, bins - 1);
            const int i1 = std::clamp(static_cast<int>(hz1 * dsp::SpectrumAnalyzer::kFftSize / sr), i0, bins - 1);
            float db = s.binDb(i0);
            for (int i = i0 + 1; i <= i1; ++i)
                db = std::max(db, s.binDb(i));
            return db;
        };

        std::vector<ImVec2> inLine, outLine;
        for (float x = a.x; x <= b.x; x += 2.0f) {
            inLine.emplace_back(x, yAt(sample(in, x)));
            outLine.emplace_back(x, yAt(sample(out, x)));
        }
        for (size_t i = 0; i + 1 < outLine.size(); ++i) {
            draw->AddQuadFilled(outLine[i], outLine[i + 1], ImVec2(outLine[i + 1].x, b.y), ImVec2(outLine[i].x, b.y),
                                theme::withAlpha(theme::kAccent, 0.22f));
            // What the chain took away between input and output, where it did.
            if (inLine[i].y < outLine[i].y && inLine[i + 1].y < outLine[i + 1].y) {
                draw->AddQuadFilled(inLine[i], inLine[i + 1], outLine[i + 1], outLine[i],
                                    theme::withAlpha(theme::kReduction, 0.18f));
            }
        }
        draw->AddPolyline(inLine.data(), static_cast<int>(inLine.size()), theme::kTextFaint, 1.2f);
        drawGlowLine(draw, outLine.data(), static_cast<int>(outLine.size()), theme::kAccentLight, 1.6f);
        draw->PopClipRect();

        displayCaption(fonts_, ImVec2(a.x + px(12.0f), a.y + px(10.0f)), "SPECTRUM - WHOLE CHAIN");
        pushFont(fonts_.caps);
        float lx = b.x - px(12.0f);
        const float ly = a.y + px(10.0f);
        const struct { const char* text; ImU32 colour; } legend[] = {
            {"REMOVED", theme::kReduction}, {"OUT", theme::kAccentLight}, {"IN", theme::kTextFaint}};
        for (const auto& item : legend) {
            const float tw = spacedTextWidth(item.text, 1.2f);
            lx -= tw;
            drawSpacedText(draw, ImVec2(lx, ly), theme::kTextFaint, item.text, 1.2f);
            lx -= px(14.0f);
            const float cy = ly + fonts_.caps->LegacySize * 0.5f;
            draw->AddRectFilled(ImVec2(lx, cy - px(4.0f)), ImVec2(lx + px(8.0f), cy + px(4.0f)), item.colour, 2.0f);
            lx -= px(12.0f);
        }
        ImGui::PopFont();

        pushFont(fonts_.mono);
        for (float hz : {100.0f, 1000.0f, 10000.0f}) {
            const std::string label = formatHz(hz);
            draw->AddText(ImVec2(xAt(hz) + px(4.0f), b.y - fonts_.mono->LegacySize - px(6.0f)), theme::kTextGhost,
                          label.c_str());
        }
        ImGui::PopFont();

        if (unavailable) {
            const std::string reasonText = module->inactiveReason();
            const char* reason = reasonText.c_str();
            pushFont(fonts_.medium);
            const ImVec2 ts = ImGui::CalcTextSize(reason, nullptr, false, w - px(60.0f));
            const ImVec2 m0((a.x + b.x - ts.x) * 0.5f - px(16.0f), (a.y + b.y - ts.y) * 0.5f - px(12.0f));
            const ImVec2 m1(m0.x + ts.x + px(32.0f), m0.y + ts.y + px(24.0f));
            draw->AddRectFilled(m0, m1, IM_COL32(0x10, 0x13, 0x1A, 0xF0), px(10.0f));
            draw->AddRect(m0, m1, theme::withAlpha(theme::kWarning, 0.6f), px(10.0f));
            draw->AddText(nullptr, 0.0f, ImVec2(m0.x + px(16.0f), m0.y + px(12.0f)), theme::kWarning, reason, nullptr,
                          w - px(60.0f));
            ImGui::PopFont();
        }
    }

    // --- amount -------------------------------------------------------------
    {
        const ImVec2 a(frame.bodyMax.x - sideW, frame.bodyMin.y);
        const ImVec2 b(frame.bodyMax.x, frame.bodyMin.y + topH);
        draw->AddRectFilled(a, b, IM_COL32(0x0B, 0x10, 0x12, 0xFF), px(10.0f));
        draw->AddRect(a, b, IM_COL32(0, 0, 0, 0xFF), px(10.0f));
        draw->AddCircleFilled(ImVec2((a.x + b.x) * 0.5f, a.y + (b.y - a.y) * 0.45f), sideW * 0.45f,
                              theme::withAlpha(theme::kAccent, 0.05f), 48);

        const float d     = std::min(sideW * 0.62f, (b.y - a.y) * 0.62f);
        const float knobH = knobHeight(fonts_, d);
        ImGui::SetCursorScreenPos(ImVec2((a.x + b.x) * 0.5f - d * 0.5f, a.y + (b.y - a.y - knobH) * 0.5f));
        ImGui::BeginDisabled(unavailable);
        float amount = params_.denoiseAmount.load() * 100.0f;
        if (knob("AMOUNT", &amount, 0.0f, 100.0f, "%.0f %%", 90.0f, d)) {
            params_.denoiseAmount.store(amount * 0.01f);
            markDirty();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "How much of the cleaned signal replaces the original.\n"
                "Unlike the gate, this removes steady noise while you are talking -\n"
                "but taking all of it can sound processed in a quiet room, so\n"
                "leaving a little of the original in is the usual remedy.\n"
                "Costs 10 ms of latency whenever this module is in the chain.");
        }
    }

    // --- voice activity -----------------------------------------------------
    {
        const ImVec2 a(frame.bodyMin.x, frame.bodyMax.y - vadH);
        const ImVec2 b(a.x + leftW, frame.bodyMax.y);
        drawInset(draw, a, b, px(10.0f));
        displayCaption(fonts_, ImVec2(a.x + px(12.0f), a.y + px(10.0f)), "VOICE ACTIVITY - LAST 6 S");

        constexpr int kBars = 60;
        const float barsTop = a.y + px(10.0f) + fonts_.caps->LegacySize + px(8.0f);
        const float barsW   = b.x - a.x - px(24.0f);
        const float barW    = barsW / kBars;
        for (int i = 0; i < kBars; ++i) {
            const int   from = i * History::kLength / kBars;
            const int   to   = (i + 1) * History::kLength / kBars;
            float value = 0.0f;
            for (int j = from; j < to; ++j)
                value = std::max(value, speechHistory_.at(j));
            const float x0 = a.x + px(12.0f) + i * barW;
            const float h  = std::max(px(3.0f), value * (b.y - barsTop - px(10.0f)));
            const ImVec2 r0(x0 + 1.0f, b.y - px(10.0f) - h), r1(x0 + barW - 1.5f, b.y - px(10.0f));
            if (value > 0.5f) {
                draw->AddRectFilled(ImVec2(r0.x - 1, r0.y - 1), ImVec2(r1.x + 1, r1.y), theme::withAlpha(theme::kAccent, 0.2f), 2.0f);
                draw->AddRectFilledMultiColor(r0, r1, theme::kAccentLight, theme::kAccentLight, theme::kAccentDim,
                                              theme::kAccentDim);
            } else {
                draw->AddRectFilled(r0, r1, IM_COL32(0x1F, 0x25, 0x2E, 0xFF), 2.0f);
            }
        }
    }

    {
        const ImVec2 a(frame.bodyMax.x - sideW, frame.bodyMax.y - vadH);
        const ImVec2 b(frame.bodyMax.x, frame.bodyMax.y);
        drawInset(draw, a, b, px(10.0f));
        displayCaption(fonts_, ImVec2(a.x + px(14.0f), a.y + px(14.0f)), "SPEECH NOW");

        const float speech = unavailable ? 0.0f : meters_.speechProbability.load();
        char text[16];
        std::snprintf(text, sizeof(text), "%.0f%%", static_cast<double>(speech * 100.0f));
        pushFont(fonts_.monoLarge);
        const ImVec2 ts = ImGui::CalcTextSize(text);
        draw->AddText(ImVec2(b.x - ts.x - px(14.0f), a.y + px(8.0f)), theme::kText, text);
        ImGui::PopFont();

        const ImVec2 m0(a.x + px(14.0f), b.y - px(22.0f)), m1(b.x - px(14.0f), b.y - px(14.0f));
        draw->AddRectFilled(m0, m1, IM_COL32(0x0F, 0x13, 0x18, 0xFF), px(4.0f));
        if (speech > 0.01f) {
            const ImVec2 f1(m0.x + (m1.x - m0.x) * speech, m1.y);
            draw->AddRectFilled(ImVec2(m0.x, m0.y - 2), ImVec2(f1.x, f1.y + 2), theme::withAlpha(theme::kAccent, 0.2f), px(5.0f));
            draw->AddRectFilledMultiColor(m0, f1, theme::kAccentDim, theme::kAccentLight, theme::kAccentLight, theme::kAccentDim);
        }
    }

    footerText(fonts_, frame.footerText, "costs 10 ms of latency while it is in the chain");
}

void App::renderLimiterView(ImVec2 origin, ImVec2 size)
{
    ModuleFrame frame = drawModuleFrame(fonts_, origin, size, "LIMITER", "FINAL STAGE - BRICKWALL BEFORE THE OUTPUT",
                                        true, dpiScale_);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    {
        bool enabled = params_.limiterEnabled.load();
        const float powerSize = px(36.0f);
        frame.controls.next(powerSize, powerSize);
        if (powerButton("##limPower", enabled, powerSize)) {
            params_.limiterEnabled.store(!enabled);
            markDirty();
        }
    }

    const float W      = frame.width();
    const float H      = frame.height();
    const float gap    = px(14.0f);
    const float sideW  = std::clamp(W * 0.27f, px(220.0f), px(290.0f));
    const float meterH = px(70.0f);
    const float leftW  = W - sideW - gap;
    const float dispH  = H - meterH - gap;
    const float ceiling = params_.limiterCeilingDb.load();
    const bool  on      = params_.limiterEnabled.load();

    // --- peak history -------------------------------------------------------
    {
        const ImVec2 a = frame.bodyMin;
        const ImVec2 b(a.x + leftW, a.y + dispH);
        drawInset(draw, a, b, px(10.0f));
        draw->PushClipRect(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, b.y - 1), true);

        const float w = b.x - a.x, h = b.y - a.y;
        auto Y = [&](float db) { return a.y + h * 0.06f + std::clamp(-db / 36.0f, 0.0f, 1.0f) * h * 0.9f; };
        for (float db : {-6.0f, -12.0f, -18.0f, -24.0f, -30.0f})
            draw->AddLine(ImVec2(a.x, Y(db)), ImVec2(b.x, Y(db)), theme::kGrid);
        for (int i = 1; i < 10; ++i)
            draw->AddLine(ImVec2(a.x + w * i / 10.0f, a.y), ImVec2(a.x + w * i / 10.0f, b.y), theme::kGrid);

        constexpr int N = History::kLength;
        // What arrived is what left plus what was taken off it.
        fillHistory(draw, N, [&](int i) { return Y(outputHistory_.at(i) - limiterHistory_.at(i)); },
                    [&](int i) { return Y(outputHistory_.at(i)); }, a.x, w, theme::withAlpha(theme::kReduction, 0.55f));
        fillHistory(draw, N, [&](int i) { return Y(outputHistory_.at(i)); }, [&](int) { return b.y; }, a.x, w,
                    theme::withAlpha(theme::kAccent, 0.32f));
        std::vector<ImVec2> line(N);
        for (int i = 0; i < N; ++i)
            line[i] = ImVec2(a.x + w * i / (N - 1.0f), Y(outputHistory_.at(i)));
        drawGlowLine(draw, line.data(), N, theme::kAccentLight, 1.4f);

        if (on) {
            const ImVec2 c0(a.x, Y(ceiling)), c1(b.x, Y(ceiling));
            draw->AddLine(c0, c1, theme::withAlpha(IM_COL32(255, 255, 255, 255), 0.25f), 5.0f);
            draw->AddLine(c0, c1, IM_COL32(0xF2, 0xF4, 0xF7, 0xFF), 1.4f);
        }
        draw->PopClipRect();

        displayCaption(fonts_, ImVec2(a.x + px(12.0f), a.y + px(10.0f)), "PEAK HISTORY");
        pushFont(fonts_.mono);
        if (on) {
            char text[24];
            std::snprintf(text, sizeof(text), "CEILING %.1f", static_cast<double>(ceiling));
            const float tw = ImGui::CalcTextSize(text).x;
            draw->AddText(ImVec2(b.x - tw - px(12.0f), Y(ceiling) + px(4.0f)), IM_COL32(0xF2, 0xF4, 0xF7, 0xFF), text);
        }
        for (float db : {-12.0f, -24.0f}) {
            char label[8];
            std::snprintf(label, sizeof(label), "%.0f", static_cast<double>(db));
            draw->AddText(ImVec2(a.x + px(10.0f), Y(db) - fonts_.mono->LegacySize - px(2.0f)), theme::kTextGhost, label);
        }
        ImGui::PopFont();

        pushFont(fonts_.caps);
        float lx = a.x + px(12.0f);
        const float ly = b.y - px(10.0f) - fonts_.caps->LegacySize;
        const struct { const char* text; ImU32 colour; } legend[] = {
            {"PASSED", theme::kAccent}, {"CAUGHT", theme::kReduction}};
        for (const auto& item : legend) {
            const float cy = ly + fonts_.caps->LegacySize * 0.5f;
            draw->AddRectFilled(ImVec2(lx, cy - px(4.0f)), ImVec2(lx + px(8.0f), cy + px(4.0f)), item.colour, 2.0f);
            lx += px(14.0f);
            drawSpacedText(draw, ImVec2(lx, ly), theme::kTextFaint, item.text, 1.2f);
            lx += spacedTextWidth(item.text, 1.2f) + px(16.0f);
        }
        ImGui::PopFont();
    }

    // --- output meter -------------------------------------------------------
    {
        const ImVec2 a(frame.bodyMin.x, frame.bodyMax.y - meterH);
        const ImVec2 b(a.x + leftW, frame.bodyMax.y);
        drawInset(draw, a, b, px(10.0f));
        displayCaption(fonts_, ImVec2(a.x + px(14.0f), a.y + px(12.0f)), "OUTPUT");

        const float peak = outputHistory_.latest();
        char text[24];
        std::snprintf(text, sizeof(text), peak > -90.0f ? "%+.1f dBFS" : "-inf", static_cast<double>(peak));
        pushFont(fonts_.mono);
        const float tw = ImGui::CalcTextSize(text).x;
        draw->AddText(ImVec2(b.x - tw - px(14.0f), a.y + px(10.0f)), theme::kText, text);
        ImGui::PopFont();

        const float barY = b.y - px(26.0f);
        ImGui::SetCursorScreenPos(ImVec2(a.x + px(14.0f), barY));
        const float barW = leftW - px(28.0f);
        levelMeter("##limOut", peak, toDb(meters_.outputRms.load()), ImVec2(barW, px(12.0f)), true);
        if (on) {
            const float cx = a.x + px(14.0f) + std::clamp((ceiling + 60.0f) / 60.0f, 0.0f, 1.0f) * barW;
            draw->AddLine(ImVec2(cx, barY - px(3.0f)), ImVec2(cx, barY + px(15.0f)), IM_COL32(0xF2, 0xF4, 0xF7, 0xFF), 2.0f);
        }
    }

    // --- controls -----------------------------------------------------------
    {
        const ImVec2 a(frame.bodyMax.x - sideW, frame.bodyMin.y);
        const ImVec2 b(frame.bodyMax.x, frame.bodyMax.y - meterH - gap);
        draw->AddRectFilled(a, b, IM_COL32(0x0B, 0x10, 0x12, 0xFF), px(10.0f));
        draw->AddRect(a, b, IM_COL32(0, 0, 0, 0xFF), px(10.0f));

        const float bigD   = std::min(sideW * 0.46f, (b.y - a.y) * 0.34f);
        const float smallD = bigD * 0.7f;
        const float total  = knobHeight(fonts_, bigD) + knobHeight(fonts_, smallD) + px(12.0f);
        float y = a.y + (b.y - a.y - total) * 0.5f;
        const float cx = (a.x + b.x) * 0.5f;

        ImGui::SetCursorScreenPos(ImVec2(cx - std::max(bigD, px(64.0f)) * 0.5f, y));
        float value = params_.limiterCeilingDb.load();
        if (knob("CEILING", &value, -12.0f, 0.0f, "%.1f dB", -1.0f, bigD)) {
            params_.limiterCeilingDb.store(value);
            markDirty();
        }
        y += knobHeight(fonts_, bigD) + px(12.0f);
        ImGui::SetCursorScreenPos(ImVec2(cx - std::max(smallD, px(64.0f)) * 0.5f, y));
        value = params_.limiterReleaseMs.load();
        if (knobLog("RELEASE", &value, 10.0f, 500.0f, "%.0f ms", 80.0f, smallD)) {
            params_.limiterReleaseMs.store(value);
            markDirty();
        }
    }

    {
        const ImVec2 a(frame.bodyMax.x - sideW, frame.bodyMax.y - meterH);
        const ImVec2 b(frame.bodyMax.x, frame.bodyMax.y);
        drawInset(draw, a, b, px(10.0f));

        float maxGr = 0.0f;
        for (int i = 0; i < History::kLength; ++i)
            maxGr = std::min(maxGr, limiterHistory_.at(i));

        const float half = (b.x - a.x) * 0.5f;
        draw->AddLine(ImVec2(a.x + half, a.y + px(10.0f)), ImVec2(a.x + half, b.y - px(10.0f)), theme::kGrid);
        const struct { const char* label; float value; float x; } cells[] = {
            {"GR NOW", meters_.limiterReductionDb.load(), a.x}, {"GR MAX", maxGr, a.x + half}};
        for (const auto& cell : cells) {
            pushFont(fonts_.caps);
            const float lw = spacedTextWidth(cell.label, 1.4f);
            drawSpacedText(draw, ImVec2(cell.x + (half - lw) * 0.5f, a.y + px(10.0f)), theme::kTextFaint, cell.label, 1.4f);
            ImGui::PopFont();
            char text[16];
            std::snprintf(text, sizeof(text), "%.1f", tidy(cell.value));
            pushFont(fonts_.monoLarge);
            const ImVec2 ts = ImGui::CalcTextSize(text);
            draw->AddText(ImVec2(cell.x + (half - ts.x) * 0.5f, b.y - ts.y - px(8.0f)), theme::kReductionLight, text);
            ImGui::PopFont();
        }
    }

    footerText(fonts_, frame.footerText,
               "Catches overshoots before the output, where the receiving application could not undo them.");
}

void App::renderPluginView(host::Vst3Plugin* plugin, ImVec2 origin, ImVec2 size)
{
    const std::string name   = upper(plugin->name());
    const std::string vendor = plugin->descriptor().vendor.empty()
                                   ? std::string("VST3 - THIRD-PARTY")
                                   : "VST3 - " + upper(plugin->descriptor().vendor);

    ModuleFrame frame = drawModuleFrame(fonts_, origin, size, name.c_str(), vendor.c_str(), false, dpiScale_);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    size_t index = SIZE_MAX;
    for (size_t i = 0; i < chainNodes_.size(); ++i)
        if (chainNodes_[i].get() == static_cast<dsp::ProcessorNode*>(plugin))
            index = i;

    const bool hasEditor = plugin->hasEditor();
    bool removeRequested = false;

    {
        const bool  enabled   = plugin->isEnabled();
        const float powerSize = px(36.0f);
        const float chipH     = ImGui::GetFrameHeight() * 0.86f;

        frame.controls.next(powerSize, powerSize);
        if (powerButton("##pluginPower", enabled, powerSize)) {
            plugin->setEnabled(!enabled);
            markDirty();
        }

        // Only a plugin can be taken out. The built-in modules are the signal
        // path itself; switching one off does the same to the sound without
        // losing where it sat.
        frame.controls.next(chipWidth(fonts_.caps, "REMOVE"), chipH);
        if (chipButton("REMOVE", false))
            removeRequested = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Take this plugin out of the chain");

        if (hasEditor) {
            frame.controls.next(chipWidth(fonts_.caps, "POP OUT"), chipH);
            if (chipButton("POP OUT", plugin->isEditorOpen())) {
                plugin->hideEmbeddedEditor();
                plugin->openEditor(mainWindow_);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Open the plugin's own editor in a window of its own");

            const char* views[] = {"EDITOR", "PARAMETERS"};
            int view = pluginShowsParameters_ ? 1 : 0;
            pushFont(fonts_.caps);
            const float segW = spacedTextWidth("EDITOR", fonts_.caps->LegacySize * 0.12f) +
                               spacedTextWidth("PARAMETERS", fonts_.caps->LegacySize * 0.12f) +
                               ImGui::GetFrameHeight() * 0.55f * 4.0f + 8.0f;
            ImGui::PopFont();
            frame.controls.next(segW, ImGui::GetFrameHeight());
            if (segmented("##pluginView", views, 2, &view))
                pluginShowsParameters_ = view == 1;
        }
    }

    const ImVec2 a = frame.bodyMin;
    const ImVec2 b = frame.bodyMax;

    if (!hasEditor || pluginShowsParameters_) {
        // The generic list, for a plugin without an editor of its own or when
        // the user prefers one place for every control.
        inspectedPlugin_ = plugin;
        ImGui::SetCursorScreenPos(a);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("##pluginParams", ImVec2(b.x - a.x, b.y - a.y), ImGuiChildFlags_None);
        renderPluginParameters();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else {
        // A stage for the plugin's own editor, which draws itself in a child
        // window laid over this area.
        draw->AddRectFilled(a, b, IM_COL32(0x0C, 0x0F, 0x13, 0xFF), px(10.0f));
        draw->PushClipRect(a, b, true);
        const float step = px(14.0f);
        for (float x = a.x - (b.y - a.y); x < b.x; x += step)
            draw->AddLine(ImVec2(x, b.y), ImVec2(x + (b.y - a.y), a.y), IM_COL32(0x12, 0x16, 0x1C, 0xFF), px(5.0f));
        draw->PopClipRect();

        // The whole stage is offered to the editor; the caption under it only
        // appears when the editor leaves room for one.
        const float captionH = fonts_.mono->LegacySize + px(14.0f);
        const ImVec2 area0(a.x + px(4.0f), a.y + px(4.0f));
        const ImVec2 area1(b.x - px(4.0f), b.y - px(4.0f));

        // A child window is drawn over everything ImGui draws, popups included,
        // so the editor steps aside while anything could appear above it.
        const bool covered = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ||
                             showPluginBrowser_ || showLog_;

        auto centredMessage = [&](const char* text, const char* action) -> bool {
            pushFont(fonts_.medium);
            const ImVec2 ts = ImGui::CalcTextSize(text);
            const float  cy = (a.y + b.y) * 0.5f;
            draw->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, cy - ts.y - px(10.0f)), theme::kTextDim, text);
            ImGui::PopFont();
            if (!action)
                return false;
            const float w = chipWidth(fonts_.caps, action);
            ImGui::SetCursorScreenPos(ImVec2((a.x + b.x - w) * 0.5f, cy + px(4.0f)));
            return chipButton(action, true);
        };

        int width = 0, height = 0;

        // Creating an editor blocks the thread that draws this window - the
        // plugin builds its whole interface there, and VST3 allows nowhere
        // else. Nothing can make that faster, but the window can say it is
        // coming: a few frames of the spinner go out first, then the editor
        // is created while they are on screen.
        //
        // Nor while a mouse button is held: the editor takes the keyboard focus
        // as it attaches, and that would drop a drag that has only just begun
        // on the unit that opened it.
        const bool opening = !plugin->hasEmbeddedEditor() && !plugin->isEditorOpen() && !covered &&
                             embedFailed_ != plugin &&
                             (editorOpening_ != plugin || openingFrames_ < 3 ||
                              ImGui::IsMouseDown(ImGuiMouseButton_Left));
        if (opening) {
            if (editorOpening_ != plugin) {
                editorOpening_ = plugin;
                openingFrames_ = 0;
            }
            ++openingFrames_;
        }

        if (opening) {
            const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f - px(14.0f));
            const float  r = px(22.0f);
            const float  t = static_cast<float>(ImGui::GetTime());
            draw->PathArcTo(c, r, 0.0f, 6.2832f, 48);
            draw->PathStroke(theme::kBorder, px(4.0f));
            draw->PathArcTo(c, r, t * 6.0f, t * 6.0f + 4.2f, 32);
            draw->PathStroke(theme::withAlpha(theme::kAccent, 0.25f), px(10.0f));
            draw->PathArcTo(c, r, t * 6.0f, t * 6.0f + 4.2f, 32);
            draw->PathStroke(theme::kAccentLight, px(4.0f));

            pushFont(fonts_.medium);
            const char* text = "Opening the editor...";
            const ImVec2 ts = ImGui::CalcTextSize(text);
            draw->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + r + px(16.0f)), theme::kTextDim, text);
            ImGui::PopFont();
        } else if (plugin->isEditorOpen()) {
            if (centredMessage("The editor is open in its own window.", "BRING IT BACK"))
                plugin->closeEditor();
        } else if (covered) {
            plugin->hideEmbeddedEditor();
            centredMessage("The editor steps aside while a menu or window is open.", nullptr);
        } else if (embedFailed_ != plugin &&
                   plugin->showEmbeddedEditor(mainWindow_, static_cast<int>(area0.x), static_cast<int>(area0.y),
                                              static_cast<int>(area1.x - area0.x),
                                              static_cast<int>(area1.y - area0.y), width, height)) {
            embeddedWanted_ = plugin;

            const ImVec2 e0(area0.x + std::floor((area1.x - area0.x - width) * 0.5f),
                            area0.y + std::floor((area1.y - area0.y - height) * 0.5f));
            const ImVec2 e1(e0.x + width, e0.y + height);
            draw->AddRect(ImVec2(e0.x - 2, e0.y - 2), ImVec2(e1.x + 2, e1.y + 2), theme::withAlpha(theme::kAccent, 0.35f),
                          px(4.0f), 1.5f);
            draw->AddRect(ImVec2(e0.x - 6, e0.y - 6), ImVec2(e1.x + 6, e1.y + 6), theme::withAlpha(theme::kAccent, 0.08f),
                          px(8.0f), 6.0f);

            if (b.y - e1.y > captionH + px(6.0f)) {
                char caption[64];
                std::snprintf(caption, sizeof(caption), "plugin's own editor - %d x %d", width, height);
                pushFont(fonts_.mono);
                const ImVec2 cs = ImGui::CalcTextSize(caption);
                draw->AddText(ImVec2((a.x + b.x - cs.x) * 0.5f, b.y - captionH + px(4.0f)), theme::kTextGhost,
                              caption);
                ImGui::PopFont();
            }
        } else if (width > 0) {
            char text[96];
            std::snprintf(text, sizeof(text), "This editor is %d x %d, larger than the room here.", width, height);
            if (centredMessage(text, "OPEN IN ITS OWN WINDOW"))
                plugin->openEditor(mainWindow_);
        } else {
            // Not tried again every frame: creating a view is the slow part.
            embedFailed_ = plugin;
            if (centredMessage("The editor could not be shown here.", "SHOW PARAMETERS"))
                pluginShowsParameters_ = true;
        }
    }

    footerText(fonts_, frame.footerText, plugin->descriptor().path.c_str());

    if (removeRequested && index != SIZE_MAX)
        removeNode(index);
}

// ---------------------------------------------------------------------------
// Footer
// ---------------------------------------------------------------------------

void App::renderFooter()
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  width  = ImGui::GetContentRegionAvail().x;
    const float  height = ImGui::GetContentRegionAvail().y;
    const ImVec2 max(origin.x + width, origin.y + height);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilledMultiColor(origin, max, IM_COL32(0x11, 0x14, 0x1A, 0xFF), IM_COL32(0x11, 0x14, 0x1A, 0xFF),
                                  IM_COL32(0x0B, 0x0D, 0x11, 0xFF), IM_COL32(0x0B, 0x0D, 0x11, 0xFF));
    draw->AddLine(origin, ImVec2(max.x, origin.y), theme::kInset);
    draw->AddLine(ImVec2(origin.x, origin.y + 1), ImVec2(max.x, origin.y + 1), theme::kHighlight);

    const float pad     = px(20.0f);
    const float centreY = origin.y + height * 0.5f;
    const float buttonW = px(116.0f);
    const float buttonH = px(42.0f);
    const float gap     = px(10.0f);
    const float meterRoom = width - pad * 2.0f - buttonW * 2.0f - gap - px(28.0f);
    const float columnW   = (meterRoom - px(28.0f)) * 0.5f;

    auto column = [&](float x, const char* label, float peakLinear, float rmsLinear, std::atomic<float>& trim,
                      const char* trimId) {
        const float peak = toDb(peakLinear);
        const float rms  = toDb(rmsLinear);

        pushFont(fonts_.caps);
        drawSpacedText(draw, ImVec2(x, centreY - px(9.0f) - fonts_.caps->LegacySize * 0.5f), theme::kTextFaint, label, 2.4f);
        ImGui::PopFont();

        const float trimW  = px(92.0f);
        const float readW  = px(62.0f);
        const float labelW = px(40.0f);
        const float meterX = x + labelW;
        const float meterW = columnW - labelW - readW - trimW - px(20.0f);

        ImGui::SetCursorScreenPos(ImVec2(meterX, centreY - px(15.0f)));
        levelMeter(label, peak, rms, ImVec2(meterW, px(12.0f)), true);

        pushFont(fonts_.mono);
        // dB scale under the bar.
        ImGui::PushFont(fonts_.small, fonts_.small->LegacySize * 0.85f);
        for (float db : {-60.0f, -48.0f, -36.0f, -24.0f, -18.0f, -12.0f, -6.0f, 0.0f}) {
            char tick[8];
            std::snprintf(tick, sizeof(tick), "%.0f", static_cast<double>(db));
            const float tw = ImGui::CalcTextSize(tick).x;
            const float tx = meterX + (db + 60.0f) / 60.0f * meterW - tw * 0.5f;
            draw->AddText(ImVec2(std::clamp(tx, meterX, meterX + meterW - tw), centreY + px(1.0f)), theme::kTextGhost, tick);
        }
        ImGui::PopFont();

        char read[16];
        if (peak > -90.0f)
            std::snprintf(read, sizeof(read), "%+.1f", static_cast<double>(peak));
        else
            std::snprintf(read, sizeof(read), "-inf");
        const ImVec2 r0(meterX + meterW + px(10.0f), centreY - px(13.0f));
        const ImVec2 r1(r0.x + readW, centreY + px(13.0f));
        drawInset(draw, r0, r1, px(6.0f));
        const float rw = ImGui::CalcTextSize(read).x;
        draw->AddText(ImVec2(r1.x - rw - px(8.0f), centreY - fonts_.mono->LegacySize * 0.5f),
                      peak > -90.0f ? (peak > -6.0f ? theme::kWarning : theme::kAccentLight) : theme::kTextGhost, read);

        // Trim, as a drag field: one more knob here would crowd the row.
        ImGui::SetCursorScreenPos(ImVec2(r1.x + px(10.0f), centreY - ImGui::GetFrameHeight() * 0.5f));
        ImGui::SetNextItemWidth(trimW);
        float value = trim.load();
        if (ImGui::DragFloat(trimId, &value, 0.1f, -30.0f, 30.0f, "%+.1f dB")) {
            trim.store(value);
            markDirty();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s gain - drag to change, double-click to type",
                              trimId[2] == 'i' ? "Input" : "Output");
        }
        ImGui::PopFont();
    };

    column(origin.x + pad, "IN", meters_.inputPeak.load(), meters_.inputRms.load(), params_.inputGainDb, "##inGain");
    column(origin.x + pad + columnW + px(28.0f), "OUT", meters_.outputPeak.load(), meters_.outputRms.load(),
           params_.outputGainDb, "##outGain");

    const float bx = max.x - pad - buttonW * 2.0f - gap;
    ImGui::SetCursorScreenPos(ImVec2(bx, centreY - buttonH * 0.5f));
    const bool mute = params_.mute.load();
    if (bigButton(fonts_, mute ? "MUTED" : "MUTE", mute, theme::kDanger, ImVec2(buttonW, buttonH))) {
        params_.mute.store(!mute);
        markDirty();
    }

    ImGui::SetCursorScreenPos(ImVec2(bx + buttonW + gap, centreY - buttonH * 0.5f));
    const bool bypass = params_.bypassAll.load();
    if (bigButton(fonts_, bypass ? "BYPASSED" : "BYPASS", bypass, theme::kWarning, ImVec2(buttonW, buttonH))) {
        params_.bypassAll.store(!bypass);
        markDirty();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sends the raw microphone signal straight through, for A/B.");
}

// ---------------------------------------------------------------------------
// Device screen
// ---------------------------------------------------------------------------

void App::renderDevicesScreen()
{
    const float pad = px(18.0f);

    // --- heading ------------------------------------------------------------
    if (chipButton("< BACK TO CONSOLE", false))
        showDevices_ = false;

    ImGui::Dummy(ImVec2(0.0f, px(10.0f)));
    ImGui::PushFont(fonts_.heading, fonts_.heading->LegacySize * 1.35f);
    ImGui::TextUnformatted("Audio devices");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextDim));
    ImGui::TextUnformatted("Where the voice comes from and where the processed signal goes. Set once, then forget.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, px(12.0f)));

    // Cards are children with their own padding, drawn on a plate.
    auto beginCard = [&](const char* id, float width) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::toVec4(theme::kPanel));
        ImGui::PushStyleColor(ImGuiCol_Border, theme::toVec4(theme::kBorder));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, px(14.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(10.0f), px(8.0f)));
        ImGui::BeginChild(id, ImVec2(width, 0.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    };
    auto endCard = [&]() {
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
    };
    auto cardTitle = [&](const char* title) {
        pushFont(fonts_.medium);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        ImGui::Spacing();
    };

    const float total   = ImGui::GetContentRegionAvail().x;
    const float gap     = px(18.0f);
    const bool  twoWide = total > px(760.0f);
    const float half    = twoWide ? (total - gap) * 0.5f : total;

    // --- microphone ---------------------------------------------------------
    beginCard("##micCard", half);
    cardTitle("Microphone");
    renderDeviceSelector("", config_.input, true);
    {
        // Sits with the input device because it is a property of how that
        // device is read: an interface with one microphone on the first of two
        // channels wants that channel alone, not a sum at half the level.
        const InputMix modes[] = {InputMix::Stereo, InputMix::MonoLeft, InputMix::MonoRight, InputMix::MonoSum};
        auto current = static_cast<InputMix>(params_.inputMix.load());
        capsLabel("CHANNELS");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##inputmix", toString(current))) {
            for (InputMix mode : modes) {
                if (ImGui::Selectable(toString(mode), mode == current)) {
                    params_.inputMix.store(static_cast<int>(mode));
                    markDirty();
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("How the capture device's channels are folded into the chain.\n"
                              "Pick a single channel when one microphone is plugged into a\n"
                              "stereo input; summing both would halve its level.");
        }
    }
    endCard();

    if (twoWide)
        ImGui::SameLine(0.0f, gap);
    else
        ImGui::Dummy(ImVec2(0.0f, gap));

    // --- output -------------------------------------------------------------
    beginCard("##outCard", half);
    cardTitle("Output");
    renderDeviceSelector("", config_.output, false);
    {
        bool mono = params_.monoOutput.load();
        ImGui::TextUnformatted("Mono output");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - switchWidth());
        if (toggleSwitch("monoOut", &mono)) {
            params_.monoOutput.store(mono);
            markDirty();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Sums the processed signal to mono before it reaches the\n"
                              "output device, so both channels carry the same audio.");
        }
    }
    ImGui::Spacing();
    renderVirtualCableHint();
    endCard();

    ImGui::Dummy(ImVec2(0.0f, gap));

    const bool  threeWide = total > px(980.0f);
    const float third     = threeWide ? (total - gap * 2.0f) / 3.0f : total;

    // --- monitor ------------------------------------------------------------
    // A virtual cable cannot be listened to - that is what makes it a cable.
    // This is the second render device that carries the same signal.
    beginCard("##monitorCard", third);
    cardTitle("Monitor");
    {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
        ImGui::TextWrapped("Where the Monitor button in the header sends the processed voice.");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        bool enabled = config_.monitorEnabled;
        ImGui::TextUnformatted("Monitor on");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - switchWidth());
        if (toggleSwitch("monitorOn", &enabled)) {
            config_.monitorEnabled = enabled;
            if (enabled)
                ensureMonitorDevice();
            markDirty();
        }

        ImGui::Spacing();
        renderDeviceSelector("", config_.monitor, false, /*allowAsio=*/false);

        capsLabel("HEADPHONE LEVEL");
        float gainDb = params_.monitorGainDb.load();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##monitorgain", &gainDb, -60.0f, 12.0f, "%+.1f dB")) {
            params_.monitorGainDb.store(gainDb);
            markDirty();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Affects only what you hear - the signal being sent onwards\n"
                              "is untouched.");
        }

        bool monitorMuted = params_.monitorMute.load();
        ImGui::TextUnformatted("Mute monitor");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - switchWidth());
        if (toggleSwitch("monitorMute", &monitorMuted)) {
            params_.monitorMute.store(monitorMuted);
            markDirty();
        }

        const auto status = engine_->status();
        if (!status.monitorError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kWarning));
            ImGui::TextWrapped("monitor: %s", status.monitorError.c_str());
            ImGui::PopStyleColor();
        }
    }
    endCard();

    if (threeWide)
        ImGui::SameLine(0.0f, gap);
    else
        ImGui::Dummy(ImVec2(0.0f, gap));

    // --- processing block ---------------------------------------------------
    beginCard("##blockCard", third);
    cardTitle("Processing block");
    {
        const char* labels[] = {"64", "128", "256", "512", "1024"};
        const int   sizes[]  = {64, 128, 256, 512, 1024};
        int current = 2;
        for (int i = 0; i < 5; ++i)
            if (sizes[i] == config_.maxBlockFrames)
                current = i;
        if (segmented("##block", labels, 5, &current)) {
            config_.maxBlockFrames = sizes[current];
            markDirty();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Largest block handed to plugins.\n"
                              "Smaller reacts faster; larger is easier on the CPU.");
        }

        const double rate = currentSampleRate();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
        ImGui::TextWrapped("%.2f ms per block at %.0f kHz. Smaller is snappier; raise it if you hear dropouts.",
                           1000.0 * config_.maxBlockFrames / rate, rate / 1000.0);
        ImGui::PopStyleColor();
    }
    endCard();

    if (threeWide)
        ImGui::SameLine(0.0f, gap);
    else
        ImGui::Dummy(ImVec2(0.0f, gap));

    // --- live stream --------------------------------------------------------
    beginCard("##streamCard", third);
    cardTitle("Stream details");
    {
        const auto status = engine_->status();
        if (status.running) {
            char text[96];

            std::snprintf(text, sizeof(text), "%.0f Hz / %d ch", status.inputSampleRate, status.inputChannels);
            infoRow("capture", text, theme::kTextDim);
            infoRow("format", status.inputFormat.c_str(), theme::kTextDim);
            std::snprintf(text, sizeof(text), "%d frames", status.inputBufferFrames);
            infoRow("buffer", text, theme::kTextDim);

            ImGui::Spacing();

            std::snprintf(text, sizeof(text), "%.0f Hz / %d ch", status.outputSampleRate, status.outputChannels);
            infoRow("render", text, theme::kTextDim);
            infoRow("format", status.outputFormat.c_str(), theme::kTextDim);
            std::snprintf(text, sizeof(text), "%d frames", status.outputBufferFrames);
            infoRow("buffer", text, theme::kTextDim);
        } else {
            ImGui::TextDisabled("audio stopped");
        }
    }
    endCard();

    ImGui::Dummy(ImVec2(0.0f, pad));
}

} // namespace rv::app
