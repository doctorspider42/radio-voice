#include "gui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include <imgui_internal.h>

#include "gui/Theme.h"

namespace rv::gui {
namespace {

constexpr float kPi = 3.14159265358979323846f;

/// Knobs sweep 270 degrees, leaving a gap at the bottom so the extremes are
/// visually distinct rather than meeting behind the control.
constexpr float kArcStart = 0.75f * kPi;
constexpr float kArcEnd   = 2.25f * kPi;

/// Meter range. -60 dB is below any usable signal and +6 leaves headroom
/// visible above full scale.
constexpr float kMeterMinDb = -60.0f;
constexpr float kMeterMaxDb = 6.0f;

float meterPosition(float db)
{
    return std::clamp((db - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb), 0.0f, 1.0f);
}

ImU32 levelColour(float db)
{
    if (db >= -0.5f)
        return theme::kDanger;
    if (db >= -6.0f)
        return theme::kWarning;
    return theme::kSignal;
}

bool knobImpl(const char* label, float* value, float minimum, float maximum,
              const char* format, float defaultValue, float diameter, bool logarithmic)
{
    ImGui::PushID(label);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float labelHeight = ImGui::GetTextLineHeight();
    const ImVec2 origin     = ImGui::GetCursorScreenPos();

    // The item is as wide as its widest part, not just the dial: a label like
    // "sidechain HP" is far wider than a 46 pixel knob, and sizing to the dial
    // alone makes adjacent knobs' labels overlap.
    char initialText[32];
    std::snprintf(initialText, sizeof(initialText), format, static_cast<double>(*value));
    const float itemWidth = std::max({diameter,
                                      ImGui::CalcTextSize(label).x,
                                      ImGui::CalcTextSize(initialText).x});

    // Wrap onto the next line when this knob will not fit on the current one.
    //
    // Panels are laid out with explicit SameLine calls, which is fine until the
    // window is narrower than the author assumed - then the last knobs on a row
    // are silently clipped, and a control the user cannot see is worse than one
    // that moved. Deciding here rather than at every call site means every knob
    // row is responsive without the panels having to think about it.
    if (ImGui::GetCursorPosX() > ImGui::GetCursorStartPos().x + 1.0f &&
        ImGui::GetContentRegionAvail().x < itemWidth) {
        ImGui::NewLine();
    }

    // Label above, knob, value below - all inside one invisible item so the
    // whole stack participates in layout and hit testing as a unit.
    const ImVec2 totalSize(itemWidth, labelHeight + 4 + diameter + 2 + labelHeight);

    ImGui::InvisibleButton("##knob", totalSize);
    const bool hovered = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive();

    bool changed = false;

    auto toNormalized = [&](float v) {
        if (logarithmic) {
            const float lo = std::log(std::max(1.0e-6f, minimum));
            const float hi = std::log(std::max(1.0e-6f, maximum));
            return (std::log(std::max(1.0e-6f, v)) - lo) / (hi - lo);
        }
        return (v - minimum) / (maximum - minimum);
    };
    auto fromNormalized = [&](float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        if (logarithmic) {
            const float lo = std::log(std::max(1.0e-6f, minimum));
            const float hi = std::log(std::max(1.0e-6f, maximum));
            return std::exp(lo + t * (hi - lo));
        }
        return minimum + t * (maximum - minimum);
    };

    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImGuiIO& io = ImGui::GetIO();
        // 200 pixels of travel covers the full range; Shift stretches that to
        // 2000 for setting a value precisely.
        const float travel = io.KeyShift ? 2000.0f : 200.0f;
        const float delta  = -io.MouseDelta.y / travel;

        *value  = fromNormalized(toNormalized(*value) + delta);
        *value  = std::clamp(*value, minimum, maximum);
        changed = true;
        ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
    }

    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        *value  = std::clamp(defaultValue, minimum, maximum);
        changed = true;
    }

    // --- draw -------------------------------------------------------------
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const ImVec2 centre(origin.x + itemWidth * 0.5f,
                        origin.y + labelHeight + 4 + diameter * 0.5f);
    const float radius = diameter * 0.5f - 3.0f;
    const float t      = std::clamp(toNormalized(*value), 0.0f, 1.0f);
    const float angle  = kArcStart + t * (kArcEnd - kArcStart);

    draw->PathArcTo(centre, radius, kArcStart, kArcEnd, 48);
    draw->PathStroke(theme::kPanelRaised, 4.0f, ImDrawFlags_None);

    const ImU32 arcColour = active   ? theme::kAccent
                          : hovered  ? theme::kAccent
                                     : theme::kAccentDim;
    draw->PathArcTo(centre, radius, kArcStart, angle, 48);
    draw->PathStroke(arcColour, 4.0f, ImDrawFlags_None);

    draw->AddCircleFilled(centre, radius - 4.0f, theme::kPanelRaised, 32);
    draw->AddCircle(centre, radius - 4.0f, theme::kBorderBright, 32, 1.0f);

    const ImVec2 pointer(centre.x + std::cos(angle) * (radius - 6.0f),
                         centre.y + std::sin(angle) * (radius - 6.0f));
    draw->AddLine(ImVec2(centre.x + std::cos(angle) * (radius - 13.0f),
                         centre.y + std::sin(angle) * (radius - 13.0f)),
                  pointer, theme::kText, 2.0f);

    // Label, centred.
    const ImVec2 labelSize = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(origin.x + (itemWidth - labelSize.x) * 0.5f, origin.y),
                  theme::kTextDim, label);

    // Re-formatted after the drag so the readout is never a frame stale.
    char valueText[32];
    std::snprintf(valueText, sizeof(valueText), format, static_cast<double>(*value));
    const ImVec2 valueSize = ImGui::CalcTextSize(valueText);
    draw->AddText(ImVec2(origin.x + (itemWidth - valueSize.x) * 0.5f,
                         origin.y + labelHeight + 4 + diameter + 2),
                  hovered || active ? theme::kText : theme::kTextDim, valueText);

    ImGui::PopID();
    (void)style;
    return changed;
}

} // namespace

bool knob(const char* label, float* value, float minimum, float maximum,
          const char* format, float defaultValue, float diameter)
{
    return knobImpl(label, value, minimum, maximum, format, defaultValue, diameter, false);
}

bool knobLog(const char* label, float* value, float minimum, float maximum,
             const char* format, float defaultValue, float diameter)
{
    return knobImpl(label, value, minimum, maximum, format, defaultValue, diameter, true);
}

void levelMeter(const char* id, float peakDb, float rmsDb, ImVec2 size, bool horizontal)
{
    ImGui::PushID(id);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##meter", size);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(origin.x + size.x, origin.y + size.y);

    draw->AddRectFilled(origin, max, theme::kBackground, 3.0f);

    // Scale marks every 6 dB; they are what make an absolute reading possible.
    for (float db = -60.0f; db <= 6.0f; db += 6.0f) {
        const float t = meterPosition(db);
        const ImU32 colour = (db == 0.0f) ? theme::kBorderBright : theme::kGrid;
        if (horizontal) {
            const float x = origin.x + t * size.x;
            draw->AddLine(ImVec2(x, origin.y), ImVec2(x, max.y), colour, 1.0f);
        } else {
            const float y = max.y - t * size.y;
            draw->AddLine(ImVec2(origin.x, y), ImVec2(max.x, y), colour, 1.0f);
        }
    }

    const float rmsT  = meterPosition(rmsDb);
    const float peakT = meterPosition(peakDb);

    if (rmsT > 0.0f) {
        const ImU32 colour = levelColour(rmsDb);
        if (horizontal) {
            draw->AddRectFilled(ImVec2(origin.x + 1, origin.y + 1),
                                ImVec2(origin.x + rmsT * size.x, max.y - 1), colour, 2.0f);
        } else {
            draw->AddRectFilled(ImVec2(origin.x + 1, max.y - rmsT * size.y),
                                ImVec2(max.x - 1, max.y - 1), colour, 2.0f);
        }
    }

    if (peakT > 0.0f) {
        const ImU32 colour = levelColour(peakDb);
        if (horizontal) {
            const float x = origin.x + peakT * size.x;
            draw->AddLine(ImVec2(x, origin.y + 1), ImVec2(x, max.y - 1), colour, 2.0f);
        } else {
            const float y = max.y - peakT * size.y;
            draw->AddLine(ImVec2(origin.x + 1, y), ImVec2(max.x - 1, y), colour, 2.0f);
        }
    }

    draw->AddRect(origin, max, theme::kBorder, 3.0f);

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("peak %.1f dB\nRMS  %.1f dB",
                          static_cast<double>(peakDb), static_cast<double>(rmsDb));
    }

    ImGui::PopID();
}

void gainReductionMeter(const char* id, float reductionDb, float rangeDb, ImVec2 size)
{
    ImGui::PushID(id);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##gr", size);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(origin.x + size.x, origin.y + size.y);

    draw->AddRectFilled(origin, max, theme::kBackground, 3.0f);

    // Reduction is negative and grows downward from the top of the meter,
    // which is how every compressor displays it.
    const float t = std::clamp(-reductionDb / std::max(1.0f, -rangeDb), 0.0f, 1.0f);
    if (t > 0.001f) {
        draw->AddRectFilled(ImVec2(origin.x + 1, origin.y + 1),
                            ImVec2(max.x - 1, origin.y + t * size.y),
                            theme::kReduction, 2.0f);
    }

    draw->AddRect(origin, max, theme::kBorder, 3.0f);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("gain reduction %.1f dB", static_cast<double>(reductionDb));

    ImGui::PopID();
}

bool sectionHeader(const char* title, bool* enabled)
{
    bool toggled = false;

    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextDim));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();

    if (enabled) {
        ImGui::SameLine();
        const float switchWidth = 34.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             ImGui::GetContentRegionAvail().x - switchWidth);
        toggled = toggleSwitch(title, enabled);
    }

    ImGui::Spacing();
    return toggled;
}

bool toggleSwitch(const char* id, bool* value)
{
    ImGui::PushID(id);

    const float height = ImGui::GetFrameHeight() * 0.75f;
    const float width  = height * 1.9f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("##toggle", ImVec2(width, height));
    bool changed = false;
    if (ImGui::IsItemClicked()) {
        *value  = !*value;
        changed = true;
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float radius = height * 0.5f;

    const ImU32 track = *value ? theme::kAccentDim : theme::kPanelRaised;
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), track, radius);
    draw->AddRect(origin, ImVec2(origin.x + width, origin.y + height),
                  ImGui::IsItemHovered() ? theme::kBorderBright : theme::kBorder, radius);

    const float knobX = *value ? origin.x + width - radius : origin.x + radius;
    draw->AddCircleFilled(ImVec2(knobX, origin.y + radius), radius - 2.5f,
                          *value ? theme::kAccent : theme::kTextFaint, 20);

    ImGui::PopID();
    return changed;
}

void statusPill(const char* text, ImU32 colour)
{
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const ImVec2 padding(10.0f, 3.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(textSize.x + padding.x * 2, textSize.y + padding.y * 2);

    ImGui::InvisibleButton(text, size);

    ImDrawList* draw = ImGui::GetWindowDrawList();

    // The badge body is the status colour at low alpha with a full-strength
    // outline: readable without shouting louder than the meters.
    ImVec4 fill = ImGui::ColorConvertU32ToFloat4(colour);
    fill.w = 0.18f;

    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                        ImGui::ColorConvertFloat4ToU32(fill), size.y * 0.5f);
    draw->AddRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), colour,
                  size.y * 0.5f);
    draw->AddText(ImVec2(origin.x + padding.x, origin.y + padding.y), colour, text);
}

bool gearButton(const char* id, float size, ImU32 tint, bool lit)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 origin     = ImGui::GetCursorScreenPos();

    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    const bool held    = ImGui::IsItemActive() || lit;

    // The frame is drawn by hand rather than by ImGui::Button so that the cog
    // can sit on top of it, but it takes its colours from the style: the
    // neighbouring Start and Restart buttons must not look like a different
    // control.
    ImDrawList* draw  = ImGui::GetWindowDrawList();
    const ImU32 frame = ImGui::GetColorU32(held      ? ImGuiCol_ButtonActive
                                           : hovered ? ImGuiCol_ButtonHovered
                                                     : ImGuiCol_Button);
    const ImVec2 corner(origin.x + size, origin.y + size);
    draw->AddRectFilled(origin, corner, frame, style.FrameRounding);
    if (style.FrameBorderSize > 0.0f) {
        draw->AddRect(origin, corner, ImGui::GetColorU32(ImGuiCol_Border),
                      style.FrameRounding, style.FrameBorderSize);
    }

    const ImVec2 centre(origin.x + size * 0.5f, origin.y + size * 0.5f);
    const float body  = size * 0.24f;
    const float tooth = body * 1.45f;
    const float hole  = body * 0.42f;

    // Eight teeth: fewer reads as a flower at this size, more turns into a
    // circle. Each is a quad, so it stays convex and fills cleanly.
    constexpr int kTeeth = 8;
    for (int i = 0; i < kTeeth; ++i) {
        const float angle = (2.0f * kPi * static_cast<float>(i)) / kTeeth;
        const float base  = 0.20f; ///< Half-width at the root, in radians.
        const float tip   = 0.13f; ///< Narrower at the tip, so it reads as a cog.

        auto point = [&](float offset, float radius) {
            return ImVec2(centre.x + std::cos(angle + offset) * radius,
                          centre.y + std::sin(angle + offset) * radius);
        };

        draw->AddQuadFilled(point(-base, body * 0.9f), point(base, body * 0.9f),
                            point(tip, tooth), point(-tip, tooth), tint);
    }

    draw->AddCircleFilled(centre, body, tint, 24);
    draw->AddCircleFilled(centre, hole, frame, 16);

    return pressed;
}

void rightLabel(const char* text, ImU32 colour)
{
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(colour));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void infoRow(const char* key, const char* value, ImU32 valueColour)
{
    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
    ImGui::TextUnformatted(key);
    ImGui::PopStyleColor();
    rightLabel(value, valueColour);
}

} // namespace rv::gui
