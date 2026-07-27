#pragma once

#include <memory>
#include <string>
#include <vector>

#include "dsp/Node.h"
#include "host/PluginDescriptor.h"

namespace rv::host {

/// A hosted VST3 effect, usable anywhere in the processing chain.
///
/// The SDK headers are kept behind a pimpl deliberately: they pull in a large
/// amount of COM boilerplate and are only available when the build was
/// configured with the VST3 SDK. Everything above this line - the chain, the
/// UI, the configuration - compiles either way.
class Vst3Plugin final : public dsp::ProcessorNode {
public:
    /// One exposed parameter, mirrored for the built-in generic editor that is
    /// offered when a plugin has no view of its own (or its view fails to
    /// attach).
    struct ParameterInfo {
        u32         id = 0;
        std::string title;
        std::string units;
        double      defaultNormalized = 0.0;
        int         stepCount = 0;   ///< 0 means continuous.
        bool        isBypass = false;
        bool        isReadOnly = false;
    };

    /// Instantiates `descriptor` and prepares it for the given format.
    /// Returns null and fills `error` on failure.
    static std::unique_ptr<Vst3Plugin> create(const PluginDescriptor& descriptor,
                                              double sampleRate, int maxBlockFrames,
                                              int channels, std::string& error);

    ~Vst3Plugin() override;

    // --- ProcessorNode ----------------------------------------------------
    dsp::NodeKind kind() const override { return dsp::NodeKind::Vst3Plugin; }
    const std::string& name() const override;
    void prepare(double sampleRate, int maxFrames, int channels) override;
    void reset() override;
    void process(dsp::PlanarBuffer& buffer) override;
    int  latencySamples() const override;

    const PluginDescriptor& descriptor() const;

    // --- editor -----------------------------------------------------------
    bool hasEditor() const;

    /// Opens the plugin's own window, or raises it if already open.
    /// `ownerWindow` becomes its owner so it stays above the main window.
    bool openEditor(void* ownerWindow);
    void closeEditor();
    bool isEditorOpen() const;

    // --- parameters -------------------------------------------------------
    const std::vector<ParameterInfo>& parameters() const;
    double      parameterValue(u32 id) const;
    void        setParameterValue(u32 id, double normalized);
    std::string parameterDisplay(u32 id) const;

    // --- state ------------------------------------------------------------
    /// Component and controller state, packed together and base64 encoded so
    /// it can live in the JSON configuration.
    std::string saveState() const;
    bool        loadState(const std::string& base64);

    /// Public only so that the SDK glue in the implementation file - the
    /// component handler in particular - can name it. Nothing outside that
    /// file can do anything with an incomplete type.
    struct Impl;

private:
    Vst3Plugin();

    std::unique_ptr<Impl> impl_;
};

} // namespace rv::host
