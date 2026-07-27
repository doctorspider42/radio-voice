#include "dsp/SpectrumAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "dsp/Smoothing.h"

namespace rv::dsp {
namespace {

/// Rise is nearly instantaneous so transients register; decay is slow enough
/// to read. These are per-update factors, not time constants.
constexpr float kRiseFactor = 0.55f;
constexpr float kFallFactor = 0.88f;

constexpr float kFloorDb = -100.0f;

} // namespace

void SpectrumAnalyzer::prepare(double sampleRate)
{
    sampleRate_ = sampleRate;

    fft_.prepare(kFftSize);

    // Four hops of slack: enough that a UI stall of a few frames does not lose
    // data, small enough that the display never lags perceptibly.
    feed_.resize(1, kHopSize * 8);

    history_.assign(kFftSize, 0.0f);
    hopBuffer_.assign(kHopSize, 0.0f);
    magnitudes_.assign(kFftSize / 2 + 1, 0.0f);
    displayDb_.assign(kFftSize / 2 + 1, kFloorDb);
    monoScratch_.assign(kMaxBlockSize, 0.0f);
}

void SpectrumAnalyzer::clear()
{
    feed_.clear();
    std::fill(history_.begin(), history_.end(), 0.0f);
    std::fill(displayDb_.begin(), displayDb_.end(), kFloorDb);
}

void SpectrumAnalyzer::push(const PlanarBuffer& buffer)
{
    const int frames   = std::min(buffer.frames(), static_cast<int>(monoScratch_.size()));
    const int channels = buffer.channels();
    if (frames <= 0 || channels <= 0)
        return;

    const float scale = 1.0f / static_cast<float>(channels);
    for (int i = 0; i < frames; ++i) {
        float sum = 0.0f;
        for (int c = 0; c < channels; ++c)
            sum += buffer.channel(c)[i];
        monoScratch_[static_cast<size_t>(i)] = sum * scale;
    }

    // Partial writes are fine: the display resynchronises on the next hop.
    feed_.write(monoScratch_.data(), frames);
}

bool SpectrumAnalyzer::update()
{
    bool recomputed = false;

    // If the UI stalled and the ring is nearly full, skip forward rather than
    // rendering a backlog of stale spectra.
    if (feed_.filled() > kHopSize * 6)
        feed_.trimTo(kHopSize * 2);

    while (feed_.filled() >= kHopSize) {
        feed_.read(hopBuffer_.data(), kHopSize);

        // Slide the analysis window by one hop.
        std::memmove(history_.data(), history_.data() + kHopSize,
                     static_cast<size_t>(kFftSize - kHopSize) * sizeof(float));
        std::memcpy(history_.data() + (kFftSize - kHopSize), hopBuffer_.data(),
                    static_cast<size_t>(kHopSize) * sizeof(float));

        recomputed = true;
    }

    if (!recomputed)
        return false;

    fft_.magnitude(history_.data(), magnitudes_.data());

    const size_t bins = displayDb_.size();
    for (size_t i = 0; i < bins; ++i) {
        const float db = std::max(kFloorDb, gainToDb(magnitudes_[i]));
        float&      d  = displayDb_[i];
        d = (db > d) ? d + (db - d) * kRiseFactor
                     : d + (db - d) * (1.0f - kFallFactor);
    }

    return true;
}

} // namespace rv::dsp
