#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "dsp/Node.h"

namespace rv::dsp {

using NodePtr = std::shared_ptr<ProcessorNode>;

/// The reorderable processing chain.
///
/// Built-in modules and hosted plugins live in the same list, so the user can
/// place a de-esser before the EQ or the gate after a compressor simply by
/// dragging.
///
/// Publishing a new order to a running audio thread is the delicate part. The
/// invariant here is that **the audio thread never allocates, frees or takes a
/// lock** - it only ever performs one atomic exchange:
///
///   1. The UI builds a fresh `Layout` (a vector of borrowed node pointers),
///      keeps ownership of it in `layouts_`, and parks a raw pointer in
///      `pending_`.
///   2. At the top of its next block the audio thread exchanges `pending_` into
///      `active_` and bumps `swapCount_`. Nothing is destroyed.
///   3. The UI observes `swapCount_` advance, which proves the audio thread has
///      left the previous layout, and only then releases the superseded
///      `Layout` objects and its references to removed nodes.
///
/// Node ownership stays on the UI thread throughout; the audio thread sees
/// borrowed pointers whose lifetime step 3 guarantees.
class Chain {
public:
    Chain() = default;
    ~Chain() = default;

    Chain(const Chain&) = delete;
    Chain& operator=(const Chain&) = delete;

    // ---- UI thread -------------------------------------------------------

    /// Prepares every node for a new stream format. The engine stops the audio
    /// thread around this.
    void prepare(double sampleRate, int maxFrames, int channels);

    /// Replaces the chain contents and order. Nodes absent from `nodes` are
    /// released once the audio thread has demonstrably moved on.
    void setNodes(std::vector<NodePtr> nodes);

    /// Snapshot for the UI to render and reorder.
    std::vector<NodePtr> nodes() const;

    size_t size() const;

    /// Sum of the reported latency of all non-bypassed nodes.
    int latencySamples() const;

    /// Frees superseded layouts and removed nodes. Call once per UI frame.
    void collectGarbage();

    /// Tells the chain whether an audio thread is currently consuming it. When
    /// it is not, hand-offs complete immediately instead of waiting for a swap
    /// that will never happen.
    void setAudioRunning(bool running);

    void resetAllNodes();

    // ---- audio thread ----------------------------------------------------

    void process(PlanarBuffer& buffer);

private:
    struct Layout {
        std::vector<ProcessorNode*> nodes;
    };

    /// Adopts a pending layout if one is waiting. Shared by the audio thread
    /// and by the UI thread when no audio thread is running.
    void adoptPending();

    // Audio-visible state.
    std::atomic<Layout*> pending_{nullptr};
    Layout*              active_ = nullptr;
    std::atomic<u64>     swapCount_{0};
    std::atomic<bool>    audioRunning_{false};

    // UI-owned state. `layouts_` keeps every published layout alive until a
    // swap proves the older ones are unreachable; the newest entry is always
    // the one the audio thread is using or about to use.
    mutable std::mutex                   uiMutex_;
    std::vector<std::unique_ptr<Layout>> layouts_;
    std::vector<NodePtr>                 owned_;
    std::vector<NodePtr>                 graveyard_;
    u64                                  publishMark_ = 0;

    double sampleRate_ = 48000.0;
    int    maxFrames_  = 256;
    int    channels_   = 2;
};

} // namespace rv::dsp
