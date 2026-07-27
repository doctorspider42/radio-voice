#include "dsp/Chain.h"

#include <algorithm>

namespace rv::dsp {

void Chain::prepare(double sampleRate, int maxFrames, int channels)
{
    std::lock_guard lock(uiMutex_);

    sampleRate_ = sampleRate;
    maxFrames_  = maxFrames;
    channels_   = channels;

    for (auto& node : owned_)
        node->prepare(sampleRate, maxFrames, channels);
}

void Chain::setNodes(std::vector<NodePtr> nodes)
{
    std::lock_guard lock(uiMutex_);

    // Nodes new to the chain have not seen prepare() yet.
    for (auto& node : nodes) {
        const bool wasPresent =
            std::find(owned_.begin(), owned_.end(), node) != owned_.end();
        if (!wasPresent)
            node->prepare(sampleRate_, maxFrames_, channels_);
    }

    // Anything dropped from the chain must outlive the audio thread's current
    // block, so it waits in the graveyard instead of being released here.
    for (auto& previous : owned_) {
        const bool stillPresent =
            std::find(nodes.begin(), nodes.end(), previous) != nodes.end();
        if (!stillPresent)
            graveyard_.push_back(previous);
    }

    auto layout = std::make_unique<Layout>();
    layout->nodes.reserve(nodes.size());
    for (auto& node : nodes)
        layout->nodes.push_back(node.get());

    Layout* raw = layout.get();
    layouts_.push_back(std::move(layout));

    owned_ = std::move(nodes);

    // Overwriting a pending layout the audio thread never adopted is fine: it
    // is still owned by `layouts_` and will be reclaimed by collectGarbage.
    pending_.store(raw, std::memory_order_release);
    publishMark_ = swapCount_.load(std::memory_order_acquire);

    if (!audioRunning_.load(std::memory_order_acquire))
        adoptPending();
}

std::vector<NodePtr> Chain::nodes() const
{
    std::lock_guard lock(uiMutex_);
    return owned_;
}

size_t Chain::size() const
{
    std::lock_guard lock(uiMutex_);
    return owned_.size();
}

int Chain::latencySamples() const
{
    std::lock_guard lock(uiMutex_);

    int total = 0;
    for (const auto& node : owned_) {
        if (!node->isBypassed())
            total += node->latencySamples();
    }
    return total;
}

void Chain::collectGarbage()
{
    std::lock_guard lock(uiMutex_);

    const bool running = audioRunning_.load(std::memory_order_acquire);
    const u64  swaps   = swapCount_.load(std::memory_order_acquire);

    // A swap recorded after the most recent publish proves the audio thread has
    // adopted the newest layout and can no longer reach any older one.
    if (running && swaps <= publishMark_)
        return;

    if (layouts_.size() > 1) {
        layouts_.erase(layouts_.begin(),
                       layouts_.begin() + static_cast<std::ptrdiff_t>(layouts_.size() - 1));
    }
    graveyard_.clear();
}

void Chain::setAudioRunning(bool running)
{
    audioRunning_.store(running, std::memory_order_release);

    if (!running) {
        std::lock_guard lock(uiMutex_);
        // Adopt anything still pending so the UI's view and the audio view
        // agree while the stream is stopped.
        adoptPending();
    }
}

void Chain::resetAllNodes()
{
    std::lock_guard lock(uiMutex_);
    for (auto& node : owned_)
        node->reset();
}

void Chain::adoptPending()
{
    if (Layout* incoming = pending_.exchange(nullptr, std::memory_order_acquire)) {
        active_ = incoming;
        swapCount_.fetch_add(1, std::memory_order_release);
    }
}

void Chain::process(PlanarBuffer& buffer)
{
    // The only thing the audio thread does with the topology: one exchange.
    adoptPending();

    if (!active_)
        return;

    for (ProcessorNode* node : active_->nodes) {
        if (!node->isBypassed())
            node->process(buffer);
    }
}

} // namespace rv::dsp
