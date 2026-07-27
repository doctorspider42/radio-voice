#pragma once

#include <atomic>

#include "core/Types.h"

namespace rv {

/// Live parameter block, written by the UI thread and read by the audio thread.
///
/// Every field is an independent relaxed atomic. There is deliberately no lock
/// and no transactional update: audio parameters are continuous quantities, so
/// observing one knob a block earlier than another is inaudible, whereas
/// blocking the audio thread on a mutex is not. Values that must not jump are
/// ramped by the DSP stage that consumes them.
///
/// `revision` is bumped whenever something that requires filter coefficients to
/// be recalculated changes, so the audio thread can skip the trigonometry on
/// the overwhelming majority of blocks where nothing moved.
struct Params {
    // -- routing / levels --------------------------------------------------
    std::atomic<float> inputGainDb{0.0f};
    std::atomic<float> outputGainDb{0.0f};
    std::atomic<bool>  mute{false};
    std::atomic<bool>  bypassAll{false};   ///< Master bypass: raw input to output.

    // -- high/low cut ------------------------------------------------------
    std::atomic<bool>  hpfEnabled{true};
    std::atomic<float> hpfHz{80.0f};       ///< 24 dB/oct Butterworth.
    std::atomic<bool>  lpfEnabled{false};
    std::atomic<float> lpfHz{16000.0f};

    // -- graphic EQ --------------------------------------------------------
    // Band 0 is a low shelf and band 9 a high shelf; the eight in between are
    // peaking. Shelves at the extremes behave far better than peaking filters
    // when a user pulls the end sliders to their limits.
    std::atomic<bool>  eqEnabled{true};
    std::atomic<float> eqGainDb[kEqBands];
    std::atomic<float> eqQ[kEqBands];

    // -- noise gate --------------------------------------------------------
    std::atomic<bool>  gateEnabled{true};
    std::atomic<float> gateThresholdDb{-45.0f};
    std::atomic<float> gateHysteresisDb{6.0f};  ///< Close threshold sits this far below open.
    std::atomic<float> gateRangeDb{-60.0f};     ///< Attenuation when fully closed.
    std::atomic<float> gateAttackMs{2.0f};
    std::atomic<float> gateHoldMs{120.0f};
    std::atomic<float> gateReleaseMs{180.0f};
    std::atomic<float> gateLookaheadMs{3.0f};
    std::atomic<float> gateSidechainHpfHz{120.0f}; ///< Keeps rumble from opening the gate.

    // -- compressor --------------------------------------------------------
    std::atomic<bool>  compEnabled{true};
    std::atomic<float> compThresholdDb{-18.0f};
    std::atomic<float> compRatio{3.0f};
    std::atomic<float> compKneeDb{6.0f};        ///< Width of the soft knee, centred on the threshold.
    std::atomic<float> compAttackMs{8.0f};
    std::atomic<float> compReleaseMs{120.0f};
    std::atomic<float> compMakeupDb{0.0f};
    std::atomic<bool>  compAutoMakeup{true};
    std::atomic<float> compLookaheadMs{2.0f};
    std::atomic<float> compSidechainHpfHz{100.0f};
    std::atomic<bool>  compRmsDetection{true};  ///< RMS follows loudness; peak follows transients.

    // -- input routing -----------------------------------------------------
    /// Stored as an int so it can live in an atomic; see InputMix.
    std::atomic<int>  inputMix{static_cast<int>(InputMix::Stereo)};
    /// Sums the processed signal to mono before it reaches the output device.
    std::atomic<bool> monoOutput{false};

    // -- output limiter ----------------------------------------------------
    std::atomic<bool>  limiterEnabled{true};
    std::atomic<float> limiterCeilingDb{-1.0f};
    std::atomic<float> limiterReleaseMs{80.0f};

    // -- coefficient invalidation -----------------------------------------
    std::atomic<u32> revision{1};

    Params()
    {
        for (int i = 0; i < kEqBands; ++i) {
            eqGainDb[i].store(0.0f, std::memory_order_relaxed);
            eqQ[i].store(1.0f, std::memory_order_relaxed);
        }
    }

    /// Call after changing anything the filter coefficients depend on.
    void touch() noexcept { revision.fetch_add(1, std::memory_order_release); }

    void resetToDefaults()
    {
        inputGainDb.store(0.0f);
        outputGainDb.store(0.0f);
        mute.store(false);
        bypassAll.store(false);
        hpfEnabled.store(true);
        hpfHz.store(80.0f);
        lpfEnabled.store(false);
        lpfHz.store(16000.0f);
        eqEnabled.store(true);
        for (int i = 0; i < kEqBands; ++i) {
            eqGainDb[i].store(0.0f);
            eqQ[i].store(1.0f);
        }
        gateEnabled.store(true);
        gateThresholdDb.store(-45.0f);
        gateHysteresisDb.store(6.0f);
        gateRangeDb.store(-60.0f);
        gateAttackMs.store(2.0f);
        gateHoldMs.store(120.0f);
        gateReleaseMs.store(180.0f);
        gateLookaheadMs.store(3.0f);
        gateSidechainHpfHz.store(120.0f);
        compEnabled.store(true);
        compThresholdDb.store(-18.0f);
        compRatio.store(3.0f);
        compKneeDb.store(6.0f);
        compAttackMs.store(8.0f);
        compReleaseMs.store(120.0f);
        compMakeupDb.store(0.0f);
        compAutoMakeup.store(true);
        compLookaheadMs.store(2.0f);
        compSidechainHpfHz.store(100.0f);
        compRmsDetection.store(true);
        inputMix.store(static_cast<int>(InputMix::Stereo));
        monoOutput.store(false);
        limiterEnabled.store(true);
        limiterCeilingDb.store(-1.0f);
        limiterReleaseMs.store(80.0f);
        touch();
    }
};

/// Audio-thread telemetry, read by the UI once per frame. Values are advisory;
/// tearing between fields is irrelevant for a meter.
struct Meters {
    std::atomic<float> inputPeak{0.0f};
    std::atomic<float> inputRms{0.0f};
    std::atomic<float> outputPeak{0.0f};
    std::atomic<float> outputRms{0.0f};
    std::atomic<float> gateReductionDb{0.0f};
    std::atomic<float> compressorReductionDb{0.0f};
    std::atomic<float> limiterReductionDb{0.0f};
    std::atomic<bool>  gateOpen{false};

    /// Fraction of the available block period spent in the processing callback.
    std::atomic<float> cpuLoad{0.0f};
    /// Buffer under/overruns since the stream started.
    std::atomic<u32>   inputXruns{0};
    std::atomic<u32>   outputXruns{0};
    /// Resampler ratio deviation from 1.0, in parts per million - how far the
    /// input and output device clocks have drifted apart.
    std::atomic<float> driftPpm{0.0f};
    /// Set when the capture buffer has stayed nearly empty for long enough that
    /// the device is clearly not delivering audio at all - a selected-but-idle
    /// virtual microphone, a muted endpoint, an unplugged interface. Without
    /// this the user sees a running engine, silence, and no explanation.
    std::atomic<bool>  inputStarved{false};
    /// End-to-end latency estimate.
    std::atomic<float> latencyMs{0.0f};

    void reset()
    {
        inputPeak.store(0.0f);
        inputRms.store(0.0f);
        outputPeak.store(0.0f);
        outputRms.store(0.0f);
        gateReductionDb.store(0.0f);
        compressorReductionDb.store(0.0f);
        limiterReductionDb.store(0.0f);
        gateOpen.store(false);
        cpuLoad.store(0.0f);
        inputXruns.store(0);
        outputXruns.store(0);
        driftPpm.store(0.0f);
        inputStarved.store(false);
        latencyMs.store(0.0f);
    }
};

} // namespace rv
