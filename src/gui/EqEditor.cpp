#include "gui/EqEditor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/Strings.h"
#include "dsp/GraphicEq.h"
#include "gui/Theme.h"

namespace rv::gui {
namespace {

constexpr float kMinHz = 20.0f;
constexpr float kMaxHz = 20000.0f;

/// Curve range. +-18 dB covers what a corrective EQ on a voice ever needs and
/// keeps the useful region large on screen.
constexpr float kMaxCurveDb = 18.0f;

/// Spectrum range, independent of the curve scale.
constexpr float kSpectrumTopDb    = -6.0f;
constexpr float kSpectrumBottomDb = -84.0f;

constexpr int kCurveResolution = 320;

float hzToX(float hz, float left, float width)
{
    const float t = std::log(std::clamp(hz, kMinHz, kMaxHz) / kMinHz) /
                    std::log(kMaxHz / kMinHz);
    return left + t * width;
}

float xToHz(float x, float left, float width)
{
    const float t = std::clamp((x - left) / std::max(1.0f, width), 0.0f, 1.0f);
    return kMinHz * std::pow(kMaxHz / kMinHz, t);
}

float dbToY(float db, float top, float height)
{
    const float t = std::clamp((db + kMaxCurveDb) / (2.0f * kMaxCurveDb), 0.0f, 1.0f);
    return top + (1.0f - t) * height;
}

float yToDb(float y, float top, float height)
{
    const float t = std::clamp((y - top) / std::max(1.0f, height), 0.0f, 1.0f);
    return (1.0f - t) * 2.0f * kMaxCurveDb - kMaxCurveDb;
}

} // namespace

bool eqEditor(const char* id, Params& params, dsp::SpectrumAnalyzer& spectrum,
              float sampleRate, ImVec2 size)
{
    ImGui::PushID(id);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##canvas", size);
    const bool canvasHovered = ImGui::IsItemHovered();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(origin.x + size.x, origin.y + size.y);

    draw->AddRectFilled(origin, max, theme::kBackground, 6.0f);
    draw->PushClipRect(origin, max, true);

    bool changed = false;

    // ---- grid ------------------------------------------------------------
    static const float kGridFrequencies[] = {
        30, 50, 100, 200, 300, 500, 1000, 2000, 3000, 5000, 10000, 20000};
    static const bool kLabelled[] = {
        false, true, true, false, false, true, true, false, false, true, true, true};

    for (size_t i = 0; i < std::size(kGridFrequencies); ++i) {
        const float x = hzToX(kGridFrequencies[i], origin.x, size.x);
        draw->AddLine(ImVec2(x, origin.y), ImVec2(x, max.y),
                      kLabelled[i] ? theme::kGridStrong : theme::kGrid, 1.0f);

        if (kLabelled[i]) {
            const std::string label = formatHz(kGridFrequencies[i]);
            draw->AddText(ImVec2(x + 4, max.y - ImGui::GetTextLineHeight() - 3),
                          theme::kTextFaint, label.c_str());
        }
    }

    for (float db = -12.0f; db <= 12.0f; db += 6.0f) {
        const float y = dbToY(db, origin.y, size.y);
        draw->AddLine(ImVec2(origin.x, y), ImVec2(max.x, y),
                      db == 0.0f ? theme::kGridStrong : theme::kGrid, 1.0f);

        char label[16];
        std::snprintf(label, sizeof(label), "%+.0f", static_cast<double>(db));
        draw->AddText(ImVec2(origin.x + 4, y - ImGui::GetTextLineHeight() * 0.5f),
                      theme::kTextFaint, label);
    }

    // ---- spectrum --------------------------------------------------------
    spectrum.update();

    const int binCount = spectrum.binCount();
    if (binCount > 2) {
        // One point per pixel column rather than per FFT bin: at 2048 points
        // the low end has far fewer bins per pixel than the top, and drawing
        // per bin would leave the bass looking sparse and the treble dense.
        std::vector<ImVec2> points;
        points.reserve(static_cast<size_t>(size.x) + 2);

        for (float x = origin.x; x <= max.x; x += 1.0f) {
            const float hz  = xToHz(x, origin.x, size.x);
            const float bin = hz * spectrum.kFftSize / sampleRate;

            const int index = std::clamp(static_cast<int>(bin), 0, binCount - 1);
            // Take the loudest bin in the span this pixel covers, so a narrow
            // peak is not lost between samples.
            const int nextHz = std::clamp(
                static_cast<int>(xToHz(x + 1.0f, origin.x, size.x) *
                                 spectrum.kFftSize / sampleRate),
                index, binCount - 1);

            float db = spectrum.binDb(index);
            for (int b = index + 1; b <= nextHz; ++b)
                db = std::max(db, spectrum.binDb(b));

            const float t = std::clamp((db - kSpectrumBottomDb) /
                                       (kSpectrumTopDb - kSpectrumBottomDb), 0.0f, 1.0f);
            points.push_back(ImVec2(x, max.y - t * size.y));
        }

        if (points.size() > 2) {
            // Filled one column at a time. A spectrum outline is not convex,
            // and AddConvexPolyFilled would tear it into overlapping triangles.
            for (const ImVec2& point : points) {
                draw->AddRectFilled(ImVec2(point.x, point.y),
                                    ImVec2(point.x + 1.0f, max.y), theme::kSpectrum);
            }
            draw->AddPolyline(points.data(), static_cast<int>(points.size()),
                              theme::kSpectrumLine, 1.4f, ImDrawFlags_None);
        }
    }

    // ---- response curve --------------------------------------------------
    {
        ImVec2 curve[kCurveResolution];
        for (int i = 0; i < kCurveResolution; ++i) {
            const float x  = origin.x + size.x * i / (kCurveResolution - 1);
            const float hz = xToHz(x, origin.x, size.x);
            const float db = dsp::GraphicEq::responseDb(params, hz, sampleRate);
            curve[i] = ImVec2(x, dbToY(db, origin.y, size.y));
        }
        draw->AddPolyline(curve, kCurveResolution, theme::kCurve, 2.2f, ImDrawFlags_None);
    }

    // ---- band handles ----------------------------------------------------
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    static int draggedBand = -1;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        draggedBand = -1;

    for (int band = 0; band < kEqBands; ++band) {
        const float hz   = kEqCenters[band];
        const float gain = params.eqGainDb[band].load(std::memory_order_relaxed);

        const ImVec2 centre(hzToX(hz, origin.x, size.x),
                            dbToY(gain, origin.y, size.y));

        const float distance = std::hypot(mouse.x - centre.x, mouse.y - centre.y);
        const bool  hovered  = canvasHovered && distance < 14.0f;

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            draggedBand = band;

        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            params.eqGainDb[band].store(0.0f, std::memory_order_relaxed);
            changed = true;
        }

        if (draggedBand == band) {
            const float newGain =
                std::clamp(yToDb(mouse.y, origin.y, size.y), -kMaxCurveDb, kMaxCurveDb);
            params.eqGainDb[band].store(newGain, std::memory_order_relaxed);
            changed = true;
        }

        if (hovered) {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                const float q = std::clamp(
                    params.eqQ[band].load(std::memory_order_relaxed) * (1.0f + wheel * 0.12f),
                    0.2f, 8.0f);
                params.eqQ[band].store(q, std::memory_order_relaxed);
                changed = true;
            }

            char tooltip[128];
            std::snprintf(tooltip, sizeof(tooltip), "%s\n%+.1f dB   Q %.2f",
                          formatHz(hz).c_str(),
                          static_cast<double>(params.eqGainDb[band].load()),
                          static_cast<double>(params.eqQ[band].load()));
            ImGui::SetTooltip("%s", tooltip);
        }

        const bool  engaged = std::abs(gain) > 0.05f;
        const ImU32 fill    = (draggedBand == band || hovered) ? theme::kCurve
                            : engaged                          ? theme::kAccent
                                                               : theme::kTextFaint;

        draw->AddCircleFilled(centre, hovered ? 7.5f : 6.0f, fill, 20);
        draw->AddCircle(centre, hovered ? 7.5f : 6.0f, theme::kBackground, 20, 1.5f);
    }

    // ---- high-pass / low-pass markers ------------------------------------
    static int draggedCut = -1; // 0 = high-pass, 1 = low-pass
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        draggedCut = -1;

    struct Cut {
        std::atomic<bool>*  enabled;
        std::atomic<float>* frequency;
        const char*         label;
        int                 index;
    };
    const Cut cuts[] = {
        {&params.hpfEnabled, &params.hpfHz, "HP", 0},
        {&params.lpfEnabled, &params.lpfHz, "LP", 1},
    };

    for (const Cut& cut : cuts) {
        if (!cut.enabled->load(std::memory_order_relaxed))
            continue;

        const float hz = cut.frequency->load(std::memory_order_relaxed);
        const float x  = hzToX(hz, origin.x, size.x);

        const ImVec2 handle(x, origin.y + 12.0f);
        const bool hovered = canvasHovered &&
                             std::hypot(mouse.x - handle.x, mouse.y - handle.y) < 12.0f;

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            draggedCut = cut.index;

        if (draggedCut == cut.index) {
            cut.frequency->store(std::clamp(xToHz(mouse.x, origin.x, size.x), 20.0f, 20000.0f),
                                 std::memory_order_relaxed);
            changed = true;
        }

        const ImU32 colour = (hovered || draggedCut == cut.index) ? theme::kCurve
                                                                  : theme::kAccentDim;
        draw->AddLine(ImVec2(x, origin.y), ImVec2(x, max.y), colour, 1.6f);
        draw->AddCircleFilled(handle, 6.0f, colour, 16);
        draw->AddText(ImVec2(x + 9, origin.y + 4), colour, cut.label);

        if (hovered)
            ImGui::SetTooltip("%s at %s", cut.label, formatHz(hz).c_str());
    }

    draw->PopClipRect();
    draw->AddRect(origin, max, theme::kBorder, 6.0f);

    if (changed)
        params.touch();

    ImGui::PopID();
    return changed;
}

} // namespace rv::gui
