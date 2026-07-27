#include "host/vst3/Vst3Plugin.h"

#include "core/Log.h"
#include "core/Strings.h"

#if RV_HAS_VST3

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

#include "base/source/fobject.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/utility/uid.h"

#include "host/vst3/Vst3Editor.h"

using namespace Steinberg;

namespace rv::host {
namespace {

/// The host context is handed to every plugin and must outlive all of them.
/// It is intentionally never released: plugins addRef and release it freely,
/// and letting the refcount reach zero would destroy an object the next
/// plugin still expects to be there.
Vst::IHostApplication* hostContext()
{
    static Vst::IHostApplication* instance = [] {
        auto* host = new Vst::HostApplication();
        host->addRef(); // deliberately leaked, see above
        return static_cast<Vst::IHostApplication*>(host);
    }();
    return instance;
}

std::string fromVstString(const Vst::TChar* text, size_t maxLength = 128)
{
    if (!text)
        return {};

    // TChar is UTF-16, which is what wchar_t is on Windows.
    const auto* wide = reinterpret_cast<const wchar_t*>(text);
    size_t length = 0;
    while (length < maxLength && wide[length] != L'\0')
        ++length;

    return toUtf8(std::wstring_view(wide, length));
}

/// Lock-free single-producer / single-consumer queue of parameter edits.
///
/// Used in both directions: the UI (or the plugin's own editor, whose callbacks
/// arrive on the UI thread) pushes edits that the audio thread must apply, and
/// the audio thread pushes automation output that the UI thread feeds back into
/// the controller so the plugin's knobs move.
class ParameterQueue {
public:
    struct Entry {
        Vst::ParamID    id = 0;
        Vst::ParamValue value = 0.0;
    };

    /// Returns false when full; a dropped parameter edit is preferable to
    /// blocking either thread.
    bool push(Vst::ParamID id, Vst::ParamValue value)
    {
        const u32 write = write_.load(std::memory_order_relaxed);
        const u32 read  = read_.load(std::memory_order_acquire);
        if (write - read >= kCapacity)
            return false;

        entries_[write & kMask] = {id, value};
        write_.store(write + 1, std::memory_order_release);
        return true;
    }

    bool pop(Entry& out)
    {
        const u32 read  = read_.load(std::memory_order_relaxed);
        const u32 write = write_.load(std::memory_order_acquire);
        if (read == write)
            return false;

        out = entries_[read & kMask];
        read_.store(read + 1, std::memory_order_release);
        return true;
    }

private:
    static constexpr u32 kCapacity = 1024;
    static constexpr u32 kMask     = kCapacity - 1;

    std::array<Entry, kCapacity> entries_{};
    std::atomic<u32> write_{0};
    std::atomic<u32> read_{0};
};

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct Vst3Plugin::Impl {
    PluginDescriptor descriptor;
    std::string      displayName;

    VST3::Hosting::Module::Ptr module;
    IPtr<Vst::IComponent>      component;
    IPtr<Vst::IAudioProcessor> processor;
    IPtr<Vst::IEditController> controller;

    IPtr<Vst::IConnectionPoint> componentConnection;
    IPtr<Vst::IConnectionPoint> controllerConnection;

    std::unique_ptr<Vst3EditorWindow> editor;

    std::vector<ParameterInfo> parameters;
    /// Guards `parameters` and the controller, which is UI-thread-only but read
    /// by the configuration save path.
    mutable std::mutex controllerMutex;

    ParameterQueue toAudio;   ///< UI/editor -> audio thread.
    ParameterQueue fromAudio; ///< Automation output -> UI thread.

    // Process plumbing, all preallocated.
    Vst::ProcessData       processData{};
    Vst::ProcessContext    processContext{};
    Vst::ParameterChanges  inputChanges;
    Vst::ParameterChanges  outputChanges;
    Vst::EventList         inputEvents;
    Vst::EventList         outputEvents;

    std::vector<Vst::AudioBusBuffers> inputBuses;
    std::vector<Vst::AudioBusBuffers> outputBuses;

    /// Channel pointer tables handed to the plugin. Input points into the
    /// caller's buffer; output points into `outputStorage`.
    std::vector<float*> mainInputPointers;
    std::vector<float*> mainOutputPointers;
    std::vector<float*> auxInputPointers;
    std::vector<float*> auxOutputPointers;

    dsp::PlanarBuffer outputStorage;
    std::vector<float> silence; ///< Zeros fed to unused side-chain inputs.
    std::vector<float> dump;    ///< Sink for unused auxiliary outputs.

    double sampleRate    = 48000.0;
    int    maxFrames     = 512;
    int    channels      = 2;
    int    mainInChannels  = 0;
    int    mainOutChannels = 0;

    std::atomic<int>  latency{0};
    std::atomic<bool> active{false};
    /// Set by restartComponent when the plugin wants its parameters re-read.
    std::atomic<bool> parametersDirty{false};

    ~Impl() { teardown(); }

    void teardown()
    {
        if (editor) {
            editor->close();
            editor.reset();
        }

        if (processor && active.load())
            processor->setProcessing(false);

        if (component && active.load())
            component->setActive(false);

        active.store(false);

        // Disconnect before releasing, otherwise each side is left holding a
        // pointer to an object that is about to be destroyed.
        if (componentConnection && controllerConnection) {
            componentConnection->disconnect(controllerConnection);
            controllerConnection->disconnect(componentConnection);
        }
        componentConnection.reset();
        controllerConnection.reset();

        if (controller) {
            controller->setComponentHandler(nullptr);
            controller->terminate();
            controller.reset();
        }

        processor.reset();

        if (component) {
            component->terminate();
            component.reset();
        }

        module.reset();
    }

    void refreshParameters()
    {
        std::lock_guard lock(controllerMutex);

        parameters.clear();
        if (!controller)
            return;

        const int32 count = controller->getParameterCount();
        parameters.reserve(static_cast<size_t>(std::max(0, count)));

        for (int32 i = 0; i < count; ++i) {
            Vst::ParameterInfo info{};
            if (controller->getParameterInfo(i, info) != kResultOk)
                continue;

            // Hidden parameters exist for the plugin's internal bookkeeping and
            // are meaningless in a generic editor.
            if (info.flags & Vst::ParameterInfo::kIsHidden)
                continue;

            ParameterInfo entry;
            entry.id                = info.id;
            entry.title             = fromVstString(info.title);
            entry.units             = fromVstString(info.units);
            entry.defaultNormalized = info.defaultNormalizedValue;
            entry.stepCount         = info.stepCount;
            entry.isBypass          = (info.flags & Vst::ParameterInfo::kIsBypass) != 0;
            entry.isReadOnly        = (info.flags & Vst::ParameterInfo::kIsReadOnly) != 0;

            parameters.push_back(std::move(entry));
        }
    }

    /// Rebuilds the bus buffer tables for the current arrangement.
    void configureBuses()
    {
        const int32 inputBusCount  = component->getBusCount(Vst::kAudio, Vst::kInput);
        const int32 outputBusCount = component->getBusCount(Vst::kAudio, Vst::kOutput);

        inputBuses.assign(static_cast<size_t>(std::max<int32>(0, inputBusCount)), {});
        outputBuses.assign(static_cast<size_t>(std::max<int32>(0, outputBusCount)), {});

        Vst::SpeakerArrangement arrangement = 0;
        mainInChannels = 0;
        if (inputBusCount > 0 &&
            processor->getBusArrangement(Vst::kInput, 0, arrangement) == kResultOk) {
            mainInChannels = Vst::SpeakerArr::getChannelCount(arrangement);
        }

        mainOutChannels = 0;
        if (outputBusCount > 0 &&
            processor->getBusArrangement(Vst::kOutput, 0, arrangement) == kResultOk) {
            mainOutChannels = Vst::SpeakerArr::getChannelCount(arrangement);
        }

        mainInChannels  = std::clamp(mainInChannels, 0, kMaxChannels);
        mainOutChannels = std::clamp(mainOutChannels, 0, kMaxChannels);

        mainInputPointers.assign(static_cast<size_t>(std::max(1, mainInChannels)), nullptr);
        mainOutputPointers.assign(static_cast<size_t>(std::max(1, mainOutChannels)), nullptr);

        outputStorage.resize(std::max(1, mainOutChannels), maxFrames);
        silence.assign(static_cast<size_t>(maxFrames), 0.0f);
        dump.assign(static_cast<size_t>(maxFrames), 0.0f);

        // Side-chain and auxiliary buses are wired to shared silence and a
        // shared sink. They stay deactivated, but a conforming host still has
        // to hand the plugin valid pointers for every bus it declares.
        auxInputPointers.assign(kMaxChannels, silence.data());
        auxOutputPointers.assign(kMaxChannels, dump.data());

        for (size_t i = 0; i < inputBuses.size(); ++i) {
            if (i == 0) {
                inputBuses[i].numChannels     = mainInChannels;
                inputBuses[i].channelBuffers32 = mainInputPointers.data();
            } else {
                inputBuses[i].numChannels     = kMaxChannels;
                inputBuses[i].channelBuffers32 = auxInputPointers.data();
                inputBuses[i].silenceFlags    = ~Steinberg::uint64(0);
            }
        }

        for (size_t i = 0; i < outputBuses.size(); ++i) {
            if (i == 0) {
                outputBuses[i].numChannels      = mainOutChannels;
                outputBuses[i].channelBuffers32 = mainOutputPointers.data();
            } else {
                outputBuses[i].numChannels      = kMaxChannels;
                outputBuses[i].channelBuffers32 = auxOutputPointers.data();
            }
        }

        processData.numInputs  = static_cast<int32>(inputBuses.size());
        processData.numOutputs = static_cast<int32>(outputBuses.size());
        processData.inputs     = inputBuses.empty() ? nullptr : inputBuses.data();
        processData.outputs    = outputBuses.empty() ? nullptr : outputBuses.data();
    }
};

// ---------------------------------------------------------------------------
// Component handler
// ---------------------------------------------------------------------------

namespace {

class ComponentHandler : public FObject, public Vst::IComponentHandler {
public:
    explicit ComponentHandler(Vst3Plugin::Impl& owner) : owner_(owner) {}

    tresult PLUGIN_API beginEdit(Vst::ParamID) override { return kResultOk; }
    tresult PLUGIN_API endEdit(Vst::ParamID) override { return kResultOk; }

    tresult PLUGIN_API performEdit(Vst::ParamID id, Vst::ParamValue value) override
    {
        // The plugin's own editor moved a knob. The controller already knows;
        // the processor does not, and only finds out through a parameter change
        // in the next process call.
        owner_.toAudio.push(id, value);
        return kResultOk;
    }

    tresult PLUGIN_API restartComponent(int32 flags) override
    {
        if (flags & Vst::kLatencyChanged) {
            if (owner_.processor) {
                owner_.latency.store(static_cast<int>(owner_.processor->getLatencySamples()),
                                     std::memory_order_relaxed);
            }
        }
        if (flags & (Vst::kParamValuesChanged | Vst::kParamTitlesChanged))
            owner_.parametersDirty.store(true, std::memory_order_relaxed);

        if (flags & Vst::kReloadComponent) {
            // A full reload means tearing the plugin down and building it back
            // up, which cannot be done from inside a plugin callback. Very few
            // effects ask for it; logging keeps the situation diagnosable
            // rather than silently wrong.
            RV_WARN("VST3 plugin requested a full component reload, which is not supported; "
                    "remove and re-add it to apply the change");
        }
        return kResultOk;
    }

    OBJ_METHODS(ComponentHandler, FObject)
    DEFINE_INTERFACES
        DEF_INTERFACE(Vst::IComponentHandler)
    END_DEFINE_INTERFACES(FObject)
    REFCOUNT_METHODS(FObject)

private:
    Vst3Plugin::Impl& owner_;
};

} // namespace

// ---------------------------------------------------------------------------
// Vst3Plugin
// ---------------------------------------------------------------------------

Vst3Plugin::Vst3Plugin() : impl_(std::make_unique<Impl>()) {}
Vst3Plugin::~Vst3Plugin() = default;

std::unique_ptr<Vst3Plugin> Vst3Plugin::create(const PluginDescriptor& descriptor,
                                               double sampleRate, int maxBlockFrames,
                                               int channels, std::string& error)
{
    std::unique_ptr<Vst3Plugin> plugin(new Vst3Plugin());
    Impl& impl = *plugin->impl_;

    impl.descriptor  = descriptor;
    impl.displayName = descriptor.name;
    impl.sampleRate  = sampleRate;
    impl.maxFrames   = maxBlockFrames;
    impl.channels    = std::clamp(channels, 1, kMaxChannels);

    impl.module = VST3::Hosting::Module::create(descriptor.path, error);
    if (!impl.module) {
        if (error.empty())
            error = "the bundle could not be loaded";
        return nullptr;
    }

    const auto uid = VST3::UID::fromString(descriptor.uid);
    if (!uid) {
        error = "the stored class identifier is malformed";
        return nullptr;
    }

    const auto factory = impl.module->getFactory();
    factory.setHostContext(hostContext());

    impl.component = factory.createInstance<Vst::IComponent>(*uid);
    if (!impl.component) {
        error = "the plugin class could not be instantiated";
        return nullptr;
    }

    if (impl.component->initialize(hostContext()) != kResultOk) {
        error = "the plugin refused to initialise";
        return nullptr;
    }

    impl.processor = FUnknownPtr<Vst::IAudioProcessor>(impl.component);
    if (!impl.processor) {
        error = "the plugin class is not an audio processor";
        return nullptr;
    }

    if (impl.processor->canProcessSampleSize(Vst::kSample32) != kResultTrue) {
        error = "the plugin does not support 32-bit float processing";
        return nullptr;
    }

    // ---- controller ------------------------------------------------------
    // A plugin may implement the controller in the same object as the
    // component (the "single component effect" pattern) or in a separate class
    // whose id the component reports.
    impl.controller = FUnknownPtr<Vst::IEditController>(impl.component);
    if (!impl.controller) {
        TUID controllerId{};
        if (impl.component->getControllerClassId(controllerId) == kResultOk) {
            impl.controller =
                factory.createInstance<Vst::IEditController>(VST3::UID(controllerId));
            if (impl.controller && impl.controller->initialize(hostContext()) != kResultOk) {
                RV_WARN("VST3 \"%s\": the edit controller refused to initialise",
                        descriptor.name.c_str());
                impl.controller.reset();
            }
        }
    }

    if (impl.controller) {
        auto* handler = new ComponentHandler(impl);
        impl.controller->setComponentHandler(handler);

        // Give the controller the component's current state, which is how the
        // plugin's UI learns the processor's defaults.
        MemoryStream stream;
        if (impl.component->getState(&stream) == kResultOk) {
            stream.seek(0, IBStream::kIBSeekSet, nullptr);
            impl.controller->setComponentState(&stream);
        }

        // The two halves exchange private messages over this connection.
        impl.componentConnection  = FUnknownPtr<Vst::IConnectionPoint>(impl.component);
        impl.controllerConnection = FUnknownPtr<Vst::IConnectionPoint>(impl.controller);
        if (impl.componentConnection && impl.controllerConnection) {
            impl.componentConnection->connect(impl.controllerConnection);
            impl.controllerConnection->connect(impl.componentConnection);
        }

        impl.refreshParameters();
    }

    plugin->prepare(sampleRate, maxBlockFrames, impl.channels);

    RV_INFO("VST3 loaded: \"%s\" by %s (%d in / %d out, %d parameters, %d samples latency)",
            descriptor.name.c_str(), descriptor.vendor.c_str(),
            impl.mainInChannels, impl.mainOutChannels,
            static_cast<int>(impl.parameters.size()),
            impl.latency.load());

    return plugin;
}

const std::string& Vst3Plugin::name() const
{
    return impl_->displayName;
}

const PluginDescriptor& Vst3Plugin::descriptor() const
{
    return impl_->descriptor;
}

void Vst3Plugin::prepare(double sampleRate, int maxFrames, int channels)
{
    Impl& impl = *impl_;
    if (!impl.component || !impl.processor)
        return;

    impl.sampleRate = sampleRate;
    impl.maxFrames  = maxFrames;
    impl.channels   = std::clamp(channels, 1, kMaxChannels);

    if (impl.active.load()) {
        impl.processor->setProcessing(false);
        impl.component->setActive(false);
        impl.active.store(false);
    }

    // ---- bus arrangement -------------------------------------------------
    const int32 inputBusCount  = impl.component->getBusCount(Vst::kAudio, Vst::kInput);
    const int32 outputBusCount = impl.component->getBusCount(Vst::kAudio, Vst::kOutput);

    const Vst::SpeakerArrangement wanted =
        (impl.channels == 1) ? Vst::SpeakerArr::kMono : Vst::SpeakerArr::kStereo;

    // Auxiliary buses are asked for kEmpty so side-chains stay out of the way;
    // plugins that insist on their side-chain simply keep their own default.
    std::vector<Vst::SpeakerArrangement> inputArrangements(
        static_cast<size_t>(std::max<int32>(0, inputBusCount)), Vst::SpeakerArr::kEmpty);
    std::vector<Vst::SpeakerArrangement> outputArrangements(
        static_cast<size_t>(std::max<int32>(0, outputBusCount)), Vst::SpeakerArr::kEmpty);

    if (!inputArrangements.empty())
        inputArrangements[0] = wanted;
    if (!outputArrangements.empty())
        outputArrangements[0] = wanted;

    if (impl.processor->setBusArrangements(
            inputArrangements.empty() ? nullptr : inputArrangements.data(),
            static_cast<int32>(inputArrangements.size()),
            outputArrangements.empty() ? nullptr : outputArrangements.data(),
            static_cast<int32>(outputArrangements.size())) != kResultOk) {
        RV_WARN("VST3 \"%s\" rejected a %d-channel arrangement; using its own default",
                impl.displayName.c_str(), impl.channels);
    }

    // ---- processing setup ------------------------------------------------
    Vst::ProcessSetup setup{};
    setup.processMode         = Vst::kRealtime;
    setup.symbolicSampleSize  = Vst::kSample32;
    setup.maxSamplesPerBlock  = maxFrames;
    setup.sampleRate          = sampleRate;

    if (impl.processor->setupProcessing(setup) != kResultOk)
        RV_WARN("VST3 \"%s\": setupProcessing failed", impl.displayName.c_str());

    // Main buses on, everything else off.
    for (int32 i = 0; i < inputBusCount; ++i)
        impl.component->activateBus(Vst::kAudio, Vst::kInput, i, i == 0);
    for (int32 i = 0; i < outputBusCount; ++i)
        impl.component->activateBus(Vst::kAudio, Vst::kOutput, i, i == 0);

    // Event buses stay off: this is a microphone processor with no MIDI source.
    for (int32 i = 0; i < impl.component->getBusCount(Vst::kEvent, Vst::kInput); ++i)
        impl.component->activateBus(Vst::kEvent, Vst::kInput, i, false);

    impl.configureBuses();

    // ---- process data ----------------------------------------------------
    impl.processContext = {};
    impl.processContext.sampleRate           = sampleRate;
    impl.processContext.tempo                = 120.0;
    impl.processContext.timeSigNumerator     = 4;
    impl.processContext.timeSigDenominator   = 4;
    // Tempo-synced effects need these flags set or they fall back to defaults
    // that vary between plugins.
    impl.processContext.state = Vst::ProcessContext::kPlaying |
                                Vst::ProcessContext::kTempoValid |
                                Vst::ProcessContext::kTimeSigValid |
                                Vst::ProcessContext::kProjectTimeMusicValid;

    impl.processData.processMode            = Vst::kRealtime;
    impl.processData.symbolicSampleSize     = Vst::kSample32;
    impl.processData.inputParameterChanges  = &impl.inputChanges;
    impl.processData.outputParameterChanges = &impl.outputChanges;
    impl.processData.inputEvents            = &impl.inputEvents;
    impl.processData.outputEvents           = &impl.outputEvents;
    impl.processData.processContext         = &impl.processContext;

    if (impl.component->setActive(true) != kResultOk) {
        RV_ERROR("VST3 \"%s\": setActive failed", impl.displayName.c_str());
        return;
    }
    impl.processor->setProcessing(true);
    impl.active.store(true);

    impl.latency.store(static_cast<int>(impl.processor->getLatencySamples()),
                       std::memory_order_relaxed);
}

void Vst3Plugin::reset()
{
    Impl& impl = *impl_;
    if (!impl.processor || !impl.active.load())
        return;

    // Cycling setProcessing is the specified way to flush a plugin's internal
    // delay lines and filter state without a full re-activation.
    impl.processor->setProcessing(false);
    impl.processor->setProcessing(true);
}

int Vst3Plugin::latencySamples() const
{
    return impl_->latency.load(std::memory_order_relaxed);
}

void Vst3Plugin::process(dsp::PlanarBuffer& buffer)
{
    Impl& impl = *impl_;
    if (!impl.active.load(std::memory_order_relaxed) || !impl.processor)
        return;

    const int frames = buffer.frames();
    if (frames <= 0 || frames > impl.maxFrames)
        return;

    // ---- parameter edits from the UI and the plugin's own editor ----------
    impl.inputChanges.clearQueue();
    ParameterQueue::Entry entry;
    while (impl.toAudio.pop(entry)) {
        int32 queueIndex = 0;
        if (auto* queue = impl.inputChanges.addParameterData(entry.id, queueIndex)) {
            int32 pointIndex = 0;
            queue->addPoint(0, entry.value, pointIndex);
        }
    }

    impl.outputChanges.clearQueue();

    // ---- wire the buffers ------------------------------------------------
    for (size_t c = 0; c < impl.mainInputPointers.size(); ++c) {
        const int source = std::min(static_cast<int>(c), buffer.channels() - 1);
        impl.mainInputPointers[c] = buffer.channel(std::max(0, source));
    }

    impl.outputStorage.setActiveFrames(frames);
    for (size_t c = 0; c < impl.mainOutputPointers.size(); ++c) {
        const int index = std::min(static_cast<int>(c), impl.outputStorage.channels() - 1);
        impl.mainOutputPointers[c] = impl.outputStorage.channel(index);
    }

    for (auto& bus : impl.inputBuses)
        bus.silenceFlags = 0;
    for (auto& bus : impl.outputBuses)
        bus.silenceFlags = 0;

    impl.processData.numSamples = frames;

    if (impl.processor->process(impl.processData) != kResultOk)
        return;

    // ---- collect the result ----------------------------------------------
    const int copyChannels = std::min(buffer.channels(), impl.mainOutChannels);
    for (int c = 0; c < copyChannels; ++c) {
        std::memcpy(buffer.channel(c), impl.mainOutputPointers[static_cast<size_t>(c)],
                    static_cast<size_t>(frames) * sizeof(float));
    }
    // A mono-output plugin in a stereo chain: mirror rather than leaving the
    // right channel holding the previous block.
    for (int c = copyChannels; c < buffer.channels(); ++c) {
        const int source = std::max(0, copyChannels - 1);
        std::memcpy(buffer.channel(c), buffer.channel(source),
                    static_cast<size_t>(frames) * sizeof(float));
    }

    // ---- automation output back to the controller ------------------------
    const int32 changedCount = impl.outputChanges.getParameterCount();
    for (int32 i = 0; i < changedCount; ++i) {
        auto* queue = impl.outputChanges.getParameterData(i);
        if (!queue)
            continue;
        const int32 points = queue->getPointCount();
        if (points <= 0)
            continue;

        int32           offset = 0;
        Vst::ParamValue value  = 0.0;
        if (queue->getPoint(points - 1, offset, value) == kResultOk)
            impl.fromAudio.push(queue->getParameterId(), value);
    }

    impl.processContext.projectTimeSamples += frames;
    impl.processContext.continousTimeSamples += frames;
}

// ---------------------------------------------------------------------------
// Editor
// ---------------------------------------------------------------------------

bool Vst3Plugin::hasEditor() const
{
    return impl_->controller != nullptr;
}

bool Vst3Plugin::openEditor(void* ownerWindow)
{
    Impl& impl = *impl_;
    if (!impl.controller)
        return false;

    if (impl.editor && impl.editor->isOpen()) {
        impl.editor->raise();
        return true;
    }

    IPtr<IPlugView> view = owned(impl.controller->createView(Vst::ViewType::kEditor));
    if (!view) {
        RV_INFO("VST3 \"%s\" has no editor of its own; use the parameter list",
                impl.displayName.c_str());
        return false;
    }

    impl.editor = std::make_unique<Vst3EditorWindow>();
    impl.editor->onClosed = [this] {
        // Dropping the window releases the view, so reopening builds a fresh
        // one; plugins tolerate that far better than repeated re-attachment.
        impl_->editor.reset();
    };

    if (!impl.editor->open(view, impl.displayName, static_cast<HWND>(ownerWindow))) {
        impl.editor.reset();
        return false;
    }

    return true;
}

void Vst3Plugin::closeEditor()
{
    if (impl_->editor) {
        impl_->editor->close();
        impl_->editor.reset();
    }
}

bool Vst3Plugin::isEditorOpen() const
{
    return impl_->editor && impl_->editor->isOpen();
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

const std::vector<Vst3Plugin::ParameterInfo>& Vst3Plugin::parameters() const
{
    Impl& impl = *impl_;

    // The plugin may have told us its parameter list changed; rebuilding here
    // keeps that off the audio thread.
    if (impl.parametersDirty.exchange(false, std::memory_order_relaxed))
        impl.refreshParameters();

    // Automation the processor emitted has to reach the controller, or the
    // plugin's own UI will not follow its own modulation.
    ParameterQueue::Entry entry;
    while (impl.fromAudio.pop(entry)) {
        std::lock_guard lock(impl.controllerMutex);
        if (impl.controller)
            impl.controller->setParamNormalized(entry.id, entry.value);
    }

    return impl.parameters;
}

double Vst3Plugin::parameterValue(u32 id) const
{
    std::lock_guard lock(impl_->controllerMutex);
    if (!impl_->controller)
        return 0.0;
    return impl_->controller->getParamNormalized(id);
}

void Vst3Plugin::setParameterValue(u32 id, double normalized)
{
    normalized = std::clamp(normalized, 0.0, 1.0);

    {
        std::lock_guard lock(impl_->controllerMutex);
        if (impl_->controller)
            impl_->controller->setParamNormalized(id, normalized);
    }

    // The controller and the processor are separate objects: setting the
    // controller alone changes what the UI shows and nothing that is heard.
    impl_->toAudio.push(id, normalized);
}

std::string Vst3Plugin::parameterDisplay(u32 id) const
{
    std::lock_guard lock(impl_->controllerMutex);
    if (!impl_->controller)
        return {};

    Vst::String128 text{};
    const Vst::ParamValue value = impl_->controller->getParamNormalized(id);
    if (impl_->controller->getParamStringByValue(id, value, text) != kResultOk)
        return {};

    return fromVstString(text);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

namespace {

void appendBlock(std::vector<unsigned char>& out, const void* data, u32 size)
{
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    out.push_back(static_cast<unsigned char>(size & 0xFF));
    out.push_back(static_cast<unsigned char>((size >> 8) & 0xFF));
    out.push_back(static_cast<unsigned char>((size >> 16) & 0xFF));
    out.push_back(static_cast<unsigned char>((size >> 24) & 0xFF));
    out.insert(out.end(), bytes, bytes + size);
}

/// Reads a length-prefixed block. Returns false when the buffer is truncated,
/// which is what a hand-edited or corrupted configuration file looks like.
bool readBlock(const std::vector<unsigned char>& in, size_t& offset,
               const unsigned char*& data, u32& size)
{
    if (offset + 4 > in.size())
        return false;

    size = static_cast<u32>(in[offset]) |
           (static_cast<u32>(in[offset + 1]) << 8) |
           (static_cast<u32>(in[offset + 2]) << 16) |
           (static_cast<u32>(in[offset + 3]) << 24);
    offset += 4;

    if (offset + size > in.size())
        return false;

    data = in.data() + offset;
    offset += size;
    return true;
}

} // namespace

std::string Vst3Plugin::saveState() const
{
    Impl& impl = *impl_;
    if (!impl.component)
        return {};

    std::vector<unsigned char> packed;

    MemoryStream componentState;
    if (impl.component->getState(&componentState) == kResultOk) {
        appendBlock(packed, componentState.getData(),
                    static_cast<u32>(componentState.getSize()));
    } else {
        appendBlock(packed, nullptr, 0);
    }

    MemoryStream controllerState;
    {
        std::lock_guard lock(impl.controllerMutex);
        if (impl.controller && impl.controller->getState(&controllerState) == kResultOk) {
            appendBlock(packed, controllerState.getData(),
                        static_cast<u32>(controllerState.getSize()));
        } else {
            appendBlock(packed, nullptr, 0);
        }
    }

    return base64Encode(packed);
}

bool Vst3Plugin::loadState(const std::string& base64)
{
    Impl& impl = *impl_;
    if (!impl.component || base64.empty())
        return false;

    const std::vector<unsigned char> packed = base64Decode(base64);

    size_t offset = 0;
    const unsigned char* data = nullptr;
    u32 size = 0;

    if (!readBlock(packed, offset, data, size))
        return false;

    if (size > 0) {
        MemoryStream stream(const_cast<void*>(static_cast<const void*>(data)),
                            static_cast<TSize>(size));
        if (impl.component->setState(&stream) != kResultOk)
            return false;

        // The controller has to see the same component state, or the plugin's
        // UI shows defaults while the processor runs the restored settings.
        stream.seek(0, IBStream::kIBSeekSet, nullptr);
        std::lock_guard lock(impl.controllerMutex);
        if (impl.controller)
            impl.controller->setComponentState(&stream);
    }

    if (readBlock(packed, offset, data, size) && size > 0) {
        MemoryStream stream(const_cast<void*>(static_cast<const void*>(data)),
                            static_cast<TSize>(size));
        std::lock_guard lock(impl.controllerMutex);
        if (impl.controller)
            impl.controller->setState(&stream);
    }

    impl.refreshParameters();
    return true;
}

} // namespace rv::host

#else // !RV_HAS_VST3

// ---------------------------------------------------------------------------
// SDK-less build: the type still exists so the chain, the UI and the saved
// configuration compile unchanged; it simply can never be instantiated.
// ---------------------------------------------------------------------------

namespace rv::host {

struct Vst3Plugin::Impl {
    PluginDescriptor           descriptor;
    std::string                name = "VST3 (unavailable)";
    std::vector<ParameterInfo> parameters;
};

Vst3Plugin::Vst3Plugin() : impl_(std::make_unique<Impl>()) {}
Vst3Plugin::~Vst3Plugin() = default;

std::unique_ptr<Vst3Plugin> Vst3Plugin::create(const PluginDescriptor&, double, int, int,
                                               std::string& error)
{
    error = "this build was compiled without the VST3 SDK";
    return nullptr;
}

const std::string& Vst3Plugin::name() const { return impl_->name; }
const PluginDescriptor& Vst3Plugin::descriptor() const { return impl_->descriptor; }
void Vst3Plugin::prepare(double, int, int) {}
void Vst3Plugin::reset() {}
void Vst3Plugin::process(dsp::PlanarBuffer&) {}
int  Vst3Plugin::latencySamples() const { return 0; }
bool Vst3Plugin::hasEditor() const { return false; }
bool Vst3Plugin::openEditor(void*) { return false; }
void Vst3Plugin::closeEditor() {}
bool Vst3Plugin::isEditorOpen() const { return false; }
const std::vector<Vst3Plugin::ParameterInfo>& Vst3Plugin::parameters() const
{
    return impl_->parameters;
}
double      Vst3Plugin::parameterValue(u32) const { return 0.0; }
void        Vst3Plugin::setParameterValue(u32, double) {}
std::string Vst3Plugin::parameterDisplay(u32) const { return {}; }
std::string Vst3Plugin::saveState() const { return {}; }
bool        Vst3Plugin::loadState(const std::string&) { return false; }

} // namespace rv::host

#endif // RV_HAS_VST3
