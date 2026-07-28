#include "app/Config.h"

#include <fstream>

#include <json.hpp>

#include "core/Log.h"
#include "core/Paths.h"

using nlohmann::json;

namespace rv::app {
namespace {

constexpr int kConfigVersion = 1;

void readDevice(const json& node, DeviceConfig& device)
{
    device.backend      = static_cast<BackendType>(node.value("backend", 0));
    device.wasapiMode   = static_cast<WasapiMode>(node.value("wasapiMode", 0));
    device.deviceId     = node.value("deviceId", "");
    device.deviceName   = node.value("deviceName", "");
    device.sampleRate   = node.value("sampleRate", 48000);
    device.channels     = node.value("channels", 2);
    device.bufferFrames = node.value("bufferFrames", 0);
}

json writeDevice(const DeviceConfig& device)
{
    return json{{"backend", static_cast<int>(device.backend)},
                {"wasapiMode", static_cast<int>(device.wasapiMode)},
                {"deviceId", device.deviceId},
                {"deviceName", device.deviceName},
                {"sampleRate", device.sampleRate},
                {"channels", device.channels},
                {"bufferFrames", device.bufferFrames}};
}

} // namespace

Config Config::load()
{
    Config config;

    // Defaults that cannot be expressed as member initialisers.
    for (int i = 0; i < kEqBands; ++i) {
        config.params.eqGainDb[i] = 0.0f;
        config.params.eqQ[i]      = 1.0f;
    }

    std::ifstream file(paths::configFile());
    if (!file) {
        RV_INFO("no configuration file yet; starting from defaults");
        return config;
    }

    json root;
    try {
        file >> root;
    } catch (const std::exception& e) {
        RV_WARN("configuration file is unreadable (%s); starting from defaults", e.what());
        return config;
    }

    if (root.value("version", 0) != kConfigVersion) {
        RV_WARN("configuration file is from a different version; starting from defaults");
        return config;
    }

    if (root.contains("input"))
        readDevice(root["input"], config.input);
    if (root.contains("output"))
        readDevice(root["output"], config.output);
    if (root.contains("monitor"))
        readDevice(root["monitor"], config.monitor);
    config.monitorEnabled = root.value("monitorEnabled", config.monitorEnabled);

    config.internalChannels = root.value("internalChannels", config.internalChannels);
    config.maxBlockFrames   = root.value("maxBlockFrames", config.maxBlockFrames);
    config.autoStart        = root.value("autoStart", config.autoStart);
    config.windowWidth      = root.value("windowWidth", config.windowWidth);
    config.windowHeight     = root.value("windowHeight", config.windowHeight);
    config.chainCollapsed   = root.value("chainCollapsed", config.chainCollapsed);
    config.ioCollapsed      = root.value("ioCollapsed", config.ioCollapsed);

    config.trayEnabled    = root.value("trayEnabled", config.trayEnabled);
    config.minimizeToTray = root.value("minimizeToTray", config.minimizeToTray);
    config.closeToTray    = root.value("closeToTray", config.closeToTray);
    config.startMinimized = root.value("startMinimized", config.startMinimized);

    if (root.contains("params")) {
        const json& p = root["params"];
        auto& v = config.params;

        v.inputGainDb  = p.value("inputGainDb", v.inputGainDb);
        v.outputGainDb = p.value("outputGainDb", v.outputGainDb);
        v.mute         = p.value("mute", v.mute);
        v.bypassAll    = p.value("bypassAll", v.bypassAll);
        v.monitorGainDb = p.value("monitorGainDb", v.monitorGainDb);
        v.monitorMute   = p.value("monitorMute", v.monitorMute);

        v.denoiseEnabled = p.value("denoiseEnabled", v.denoiseEnabled);
        v.denoiseAmount  = p.value("denoiseAmount", v.denoiseAmount);

        v.hpfEnabled = p.value("hpfEnabled", v.hpfEnabled);
        v.hpfHz      = p.value("hpfHz", v.hpfHz);
        v.lpfEnabled = p.value("lpfEnabled", v.lpfEnabled);
        v.lpfHz      = p.value("lpfHz", v.lpfHz);

        v.eqEnabled = p.value("eqEnabled", v.eqEnabled);
        if (p.contains("eqGainDb") && p["eqGainDb"].is_array()) {
            const auto& array = p["eqGainDb"];
            for (int i = 0; i < kEqBands && i < static_cast<int>(array.size()); ++i)
                v.eqGainDb[i] = array[static_cast<size_t>(i)].get<float>();
        }
        if (p.contains("eqQ") && p["eqQ"].is_array()) {
            const auto& array = p["eqQ"];
            for (int i = 0; i < kEqBands && i < static_cast<int>(array.size()); ++i)
                v.eqQ[i] = array[static_cast<size_t>(i)].get<float>();
        }

        v.gateEnabled        = p.value("gateEnabled", v.gateEnabled);
        v.gateThresholdDb    = p.value("gateThresholdDb", v.gateThresholdDb);
        v.gateHysteresisDb   = p.value("gateHysteresisDb", v.gateHysteresisDb);
        v.gateRangeDb        = p.value("gateRangeDb", v.gateRangeDb);
        v.gateAttackMs       = p.value("gateAttackMs", v.gateAttackMs);
        v.gateHoldMs         = p.value("gateHoldMs", v.gateHoldMs);
        v.gateReleaseMs      = p.value("gateReleaseMs", v.gateReleaseMs);
        v.gateLookaheadMs    = p.value("gateLookaheadMs", v.gateLookaheadMs);
        v.gateSidechainHpfHz = p.value("gateSidechainHpfHz", v.gateSidechainHpfHz);

        v.compEnabled        = p.value("compEnabled", v.compEnabled);
        v.compThresholdDb    = p.value("compThresholdDb", v.compThresholdDb);
        v.compRatio          = p.value("compRatio", v.compRatio);
        v.compKneeDb         = p.value("compKneeDb", v.compKneeDb);
        v.compAttackMs       = p.value("compAttackMs", v.compAttackMs);
        v.compReleaseMs      = p.value("compReleaseMs", v.compReleaseMs);
        v.compMakeupDb       = p.value("compMakeupDb", v.compMakeupDb);
        v.compAutoMakeup     = p.value("compAutoMakeup", v.compAutoMakeup);
        v.compLookaheadMs    = p.value("compLookaheadMs", v.compLookaheadMs);
        v.compSidechainHpfHz = p.value("compSidechainHpfHz", v.compSidechainHpfHz);
        v.compRmsDetection   = p.value("compRmsDetection", v.compRmsDetection);

        v.inputMix   = p.value("inputMix", v.inputMix);
        v.monoOutput = p.value("monoOutput", v.monoOutput);

        v.limiterEnabled   = p.value("limiterEnabled", v.limiterEnabled);
        v.limiterCeilingDb = p.value("limiterCeilingDb", v.limiterCeilingDb);
        v.limiterReleaseMs = p.value("limiterReleaseMs", v.limiterReleaseMs);
    }

    for (const auto& entry : root.value("chain", json::array())) {
        ChainEntryConfig node;
        node.kind        = entry.value("kind", "");
        node.bypassed    = entry.value("bypassed", false);
        node.pluginPath  = entry.value("pluginPath", "");
        node.pluginUid   = entry.value("pluginUid", "");
        node.pluginName  = entry.value("pluginName", "");
        node.pluginState = entry.value("pluginState", "");
        if (!node.kind.empty())
            config.chain.push_back(std::move(node));
    }

    return config;
}

void Config::save() const
{
    json root;
    root["version"] = kConfigVersion;
    root["input"]   = writeDevice(input);
    root["output"]  = writeDevice(output);
    root["monitor"] = writeDevice(monitor);

    root["monitorEnabled"] = monitorEnabled;

    root["internalChannels"] = internalChannels;
    root["maxBlockFrames"]   = maxBlockFrames;
    root["autoStart"]        = autoStart;
    root["windowWidth"]      = windowWidth;
    root["windowHeight"]     = windowHeight;
    root["chainCollapsed"]   = chainCollapsed;
    root["ioCollapsed"]      = ioCollapsed;

    root["trayEnabled"]    = trayEnabled;
    root["minimizeToTray"] = minimizeToTray;
    root["closeToTray"]    = closeToTray;
    root["startMinimized"] = startMinimized;

    json p;
    p["inputGainDb"]  = params.inputGainDb;
    p["outputGainDb"] = params.outputGainDb;
    p["mute"]         = params.mute;
    p["bypassAll"]    = params.bypassAll;
    p["monitorGainDb"] = params.monitorGainDb;
    p["monitorMute"]   = params.monitorMute;
    p["denoiseEnabled"] = params.denoiseEnabled;
    p["denoiseAmount"]  = params.denoiseAmount;
    p["hpfEnabled"]   = params.hpfEnabled;
    p["hpfHz"]        = params.hpfHz;
    p["lpfEnabled"]   = params.lpfEnabled;
    p["lpfHz"]        = params.lpfHz;
    p["eqEnabled"]    = params.eqEnabled;
    p["eqGainDb"]     = std::vector<float>(std::begin(params.eqGainDb), std::end(params.eqGainDb));
    p["eqQ"]          = std::vector<float>(std::begin(params.eqQ), std::end(params.eqQ));
    p["gateEnabled"]        = params.gateEnabled;
    p["gateThresholdDb"]    = params.gateThresholdDb;
    p["gateHysteresisDb"]   = params.gateHysteresisDb;
    p["gateRangeDb"]        = params.gateRangeDb;
    p["gateAttackMs"]       = params.gateAttackMs;
    p["gateHoldMs"]         = params.gateHoldMs;
    p["gateReleaseMs"]      = params.gateReleaseMs;
    p["gateLookaheadMs"]    = params.gateLookaheadMs;
    p["gateSidechainHpfHz"] = params.gateSidechainHpfHz;
    p["compEnabled"]        = params.compEnabled;
    p["compThresholdDb"]    = params.compThresholdDb;
    p["compRatio"]          = params.compRatio;
    p["compKneeDb"]         = params.compKneeDb;
    p["compAttackMs"]       = params.compAttackMs;
    p["compReleaseMs"]      = params.compReleaseMs;
    p["compMakeupDb"]       = params.compMakeupDb;
    p["compAutoMakeup"]     = params.compAutoMakeup;
    p["compLookaheadMs"]    = params.compLookaheadMs;
    p["compSidechainHpfHz"] = params.compSidechainHpfHz;
    p["compRmsDetection"]   = params.compRmsDetection;
    p["inputMix"]           = params.inputMix;
    p["monoOutput"]         = params.monoOutput;
    p["limiterEnabled"]     = params.limiterEnabled;
    p["limiterCeilingDb"]   = params.limiterCeilingDb;
    p["limiterReleaseMs"]   = params.limiterReleaseMs;
    root["params"] = std::move(p);

    json chainJson = json::array();
    for (const auto& node : chain) {
        chainJson.push_back({{"kind", node.kind},
                             {"bypassed", node.bypassed},
                             {"pluginPath", node.pluginPath},
                             {"pluginUid", node.pluginUid},
                             {"pluginName", node.pluginName},
                             {"pluginState", node.pluginState}});
    }
    root["chain"] = std::move(chainJson);

    std::ofstream file(paths::configFile(), std::ios::trunc);
    if (!file) {
        RV_ERROR("could not write the configuration file");
        return;
    }
    file << root.dump(2);
}

void Config::applyTo(Params& target) const
{
    target.inputGainDb.store(params.inputGainDb);
    target.outputGainDb.store(params.outputGainDb);
    target.mute.store(params.mute);
    target.bypassAll.store(params.bypassAll);
    target.monitorGainDb.store(params.monitorGainDb);
    target.monitorMute.store(params.monitorMute);

    target.denoiseEnabled.store(params.denoiseEnabled);
    target.denoiseAmount.store(params.denoiseAmount);
    target.hpfEnabled.store(params.hpfEnabled);
    target.hpfHz.store(params.hpfHz);
    target.lpfEnabled.store(params.lpfEnabled);
    target.lpfHz.store(params.lpfHz);

    target.eqEnabled.store(params.eqEnabled);
    for (int i = 0; i < kEqBands; ++i) {
        target.eqGainDb[i].store(params.eqGainDb[i]);
        target.eqQ[i].store(params.eqQ[i]);
    }

    target.gateEnabled.store(params.gateEnabled);
    target.gateThresholdDb.store(params.gateThresholdDb);
    target.gateHysteresisDb.store(params.gateHysteresisDb);
    target.gateRangeDb.store(params.gateRangeDb);
    target.gateAttackMs.store(params.gateAttackMs);
    target.gateHoldMs.store(params.gateHoldMs);
    target.gateReleaseMs.store(params.gateReleaseMs);
    target.gateLookaheadMs.store(params.gateLookaheadMs);
    target.gateSidechainHpfHz.store(params.gateSidechainHpfHz);

    target.compEnabled.store(params.compEnabled);
    target.compThresholdDb.store(params.compThresholdDb);
    target.compRatio.store(params.compRatio);
    target.compKneeDb.store(params.compKneeDb);
    target.compAttackMs.store(params.compAttackMs);
    target.compReleaseMs.store(params.compReleaseMs);
    target.compMakeupDb.store(params.compMakeupDb);
    target.compAutoMakeup.store(params.compAutoMakeup);
    target.compLookaheadMs.store(params.compLookaheadMs);
    target.compSidechainHpfHz.store(params.compSidechainHpfHz);
    target.compRmsDetection.store(params.compRmsDetection);

    target.inputMix.store(params.inputMix);
    target.monoOutput.store(params.monoOutput);

    target.limiterEnabled.store(params.limiterEnabled);
    target.limiterCeilingDb.store(params.limiterCeilingDb);
    target.limiterReleaseMs.store(params.limiterReleaseMs);

    target.touch();
}

void Config::captureFrom(const Params& source)
{
    params.inputGainDb  = source.inputGainDb.load();
    params.outputGainDb = source.outputGainDb.load();
    params.mute         = source.mute.load();
    params.bypassAll    = source.bypassAll.load();
    params.monitorGainDb = source.monitorGainDb.load();
    params.monitorMute   = source.monitorMute.load();

    params.denoiseEnabled = source.denoiseEnabled.load();
    params.denoiseAmount  = source.denoiseAmount.load();
    params.hpfEnabled = source.hpfEnabled.load();
    params.hpfHz      = source.hpfHz.load();
    params.lpfEnabled = source.lpfEnabled.load();
    params.lpfHz      = source.lpfHz.load();

    params.eqEnabled = source.eqEnabled.load();
    for (int i = 0; i < kEqBands; ++i) {
        params.eqGainDb[i] = source.eqGainDb[i].load();
        params.eqQ[i]      = source.eqQ[i].load();
    }

    params.gateEnabled        = source.gateEnabled.load();
    params.gateThresholdDb    = source.gateThresholdDb.load();
    params.gateHysteresisDb   = source.gateHysteresisDb.load();
    params.gateRangeDb        = source.gateRangeDb.load();
    params.gateAttackMs       = source.gateAttackMs.load();
    params.gateHoldMs         = source.gateHoldMs.load();
    params.gateReleaseMs      = source.gateReleaseMs.load();
    params.gateLookaheadMs    = source.gateLookaheadMs.load();
    params.gateSidechainHpfHz = source.gateSidechainHpfHz.load();

    params.compEnabled        = source.compEnabled.load();
    params.compThresholdDb    = source.compThresholdDb.load();
    params.compRatio          = source.compRatio.load();
    params.compKneeDb         = source.compKneeDb.load();
    params.compAttackMs       = source.compAttackMs.load();
    params.compReleaseMs      = source.compReleaseMs.load();
    params.compMakeupDb       = source.compMakeupDb.load();
    params.compAutoMakeup     = source.compAutoMakeup.load();
    params.compLookaheadMs    = source.compLookaheadMs.load();
    params.compSidechainHpfHz = source.compSidechainHpfHz.load();
    params.compRmsDetection   = source.compRmsDetection.load();

    params.inputMix   = source.inputMix.load();
    params.monoOutput = source.monoOutput.load();

    params.limiterEnabled   = source.limiterEnabled.load();
    params.limiterCeilingDb = source.limiterCeilingDb.load();
    params.limiterReleaseMs = source.limiterReleaseMs.load();
}

audio::EngineConfig Config::toEngineConfig() const
{
    audio::EngineConfig engine;

    engine.input.deviceId     = input.deviceId;
    engine.input.backend      = input.backend;
    engine.input.wasapiMode   = input.wasapiMode;
    engine.input.sampleRate   = input.sampleRate;
    engine.input.channels     = input.channels;
    engine.input.bufferFrames = input.bufferFrames;

    engine.output.deviceId     = output.deviceId;
    engine.output.backend      = output.backend;
    engine.output.wasapiMode   = output.wasapiMode;
    engine.output.sampleRate   = output.sampleRate;
    engine.output.channels     = output.channels;
    engine.output.bufferFrames = output.bufferFrames;

    engine.monitor.deviceId     = monitor.deviceId;
    engine.monitor.backend      = monitor.backend;
    engine.monitor.wasapiMode   = monitor.wasapiMode;
    engine.monitor.sampleRate   = monitor.sampleRate;
    engine.monitor.channels     = monitor.channels;
    engine.monitor.bufferFrames = monitor.bufferFrames;
    engine.monitorEnabled       = monitorEnabled;

    engine.internalChannels = internalChannels;
    engine.maxBlockFrames   = maxBlockFrames;

    return engine;
}

} // namespace rv::app
