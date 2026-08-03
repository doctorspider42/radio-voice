#pragma once

#include <atomic>
#include <string>

#include "dsp/AudioBuffer.h"

namespace rv::dsp {

enum class NodeKind {
    NoiseSuppressor,
    Gate,
    Equalizer,
    Compressor,
    Vst3Plugin,
};

/// One stage of the reorderable processing chain.
///
/// Built-in modules and hosted VST3 plugins implement the same interface, which
/// is what lets the user drag a plugin above or below the gate and the EQ
/// instead of being stuck with a fixed topology.
///
/// `process` runs on the audio thread and must not allocate, lock or block.
/// `prepare` and `reset` run on the UI thread while the chain is detached.
class ProcessorNode {
public:
    virtual ~ProcessorNode() = default;

    virtual NodeKind kind() const = 0;

    /// Display name for the chain list.
    virtual const std::string& name() const = 0;

    /// Called before the node joins a live chain. `maxFrames` is the largest
    /// block `process` will ever be given.
    virtual void prepare(double sampleRate, int maxFrames, int channels) = 0;

    /// Clears internal state (filter memory, delay lines, envelopes).
    virtual void reset() = 0;

    virtual void process(PlanarBuffer& buffer) = 0;

    /// Processing delay this node introduces, in samples. Reported to the user
    /// and summed into the end-to-end latency figure.
    virtual int latencySamples() const { return 0; }

    /// Bypass is read on the audio thread and toggled from the UI.
    bool isBypassed() const noexcept { return bypassed_.load(std::memory_order_relaxed); }
    void setBypassed(bool b) noexcept { bypassed_.store(b, std::memory_order_relaxed); }

    /// Whether the module is switched on, as the user means it.
    ///
    /// For a plugin that is the bypass flag and nothing more. A built-in module
    /// carries a second switch on its own panel, backed by the parameter block,
    /// and the two must not be separate pieces of state: one of them would then
    /// be off while the other said on, and which of the two the sound followed
    /// would depend on which one the user had reached for last. So the built-in
    /// modules override this to answer from their parameter, and their bypass
    /// flag is left alone.
    ///
    /// It is also the better switch for them on its own merits. Skipping the
    /// node outright drops its delay line and its detector state out of the
    /// path; the modules' own disabled branches keep both running and only stop
    /// acting on the signal, so switching back on does not splice.
    virtual bool isEnabled() const { return !isBypassed(); }
    virtual void setEnabled(bool on) { setBypassed(!on); }

    /// Stable handle used by the UI and by the saved configuration to refer to
    /// this node across chain rebuilds.
    u64 id() const noexcept { return id_; }

protected:
    ProcessorNode() : id_(nextId()) {}

private:
    static u64 nextId()
    {
        static std::atomic<u64> counter{1};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<bool> bypassed_{false};
    u64 id_;
};

} // namespace rv::dsp
