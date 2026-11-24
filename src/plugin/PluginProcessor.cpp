/*
    PluginProcessor.cpp - JUCE plugin bridge using ParameterManager.
*/

#include "PluginProcessor.h"
#include "Config.h"
#include "DebugLog.h"
#include "KitScanner.h"
#include "PluginEditor.h"

namespace glorp
{

//==============================================================================
std::optional<juce::String> PluginProcessor::getNameForMidiNoteNumber(int note, int midiChannel)
{
    juce::ignoreUnused(midiChannel);

    if (note < 0 || note > 127)
        return std::nullopt;

    const int instrument = getMidiMap().instrumentForNote(note);

    if (instrument < 0 || instrument >= audioEngine.getSampler().numInstruments())
        return std::nullopt;

    auto name = audioEngine.getSampler().instrumentName(instrument);

    if (name.isEmpty())
        return std::nullopt;

    return name;
}

namespace
{
juce::String normaliseRoutingName(const juce::String &name)
{
    return name.toLowerCase().removeCharacters(" .-_/\\");
}

bool micMatchesInstrument(const juce::String &micName, const juce::String &instrumentName)
{
    const auto mic = normaliseRoutingName(micName);
    const auto inst = normaliseRoutingName(instrumentName);

    if (mic.isEmpty() || inst.isEmpty())
        return false;

    if (inst.contains(mic) || mic.contains(inst))
        return true;

    if (mic.contains("kick") || mic.contains("kdrum") || mic.contains("bdrum"))
        return inst.contains("kick") || inst.contains("kdrum") || inst.contains("bdrum");

    if (mic.contains("snare"))
        return inst.contains("snare") || inst.contains("rim") || inst.contains("click") || inst.contains("cross")
               || inst.contains("stick");

    if (mic.contains("hat") || mic == "hh")
        return inst.contains("hat") || inst.contains("hh");

    for (int n = 1; n <= 4; ++n)
    {
        const auto key = "tom" + juce::String(n);

        if (mic.contains(key))
            return inst.contains(key);
    }

    if (mic.contains("floor") || mic.contains("ftom"))
        return inst.contains("floor") || inst.contains("ftom") || inst.contains("tom4");

    if (mic.contains("ride"))
        return inst.contains("ride");
    if (mic.contains("crash"))
        return inst.contains("crash");
    if (mic.contains("china"))
        return inst.contains("china");
    if (mic.contains("splash"))
        return inst.contains("splash");

    return false;
}
} // namespace

//==============================================================================
juce::AudioProcessor::BusesProperties PluginProcessor::createBusesProperties()
{
    BusesProperties bp;
    for (int i = 0; i < maxOutputBuses; ++i)
    {
        const auto busName = (i == 0) ? juce::String("Out 1/2 (Main)")
                                      : ("Out " + juce::String(i * 2 + 1) + "/" + juce::String(i * 2 + 2));
        bp = bp.withOutput(busName, juce::AudioChannelSet::stereo(), true);
    }
    return bp;
}

//==============================================================================
PluginProcessor::PluginProcessor() : juce::AudioProcessor(createBusesProperties())
{
    DebugLog::instance().setLogFile(settings.getConfigDirectory().getChildFile("suitedrumsglorp_debug.log"));
    DebugLog::instance().logInfo("APP",
                                 "PluginProcessor init (Plugin, OS="
                                     + juce::SystemStats::getOperatingSystemName() + ")");

    parameterManager.createParameterLayout(*this);

    midiRouter.onMidiMapChanged = [this] { listeners.call(&Listener::parametersChanged); };

    audioEngine.getSampler().setSampleRateAndBlock(44100.0, 512);
    audioEngine.getSampler().setMultiOutMode(true);
    refreshKitList();

    if (settings.lastKitPath.isNotEmpty() && juce::File(settings.lastKitPath).existsAsFile())
    {
        loadKit(settings.lastKitPath);
    }
    else if (!kitList.empty())
    {
        for (const auto &entry : kitList)
        {
            if (entry.valid)
            {
                loadKit(juce::String(entry.drumkitPath));
                break;
            }
        }
    }

    if (!audioEngine.getSampler().isKitLoaded())
    {
        const auto homeDemo = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                  .getChildFile(config::CONFIG_DIR_NAME)
                                  .getChildFile("kits/GlorpDemoKit/drumkit.xml");
        const auto legacyDemo = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                                    .getChildFile(".config/GlorpDrums/kits/GlorpDemoKit/drumkit.xml");
        if (homeDemo.existsAsFile())
        {
            loadKit(homeDemo.getFullPathName());
        }
        else if (legacyDemo.existsAsFile())
        {
            loadKit(legacyDemo.getFullPathName());
        }
        else
        {
            const auto localDemo =
                juce::File::getCurrentWorkingDirectory().getChildFile("test/kits/GlorpDemoKit/drumkit.xml");
            if (localDemo.existsAsFile())
                loadKit(localDemo.getFullPathName());
        }
    }
}

PluginProcessor::~PluginProcessor()
{
    settings.saveIfChanged();
}

//==============================================================================
void PluginProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    audioEngine.prepareToPlay(sampleRate, samplesPerBlock);
}

void PluginProcessor::releaseResources()
{
    audioEngine.releaseResources();
}

void PluginProcessor::sampleRateChanged(double newRate)
{
    audioEngine.sampleRateChanged(newRate);
}

void PluginProcessor::blockSizeChanged(int)
{
    audioEngine.blockSizeChanged(0); // blockSize parameter not used in current implementation
}

double PluginProcessor::getTailLengthSeconds() const
{
    // Long enough for a ring-out to finish once the tuner has flushed.
    return 4.0;
}

bool PluginProcessor::isBusesLayoutSupported(const BusesLayout &layouts) const
{
    // Suite Drums Glorp has no audio inputs
    if (!layouts.getMainInputChannelSet().isDisabled())
        return false;

    // Main output bus (Bus 1) must be stereo
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // Additional output buses (Buses 2..16) must be stereo or disabled
    for (int i = 1; i < layouts.outputBuses.size(); ++i)
    {
        const auto &set = layouts.outputBuses[i];
        if (!set.isDisabled() && set != juce::AudioChannelSet::stereo())
            return false;
    }

    return true;
}

double PluginProcessor::getCurrentHostBpm() const
{
    if (auto *playHead = getPlayHead())
    {
        if (const auto position = playHead->getPosition())
        {
            if (const auto bpm = position->getBpm(); bpm.hasValue() && *bpm > 1.0)
                return *bpm;
        }
    }

    return hostBpm.load(std::memory_order_relaxed);
}

double PluginProcessor::getCurrentHostPpqPosition() const
{
    if (auto *playHead = getPlayHead())
    {
        if (const auto position = playHead->getPosition())
        {
            if (const auto ppq = position->getPpqPosition(); ppq.hasValue())
                return *ppq;
        }
    }

    return hostPpqPosition.load(std::memory_order_relaxed);
}

bool PluginProcessor::getCurrentHostIsPlaying() const
{
    if (auto *playHead = getPlayHead())
    {
        if (const auto position = playHead->getPosition())
            return position->getIsPlaying();
    }

    return hostIsPlaying.load(std::memory_order_relaxed);
}

//==============================================================================
void PluginProcessor::processBlock(juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midi)
{
    juce::ScopedNoDenormals noDenormals;

    if (auto *playHead = getPlayHead())
    {
        if (const auto position = playHead->getPosition())
        {
            if (const auto bpm = position->getBpm(); bpm.hasValue() && *bpm > 1.0)
                hostBpm.store(*bpm, std::memory_order_relaxed);

            if (const auto ppq = position->getPpqPosition(); ppq.hasValue())
                hostPpqPosition.store(*ppq, std::memory_order_relaxed);

            hostIsPlaying.store(position->getIsPlaying(), std::memory_order_relaxed);
        }
    }

    const auto numSamples = buffer.getNumSamples();

    // Map host output buses to up to 16 stereo bus write pointers (32 channels)
    std::array<float *, maxOutputBuses * 2> busOutputs {};
    busOutputs.fill(nullptr);

    const int busCount = getBusCount(false);
    for (int b = 0; b < maxOutputBuses && b < busCount; ++b)
    {
        auto busBuffer = getBusBuffer(buffer, false, b);
        if (busBuffer.getNumChannels() >= 2)
        {
            busOutputs[static_cast<size_t>(b * 2)] = busBuffer.getWritePointer(0);
            busOutputs[static_cast<size_t>(b * 2 + 1)] = busBuffer.getWritePointer(1);
        }
    }

    // Fallback for flat buffers or unit tests where buses aren't configured in the host
    if (busOutputs[0] == nullptr && buffer.getNumChannels() >= 2)
    {
        const int available = std::min(buffer.getNumChannels(), maxOutputBuses * 2);
        for (int c = 0; c < available; ++c)
            busOutputs[static_cast<size_t>(c)] = buffer.getWritePointer(c);
    }

    // Delegate core audio processing to AudioEngine (parameters, MIDI, sampler rendering)
    audioEngine.render(busOutputs.data(),
                       maxOutputBuses * 2,
                       numSamples,
                       midi,
                       parameterManager,
                       midiRouter,
                       lastHiHatCC4,
                       lastSnareCC16);

    // Update latency if needed
    const int samplerLatency = audioEngine.getSampler().latencySamples();
    if (getLatencySamples() != samplerLatency)
        setLatencySamples(samplerLatency);

}

//==============================================================================
juce::String PluginProcessor::controlIdForInstrument(const juce::String &suffix, int index)
{
    return ParameterManager::controlIdForInstrument(suffix, index);
}

juce::AudioProcessorParameter *PluginProcessor::getParameterForMidiControl(int cc, int channel)
{
    // Only note input for a drum plugin: CCs are routed through the learn map
    // instead, because the same CC may drive several instruments.
    juce::ignoreUnused(cc, channel);
    return nullptr;
}

juce::AudioProcessorParameter *PluginProcessor::findParameter(const juce::String &paramID) const
{
    for (auto *p : getParameters())
        if (auto *pWithId = dynamic_cast<juce::AudioProcessorParameterWithID *>(p))
            if (pWithId->paramID == paramID)
                return p;

    return nullptr;
}

//==============================================================================
void PluginProcessor::refreshKitList()
{
    kitList = KitScanner::scan(settings.allSearchPaths());
}

void PluginProcessor::loadKit(const juce::String &drumkitPath)
{
    try
    {
        loadKitImpl(drumkitPath);
    }
    catch (const std::exception &e)
    {
        lastKitError = juce::String("Could not load kit: ") + e.what();
        audioEngine.getSampler().unloadKit();
        currentKit = {};
        currentKitPath.clear();
        DebugLog::instance().logInfo("KIT", lastKitError);
    }
    catch (...)
    {
        lastKitError = "Could not load kit: unknown error";
        audioEngine.getSampler().unloadKit();
        currentKit = {};
        currentKitPath.clear();
        DebugLog::instance().logInfo("KIT", lastKitError);
    }
}

void PluginProcessor::loadKitImpl(const juce::String &drumkitPath)
{
    lastKitError.clear();

    if (drumkitPath.isEmpty())
        return;

    currentKit = DrumKit::load(drumkitPath.toStdString());

    if (!currentKit.error.empty())
        lastKitError = juce::String(currentKit.error);
    else if (!currentKit.warnings.empty())
        lastKitError = juce::String(currentKit.warnings.front());

    // Keep the suggested map before the kit is passed to the sampler.
    const auto suggestedMap = currentKit.suggestedMap;

    juce::String error;

    if (!audioEngine.getSampler().loadKit(currentKit, error))
    {
        lastKitError = error;
        DebugLog::instance().logInfo("KIT", "Failed to load kit '" + drumkitPath + "': " + error);
        audioEngine.getSampler().unloadKit();
        currentKit = {};
        return;
    }

    currentKitPath = drumkitPath;
    settings.pushRecentKit(drumkitPath);
    settings.saveIfChanged();
    audioEngine.invalidateParamCache();

    DebugLog::instance().logInfo("KIT",
                                 "Loaded kit '" + drumkitPath + "' ("
                                     + juce::String(audioEngine.getSampler().numInstruments()) + " instruments)");

    // Delegate note mapping to MidiRouter (handles kit midimap, GM fallback, and default assignments)
    midiRouter.setupNoteMappingFromKit(currentKit, audioEngine.getSampler());

    // Preserve the user's selected MIDI map preset (e.g. "Custom" or e-drum preset) across restarts / kit switches
    if (settings.lastMapPreset.isNotEmpty())
    {
        for (const auto &preset : MidiMap::availablePresets())
        {
            if (preset.name == settings.lastMapPreset)
            {
                midiRouter.applyMapPreset(settings.lastMapPreset, audioEngine.getSampler());
                break;
            }
        }
    }

    // Reset per-instrument controls for the new kit.
    audioEngine.getSampler().resetInstrumentParams();

    for (int i = 0; i < maxSupportedInstruments; ++i)
    {
        float levelNorm = 0.5f; // default 1.0f (0.5 * 2.0f)
        float panNorm = 0.5f;   // default 0.0f (0.5 * 2.0f - 1.0f)
        int busIdx = (i < audioEngine.getSampler().numInstruments()) ? audioEngine.getSampler().getInstrumentBus(i)
                                                                     : std::min(i, 15);

        if (currentKit.isNativeGlorpKit && i < static_cast<int>(currentKit.instruments.size()))
        {
            const auto &inst = currentKit.instruments[static_cast<size_t>(i)];
            busIdx = std::clamp(inst.outputBus, 0, 15);

            auto itL = currentKit.factoryInstrumentLevels.find(inst.logicalName);
            if (itL == currentKit.factoryInstrumentLevels.end())
                itL = currentKit.factoryInstrumentLevels.find(inst.name);
            if (itL != currentKit.factoryInstrumentLevels.end())
                levelNorm = std::clamp(itL->second / 2.0f, 0.0f, 1.0f);

            auto itP = currentKit.factoryInstrumentPans.find(inst.logicalName);
            if (itP == currentKit.factoryInstrumentPans.end())
                itP = currentKit.factoryInstrumentPans.find(inst.name);
            if (itP != currentKit.factoryInstrumentPans.end())
                panNorm = std::clamp((itP->second + 1.0f) * 0.5f, 0.0f, 1.0f);
        }

        if (i < static_cast<int>(parameterManager.instrumentLevelParams.size()))
            parameterManager.instrumentLevelParams[static_cast<size_t>(i)]->setValueNotifyingHost(levelNorm);
        if (i < static_cast<int>(parameterManager.instrumentPanParams.size()))
            parameterManager.instrumentPanParams[static_cast<size_t>(i)]->setValueNotifyingHost(panNorm);
        if (i < static_cast<int>(parameterManager.instrumentPitchParams.size()))
            parameterManager.instrumentPitchParams[static_cast<size_t>(i)]->setValueNotifyingHost(0.5f);
        if (i < static_cast<int>(parameterManager.instrumentDynamicsParams.size()))
            parameterManager.instrumentDynamicsParams[static_cast<size_t>(i)]->setValueNotifyingHost(0.5f);
        if (i < static_cast<int>(parameterManager.instrumentBusParams.size()))
            parameterManager.instrumentBusParams[static_cast<size_t>(i)]->setValueNotifyingHost(busIdx / 15.0f);
    }

    listeners.call(&Listener::kitChanged);
}

void PluginProcessor::setMicOutputBus(int micStripIndex, int bus)
{
    bus = juce::jlimit(0, 15, bus);

    if (micStripIndex < 0 || micStripIndex >= audioEngine.getSampler().numMicStrips())
        return;

    audioEngine.getSampler().setMicOutputBus(micStripIndex, bus);

    const auto micName = audioEngine.getSampler().micStripName(micStripIndex);
    bool changed = false;

    for (int i = 0; i < audioEngine.getSampler().numInstruments(); ++i)
    {
        if (!micMatchesInstrument(micName, audioEngine.getSampler().instrumentName(i)))
            continue;

        audioEngine.getSampler().setInstrumentBus(i, bus);

        if (i < static_cast<int>(parameterManager.instrumentBusParams.size()))
            parameterManager.instrumentBusParams[static_cast<size_t>(i)]->setValueNotifyingHost(
                static_cast<float>(bus) / 15.0f);

        changed = true;
    }

    if (changed)
        listeners.call(&Listener::busRoutingChanged);
}

void PluginProcessor::setInstrumentOutputBus(int instrumentIndex, int bus)
{
    bus = juce::jlimit(0, 15, bus);

    if (instrumentIndex < 0 || instrumentIndex >= audioEngine.getSampler().numInstruments())
        return;

    audioEngine.getSampler().setInstrumentBus(instrumentIndex, bus);

    if (instrumentIndex < static_cast<int>(parameterManager.instrumentBusParams.size()))
        parameterManager.instrumentBusParams[static_cast<size_t>(instrumentIndex)]->setValueNotifyingHost(
            static_cast<float>(bus) / 15.0f);

    const auto instrumentName = audioEngine.getSampler().instrumentName(instrumentIndex);

    for (int i = 0; i < audioEngine.getSampler().numMicStrips(); ++i)
        if (micMatchesInstrument(audioEngine.getSampler().micStripName(i), instrumentName))
            audioEngine.getSampler().setMicOutputBus(i, bus);

    listeners.call(&Listener::busRoutingChanged);
}

void PluginProcessor::applyMapPreset(const juce::String &presetName)
{
    midiRouter.applyMapPreset(presetName, audioEngine.getSampler());

    DebugLog::instance().logInfo("MIDI",
                                 "Applied map preset '" + presetName + "' (mapped notes="
                                     + juce::String(static_cast<int>(midiRouter.getMidiMap().noteAssignments().size()))
                                     + ")");

    settings.lastMapPreset = presetName;
    settings.markDirty();
    settings.saveIfChanged();
    notifyParametersChanged();
}

juce::String PluginProcessor::saveCustomMapPreset(const juce::String &presetName)
{
    auto trimmed = presetName.trim();
    if (trimmed.isEmpty())
        trimmed = "Custom";

    const juce::String canonicalName = trimmed.equalsIgnoreCase("Custom") ? juce::String("Custom") : trimmed;

    std::vector<juce::String> names;
    const int numInst = audioEngine.getSampler().numInstruments();
    names.reserve(static_cast<size_t>(numInst));
    for (int i = 0; i < numInst; ++i)
        names.push_back(audioEngine.getSampler().instrumentName(i));

    juce::File savedFile;
    if (!getMidiMap().saveUserPreset(canonicalName, names, &savedFile))
        return {};

    DebugLog::instance().logInfo("MIDI", "Saved map preset '" + canonicalName + "' -> " + savedFile.getFullPathName());

    settings.lastMapPreset = canonicalName;
    settings.markDirty();
    settings.saveIfChanged();
    notifyParametersChanged();
    return savedFile.getFullPathName();
}

juce::String PluginProcessor::suggestedPresetSaveName() const
{
    const auto &last = settings.lastMapPreset;
    if (last.isEmpty() || last.equalsIgnoreCase("Custom"))
        return "My Pad Map";

    for (const auto &p : MidiMap::availablePresets())
    {
        if (p.name == last && p.isBuiltIn)
            return "My Pad Map";
    }

    return last;
}

void PluginProcessor::notifyParametersChanged()
{
    listeners.call(&Listener::parametersChanged);
}

//==============================================================================
void PluginProcessor::setControlValue(const juce::String &controlId, float normalised)
{
    parameterManager.setMasterParameterValue(controlId, normalised);

    int index = 0;
    juce::String field;

    if (parameterManager.parseControlId(controlId, index, field))
        parameterManager.setInstrumentParameterValue(index, field, normalised);
}

void PluginProcessor::drainControlQueue()
{
    parameterManager.drainControlQueue();
}

//==============================================================================
void PluginProcessor::getStateInformation(juce::MemoryBlock &destination)
{
    juce::XmlElement state("SuiteDrumsGlorpState");
    state.setAttribute("version", 1);
    state.setAttribute("kit", currentKitPath);
    state.setAttribute("hiHatMode", static_cast<int>(audioEngine.getSampler().getHiHatMode()));

    const auto mixerPreset = audioEngine.getSampler().getCurrentMixerPreset("CurrentMix");
    if (auto mixerXml = juce::XmlDocument::parse(mixerPreset.toXmlString()))
    {
        mixerXml->setTagName("MixerState");
        state.addChildElement(mixerXml.release());
    }

    audioEngine.getSampler().getBleedMatrix().writeXml(state);

    auto *parameters = state.createNewChildElement("Parameters");

    for (const auto *parameter : getParameters())
    {
        if (auto *pWithId = dynamic_cast<const juce::AudioProcessorParameterWithID *>(parameter))
        {
            auto *p = parameters->createNewChildElement("P");
            p->setAttribute("id", pWithId->paramID);
            p->setAttribute("v", parameter->getValue());
        }
    }

    auto *curves = state.createNewChildElement("VelocityCurves");

    for (int i = 0; i < audioEngine.getSampler().numInstruments(); ++i)
    {
        const auto params = audioEngine.getSampler().getInstrumentParams(i);

        if (!params.useCustomVelocityCurve)
            continue;

        auto *curve = curves->createNewChildElement("curve");
        curve->setAttribute("instrument", i);

        for (size_t point = 0; point < params.customVelocityCurve.size(); ++point)
            curve->setAttribute("p" + juce::String(static_cast<int>(point)),
                                static_cast<double>(params.customVelocityCurve[point]));
    }

    auto *tweaks = state.createNewChildElement("InstrumentTweaks");

    for (int i = 0; i < audioEngine.getSampler().numInstruments(); ++i)
    {
        const auto params = audioEngine.getSampler().getInstrumentParams(i);

        if (std::abs(params.attackAmount) <= 0.001f && params.attackMs <= 0.001f && params.damping <= 0.001f)
            continue;

        auto *tweak = tweaks->createNewChildElement("instrument");
        tweak->setAttribute("index", i);
        tweak->setAttribute("attackAmount", static_cast<double>(params.attackAmount));
        tweak->setAttribute("attackMs", static_cast<double>(params.attackMs));
        tweak->setAttribute("damping", static_cast<double>(params.damping));
    }

    if (auto *map = getMidiMap().exportXml())
    {
        map->setTagName("MidiMap");
        state.addChildElement(map);
    }

    if (auto *layers = state.createNewChildElement("UserLayers"))
    {
        for (int slot = 0; slot < 2; ++slot)
        {
            if (!audioEngine.getSampler().isUserLayerLoaded(slot)
                && !audioEngine.getSampler().isUserLayerMissing(slot))
                continue;

            const auto params = audioEngine.getSampler().getUserLayerParams(slot);
            auto *layer = layers->createNewChildElement("Layer");
            layer->setAttribute("slot", slot);
            layer->setAttribute("path", audioEngine.getSampler().getUserLayerPath(slot));
            layer->setAttribute("blend", static_cast<double>(params.blend));
            layer->setAttribute("attackMs", static_cast<double>(params.attackMs));
            layer->setAttribute("decayMs", static_cast<double>(params.decayMs));
            layer->setAttribute("sustain", static_cast<double>(params.sustain));
            layer->setAttribute("releaseMs", static_cast<double>(params.releaseMs));
            layer->setAttribute("invert", params.polarityInvert ? 1 : 0);
            layer->setAttribute("delayMs", static_cast<double>(params.delayMs));
            layer->setAttribute("trimMs", static_cast<double>(params.trimMs));
            layer->setAttribute("level", static_cast<double>(params.level));
            layer->setAttribute("pan", static_cast<double>(params.pan));
        }
    }

    copyXmlToBinary(state, destination);
}

void PluginProcessor::setStateInformation(const void *data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;

    auto xml = getXmlFromBinary(data, sizeInBytes);

    if (xml == nullptr
        || (!xml->hasTagName("SuiteDrumsGlorpState") && !xml->hasTagName("GlorpDrumsState")))
        return;

    auto kitPath = xml->getStringAttribute("kit");

    if (xml->hasAttribute("hiHatMode"))
    {
        const int modeInt = xml->getIntAttribute("hiHatMode", 0);
        audioEngine.getSampler().setHiHatMode(modeInt == 1 ? HiHatMode::discreteNotes : HiHatMode::continuousCC4);
    }

    MixerPreset storedMixer;
    bool hasStoredMixer = false;

    if (auto *mixerXml = xml->getChildByName("MixerState"))
        hasStoredMixer = MixerPreset::fromXmlString(mixerXml->toString(), storedMixer);

    audioEngine.getSampler().getBleedMatrix().readXml(*xml);

    if (auto *parameters = xml->getChildByName("Parameters"))
    {
        for (auto *p : parameters->getChildIterator())
        {
            const auto id = p->getStringAttribute("id");

            if (auto *parameter = findParameter(id))
                parameter->setValueNotifyingHost(
                    juce::jlimit(0.0f, 1.0f, static_cast<float>(p->getDoubleAttribute("v", 0.0))));
        }
    }

    struct StoredCurve
    {
        int instrument = -1;
        std::array<float, 8> points {};
    };

    std::vector<StoredCurve> storedCurves;

    if (auto *curves = xml->getChildByName("VelocityCurves"))
    {
        for (auto *curve : curves->getChildIterator())
        {
            StoredCurve stored;
            stored.instrument = curve->getIntAttribute("instrument", -1);

            if (stored.instrument < 0)
                continue;

            for (size_t point = 0; point < stored.points.size(); ++point)
                stored.points[point] = static_cast<float>(
                    curve->getDoubleAttribute("p" + juce::String(static_cast<int>(point)), 0.0));

            storedCurves.push_back(stored);
        }
    }

    if (kitPath.isNotEmpty() && kitPath != currentKitPath)
    {
        loadKit(kitPath);
        if (auto *map = xml->getChildByName("MidiMap"))
            getMidiMap().importXml(*map);
        listeners.call(&Listener::kitChanged);
        listeners.call(&Listener::parametersChanged);
    }
    else
    {
        if (auto *map = xml->getChildByName("MidiMap"))
            getMidiMap().importXml(*map);
        listeners.call(&Listener::kitChanged);
        listeners.call(&Listener::parametersChanged);
    }

    for (const auto &stored : storedCurves)
    {
        if (stored.instrument >= audioEngine.getSampler().numInstruments())
            continue;

        auto params = audioEngine.getSampler().getInstrumentParams(stored.instrument);
        params.useCustomVelocityCurve = true;
        params.customVelocityCurve = stored.points;
        audioEngine.getSampler().setInstrumentParams(stored.instrument, params);
    }

    if (auto *tweaks = xml->getChildByName("InstrumentTweaks"))
    {
        for (auto *tweak : tweaks->getChildIterator())
        {
            const int index = tweak->getIntAttribute("index", -1);
            if (index < 0 || index >= audioEngine.getSampler().numInstruments())
                continue;

            auto params = audioEngine.getSampler().getInstrumentParams(index);
            const auto legacyAttackMs = static_cast<float>(tweak->getDoubleAttribute("attackMs", 0.0));

            if (tweak->hasAttribute("attackAmount"))
            {
                params.attackAmount = static_cast<float>(tweak->getDoubleAttribute("attackAmount", 0.0));
                params.attackMs = 0.0f;
            }
            else
            {
                // Alpha 37 exposed a fade-in attack as attackMs. Map it to the
                // new bipolar control as a softened attack instead of leaving a
                // hidden fade applied to the piece.
                params.attackAmount = juce::jlimit(-1.0f, 1.0f, -legacyAttackMs / 200.0f);
                params.attackMs = 0.0f;
            }

            params.damping = static_cast<float>(tweak->getDoubleAttribute("damping", 0.0));
            audioEngine.getSampler().setInstrumentParams(index, params);
        }
    }

    bool layersRestored = false;

    if (auto *layers = xml->getChildByName("UserLayers"))
    {
        const auto layerDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                  .getChildFile(config::CONFIG_DIR_NAME)
                                  .getChildFile("user_layers");

        for (auto *layer : layers->getChildIterator())
        {
            const int slot = layer->getIntAttribute("slot", -1);
            if (slot < 0 || slot > 1)
                continue;

            auto file = juce::File(layer->getStringAttribute("path"));
            if (!file.existsAsFile())
                file = layerDir.getChildFile(file.getFileName());

            UserLayerParams params;
            params.blend = static_cast<float>(layer->getDoubleAttribute("blend", 0.5));
            params.attackMs = static_cast<float>(layer->getDoubleAttribute("attackMs", 0.0));
            params.decayMs = static_cast<float>(layer->getDoubleAttribute("decayMs", 120.0));
            params.sustain = static_cast<float>(layer->getDoubleAttribute("sustain", 1.0));
            params.releaseMs = static_cast<float>(layer->getDoubleAttribute("releaseMs", 80.0));
            params.polarityInvert = layer->getIntAttribute("invert", 0) != 0;
            params.delayMs = static_cast<float>(layer->getDoubleAttribute("delayMs", 0.0));
            params.trimMs = static_cast<float>(layer->getDoubleAttribute("trimMs", 0.0));
            params.level = static_cast<float>(layer->getDoubleAttribute("level", 1.0));
            params.pan = static_cast<float>(layer->getDoubleAttribute("pan", 0.0));

            if (!file.existsAsFile())
            {
                audioEngine.getSampler().markUserLayerMissing(slot, file.getFullPathName(), params);
                layersRestored = true;
                continue;
            }

            juce::String error;
            if (audioEngine.getSampler().loadUserLayer(slot, file.getFullPathName(), params, error))
                layersRestored = true;
        }
    }

    // Layer strips are appended after the kit mics, so the mixer state has to
    // be applied after the layers are restored to reach their channels.
    if (hasStoredMixer)
        audioEngine.getSampler().applyMixerPreset(storedMixer);

    if (hasStoredMixer || layersRestored)
    {
        listeners.call(&Listener::kitChanged);
        listeners.call(&Listener::parametersChanged);
    }
}

//==============================================================================
juce::AudioProcessorEditor *PluginProcessor::createEditor()
{
    return new PluginEditor(*this);
}

} // namespace glorp

//==============================================================================
// Global filter creator
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter()
{
    return new glorp::PluginProcessor();
}