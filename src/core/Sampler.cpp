#include "Sampler.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace glorp
{

namespace
{
constexpr double pi = 3.14159265358979323846;

/// Equal-power pan gains for a -1..+1 position.
void panGains(float pan, float &left, float &right) noexcept
{
    const auto p = std::clamp(static_cast<double>((std::clamp(pan, -1.0f, 1.0f) + 1.0f) * 0.25f) * pi, 0.0, 0.5 * pi);
    left = static_cast<float>(std::cos(p));
    right = static_cast<float>(std::sin(p));
}

/// Builds a std::atomic-friendly next-sample-clock value. Guarded by callers
/// being on the audio thread only.
inline float velocityWithCurve(float velocity, float curve) noexcept
{
    const auto v = std::clamp(velocity, 0.0f, 1.0f);
    return std::pow(v, std::max(0.05f, curve));
}

float evaluateVelocityPoints(const std::array<float, 8> &points, float x) noexcept
{
    x = std::clamp(x, 0.0f, 1.0f);
    const auto scaled = x * static_cast<float>(points.size() - 1);
    const auto index = static_cast<int>(scaled);
    const auto frac = scaled - static_cast<float>(index);
    const auto a = points[static_cast<size_t>(std::clamp(index, 0, 7))];
    const auto b = points[static_cast<size_t>(std::clamp(index + 1, 0, 7))];
    return std::clamp(a + (b - a) * frac, 0.0f, 1.0f);
}
} // namespace

//==============================================================================
//==============================================================================
// Varispeed tuning introduces zero latency — no window, no lookahead.

namespace
{
/// Releases the kit lock however the enclosing scope is left.
struct KitUnlock
{
    juce::CriticalSection &lock;
    explicit KitUnlock(juce::CriticalSection &l) : lock(l) {}
    ~KitUnlock() { lock.exit(); }
};
} // namespace

Sampler::Sampler()
{
    for (int i = 0; i < maxVoices; ++i)
        pool.voices.push_back(std::make_unique<Voice>());

    setSampleRateAndBlock(44100.0, 512);
}

Sampler::~Sampler() = default;

void Sampler::setSampleRateAndBlock(double rate, int blockSize)
{
    const juce::ScopedLock lock(kitLock);

    currentSampleRate = rate > 0.0 ? rate : 44100.0;
    currentBlockSize = std::max(4096, blockSize);

    // Preallocate voice scratch buffers to max block size to avoid audio-thread allocation
    for (auto &v : pool.voices)
    {
        v->mixL.resize(static_cast<size_t>(currentBlockSize));
        v->mixR.resize(static_cast<size_t>(currentBlockSize));
    }

    micScratchL.resize(32);
    micScratchR.resize(32);
    for (size_t i = 0; i < 32; ++i)
    {
        micScratchL[i].resize(static_cast<size_t>(currentBlockSize));
        micScratchR[i].resize(static_cast<size_t>(currentBlockSize));
    }
    reverbAccumL.resize(static_cast<size_t>(currentBlockSize));
    reverbAccumR.resize(static_cast<size_t>(currentBlockSize));

    masterFX.prepare(currentSampleRate, currentBlockSize);
    crosstalk.prepare(currentSampleRate, currentBlockSize);
    for (auto &ch : micChannels)
        ch->fx.prepare(currentSampleRate, currentBlockSize);
}

//==============================================================================
bool Sampler::loadKit(DrumKit kit, juce::String &error)
{
    const juce::ScopedLock lock(kitLock);

    kitLoaded.store(false);
    allNotesOff();

    if (kit.instruments.empty())
    {
        error = kit.error.empty() ? juce::String("the kit has no playable instruments") : juce::String(kit.error);
        return false;
    }

    if (static_cast<int>(kit.instruments.size()) > config::MAX_INSTRUMENT_BUSES)
    {
        // Not a hard failure. Big kits are normal - CrocellKit_full alone has 27
        // instruments - and refusing to play it would be worse than losing a bit of
        // per-instrument separation on the extras. Everything past the last bus
        // shares that bus, and the user is told.
        const auto overflow = static_cast<int>(kit.instruments.size()) - config::MAX_INSTRUMENT_BUSES;
        kit.warnings.push_back("This kit has " + std::to_string(kit.instruments.size())
                               + " instruments but there are only " + std::to_string(config::MAX_INSTRUMENT_BUSES)
                               + " output buses, so the last " + std::to_string(overflow)
                               + " instruments share a bus. Rebuild with a higher "
                                 "-Dconfig::MAX_INSTRUMENT_BUSES for full separation.");
    }

    std::vector<InstrumentRuntime> resolved;
    resolved.reserve(kit.instruments.size());

    int missingFiles = 0;

    for (const auto &instrument : kit.instruments)
    {
        InstrumentRuntime runtime;
        resolveInstrumentChannels(kit, instrument, runtime);

        if (runtime.hits.empty())
            ++missingFiles;

        resolved.push_back(std::move(runtime));
    }

    for (size_t i = 0; i < kit.instruments.size() && i < resolved.size(); ++i)
    {
        for (const auto &chokeSpec : kit.instruments[i].chokes)
        {
            const auto targetStr = juce::String(chokeSpec.instrument);
            for (size_t targetIdx = 0; targetIdx < resolved.size(); ++targetIdx)
            {
                if (resolved[targetIdx].name.equalsIgnoreCase(targetStr))
                {
                    resolved[i].chokeTargets.push_back(
                        {static_cast<int>(targetIdx), chokeSpec.hasChokeTime ? chokeSpec.chokeTimeMs : 40.0f});
                }
            }
        }
    }

    if (resolved.empty())
    {
        error = "None of this kit's sample files could be read. Check the kit directory is still in place.";
        return false;
    }

    instruments_ = std::move(resolved);
    kitChannels_ = kit.channels;
    instrumentCount = static_cast<int>(instruments_.size());
    kitSampleRate = kit.declaredSampleRate > 0 ? static_cast<double>(kit.declaredSampleRate) : currentSampleRate;

    // The number of buses in use is bounded by what was built; anything beyond
    // that shares the last bus rather than being dropped.
    busCount = std::min(instrumentCount, config::MAX_INSTRUMENT_BUSES);
    kitTitle = kit.title;

    channelParams.assign(static_cast<size_t>(instrumentCount), ChannelParams {});
    roundRobinCursor.assign(static_cast<size_t>(instrumentCount), 0);
    instrumentBuses.assign(static_cast<size_t>(instrumentCount), 0);

    // Identify hi-hat articulation indices for continuous pedal control
    hiHat = {};
    cc4Active.store(false);
    for (int i = 0; i < instrumentCount; ++i)
    {
        const auto n = instruments_[static_cast<size_t>(i)].name.toLowerCase();
        if (n.contains("hat") || n.contains("hh")
            || instruments_[static_cast<size_t>(i)].group.equalsIgnoreCase("hihat"))
        {
            const bool isSplash = n.contains("splash") || n.contains("spl");
            const bool isPedal = n.contains("pedal") || n.contains("foot");

            if (! isSplash && ! isPedal && (n.contains("open") || n.contains("semi") || n.contains("swish")))
                hiHat.openArticulations.push_back(i);

            if (isSplash)
                hiHat.splash = i;
            else if (isPedal)
                hiHat.pedal = i;
            else if (n.contains("open1") || n.contains("op1") || n.contains("op 1"))
                hiHat.open1 = i;
            else if (n.contains("open2") || n.contains("op2") || n.contains("op 2"))
                hiHat.open2 = i;
            else if (n.contains("open3") || n.contains("op3") || n.contains("op 3"))
                hiHat.open3 = i;
            else if (n.contains("open4") || n.contains("op4") || n.contains("op 4"))
                hiHat.open4 = i;
            else if (n.contains("semi"))
                hiHat.semiOpen = i;
            else if (n.contains("closed") || n.contains("close") || n.contains("clo"))
            {
                if (n.contains("edge") || n.contains("edg"))
                    hiHat.closedEdge = i;
                else
                    hiHat.closed = i;
            }
            else if (n.contains("open") || n.contains("op"))
            {
                if (n.contains("edge") || n.contains("edg"))
                    hiHat.openEdge = i;
                else
                    hiHat.open = i;
            }
        }
    }

    for (int i = 0; i < instrumentCount; ++i)
    {
        if (kit.isNativeGlorpKit)
        {
            instrumentBuses[static_cast<size_t>(i)] =
                std::clamp(kit.instruments[static_cast<size_t>(i)].outputBus, 0, 15);

            const auto &inst = kit.instruments[static_cast<size_t>(i)];
            auto itL = kit.factoryInstrumentLevels.find(inst.logicalName);
            if (itL == kit.factoryInstrumentLevels.end())
                itL = kit.factoryInstrumentLevels.find(inst.name);
            if (itL != kit.factoryInstrumentLevels.end())
                channelParams[static_cast<size_t>(i)].level = itL->second;

            auto itP = kit.factoryInstrumentPans.find(inst.logicalName);
            if (itP == kit.factoryInstrumentPans.end())
                itP = kit.factoryInstrumentPans.find(inst.name);
            if (itP != kit.factoryInstrumentPans.end())
                channelParams[static_cast<size_t>(i)].pan = itP->second;
        }
        else
        {
            const auto n = instruments_[static_cast<size_t>(i)].name.toLowerCase();
            if (n.contains("kick") || n.contains("kdrum"))
                instrumentBuses[static_cast<size_t>(i)] = 0; // Bus 1: Ch 1-2
            else if (n.contains("snare") || n.contains("rimclick") || n.contains("rimshot"))
                instrumentBuses[static_cast<size_t>(i)] = 1; // Bus 2: Ch 3-4
            else if (n.contains("hihat") || n.contains("hh"))
                instrumentBuses[static_cast<size_t>(i)] = 2; // Bus 3: Ch 5-6
            else if (n.contains("tom1") || n.contains("hightom"))
                instrumentBuses[static_cast<size_t>(i)] = 3; // Bus 4: Ch 7-8
            else if (n.contains("tom2") || n.contains("midtom"))
                instrumentBuses[static_cast<size_t>(i)] = 4; // Bus 5: Ch 9-10
            else if (n.contains("ftom") || n.contains("floor"))
                instrumentBuses[static_cast<size_t>(i)] = 5; // Bus 6: Ch 11-12
            else if (n.contains("ride"))
                instrumentBuses[static_cast<size_t>(i)] = 6; // Bus 7: Ch 13-14
            else if (n.contains("crash") || n.contains("cymbal") || n.contains("splash") || n.contains("china"))
                instrumentBuses[static_cast<size_t>(i)] = 7; // Bus 8: Ch 15-16
            else
                instrumentBuses[static_cast<size_t>(i)] = std::min(i, 15);
        }
    }

    // Initialize per-mic channel strip states & DSP
    micChannels.clear();
    for (size_t k = 0; k < kitChannels_.size(); ++k)
    {
        auto ch = std::make_unique<MicChannelState>();
        ch->name = kitChannels_[k].name;
        ch->title = kitChannels_[k].title.empty() ? kitChannels_[k].name : kitChannels_[k].title;
        ch->params = MicStripParams {};
        ch->fx.prepare(currentSampleRate, currentBlockSize);

        auto itM = kit.factoryMicLevels.find(ch->name.toStdString());
        if (itM != kit.factoryMicLevels.end())
            ch->params.level = itM->second;

        micChannels.push_back(std::move(ch));
    }

    for (int slot = 0; slot < 2; ++slot)
    {
        userLayers[static_cast<size_t>(slot)].micChannelIndex = -1;
        if (userLayers[static_cast<size_t>(slot)].loaded)
            appendUserLayerChannel(slot);
    }

    // Detect snare top and bottom mics for sympathetic snare wire resonance
    snareMicIndex = -1;
    snareBotMicIndex = -1;
    for (size_t k = 0; k < kitChannels_.size(); ++k)
    {
        const auto n = juce::String(kitChannels_[k].name).toLowerCase();
        if (n.contains("snare") || n.contains("sd_") || n.contains("sn_"))
        {
            if (n.contains("bot") || n.contains("bottom") || n.contains("under"))
                snareBotMicIndex = static_cast<int>(k);
            else if (snareMicIndex < 0 || n.contains("top") || n.contains("up"))
                snareMicIndex = static_cast<int>(k);
        }
    }
    if (snareMicIndex < 0 && snareBotMicIndex >= 0)
        snareMicIndex = snareBotMicIndex;
    if (snareBotMicIndex < 0 && snareMicIndex >= 0)
        snareBotMicIndex = snareMicIndex;

    // A kit with more instruments than buses is a hard error, not a silent
    // truncation, so surface it rather than hiding some of the kit.
    if (missingFiles > 0)
    {
        error = juce::String(missingFiles) + " of " + juce::String(kit.instruments.size())
                + " instruments had unreadable sample files and were skipped.";
    }

    recomputeSoloFlags();
    sampleCache.trim();

    kitLoaded.store(true);
    return true;
}

void Sampler::unloadKit()
{
    const juce::ScopedLock lock(kitLock);

    kitLoaded.store(false);
    allNotesOff();

    crosstalk.reset();
    snareMicIndex = -1;
    snareBotMicIndex = -1;

    instruments_.clear();
    kitChannels_.clear();
    micChannels.clear();
    channelParams.clear();
    roundRobinCursor.clear();
    instrumentBuses.clear();
    instrumentCount = 0;
    busCount = 0;
    kitTitle.clear();

    anySolo.store(false);
    hiHat = {};
    cc4Active.store(false);
    sampleCache.trim();
}

void Sampler::recomputeSoloFlags()
{
    const auto soloing = anySolo.load();

    for (auto &p : channelParams)
        p.muted = p.mute || (soloing && !p.solo);
}

void Sampler::resetInstrumentParams()
{
    for (auto &p : channelParams)
    {
        p = ChannelParams {};
        p.level = 1.0f;
        p.velocityCurve = VelocityCurve::linear;
        p.useCustomVelocityCurve = false;
    }

    anySolo.store(false);
    recomputeSoloFlags();
}

int Sampler::numKitChannels() const
{
    return static_cast<int>(kitChannels_.size());
}

juce::String Sampler::kitChannelName(int index) const
{
    if (index < 0 || index >= static_cast<int>(kitChannels_.size()))
        return {};

    return kitChannels_[static_cast<size_t>(index)].name;
}

juce::String Sampler::kitChannelTitle(int index) const
{
    if (index < 0 || index >= static_cast<int>(kitChannels_.size()))
        return {};

    const auto &c = kitChannels_[static_cast<size_t>(index)];

    if (!c.title.empty())
        return juce::String(c.title);

    return juce::String(c.name);
}

int Sampler::numMicStrips() const noexcept
{
    return static_cast<int>(micChannels.size());
}

juce::String Sampler::micStripName(int index) const
{
    if (index >= 0 && index < static_cast<int>(micChannels.size()))
        return micChannels[static_cast<size_t>(index)]->name;
    return {};
}

juce::String Sampler::micStripTitle(int index) const
{
    if (index >= 0 && index < static_cast<int>(micChannels.size()))
        return micChannels[static_cast<size_t>(index)]->title;
    return {};
}

void Sampler::setMicStripParams(int index, const MicStripParams &params)
{
    if (index >= 0 && index < static_cast<int>(micChannels.size()))
        micChannels[static_cast<size_t>(index)]->params = params;
}

MicStripParams Sampler::getMicStripParams(int index) const
{
    if (index >= 0 && index < static_cast<int>(micChannels.size()))
        return micChannels[static_cast<size_t>(index)]->params;
    return {};
}

void Sampler::setMicOutputBus(int index, int bus)
{
    if (index >= 0 && index < static_cast<int>(micChannels.size()))
        micChannels[static_cast<size_t>(index)]->params.outputBus = std::clamp(bus, 0, 15);
}

int Sampler::getMicOutputBus(int index) const
{
    if (index >= 0 && index < static_cast<int>(micChannels.size()))
        return micChannels[static_cast<size_t>(index)]->params.outputBus;
    return 0;
}

void Sampler::setMasterStripParams(const MasterStripParams &params)
{
    masterStripParams = params;
}

MasterStripParams Sampler::getMasterStripParams() const
{
    return masterStripParams;
}

void Sampler::applyMixerPreset(const MixerPreset &preset)
{
    masterStripParams = preset.master;
    for (size_t i = 0; i < preset.micStrips.size() && i < micChannels.size(); ++i)
        micChannels[i]->params = preset.micStrips[i];
}

MixerPreset Sampler::getCurrentMixerPreset(const juce::String &name) const
{
    MixerPreset p;
    p.name = name;
    p.master = masterStripParams;
    for (const auto &ch : micChannels)
    {
        p.micStrips.push_back(ch->params);
        p.micNames.push_back(ch->title);
    }
    return p;
}

float Sampler::getMicPeakL(int index) const noexcept
{
    if (index >= 0 && index < static_cast<int>(micChannels.size()))
        return micChannels[static_cast<size_t>(index)]->peakL.load(std::memory_order_relaxed);
    return 0.0f;
}

float Sampler::getMicPeakR(int index) const noexcept
{
    if (index >= 0 && index < static_cast<int>(micChannels.size()))
        return micChannels[static_cast<size_t>(index)]->peakR.load(std::memory_order_relaxed);
    return 0.0f;
}

float Sampler::getMicRmsL(int index) const noexcept
{
    if (index >= 0 && index < static_cast<int>(micChannels.size()))
        return micChannels[static_cast<size_t>(index)]->rmsL.load(std::memory_order_relaxed);
    return 0.0f;
}

float Sampler::getMicRmsR(int index) const noexcept
{
    if (index >= 0 && index < static_cast<int>(micChannels.size()))
        return micChannels[static_cast<size_t>(index)]->rmsR.load(std::memory_order_relaxed);
    return 0.0f;
}

//==============================================================================
juce::String Sampler::instrumentName(int index) const
{
    if (index < 0 || index >= instrumentCount)
        return {};

    return instruments_[static_cast<size_t>(index)].name;
}

int Sampler::numSamplesForInstrument(int index) const
{
    if (index < 0 || index >= instrumentCount)
        return 0;

    return static_cast<int>(instruments_[static_cast<size_t>(index)].hits.size());
}

juce::String Sampler::sampleName(int instrument, int sample) const
{
    if (instrument < 0 || instrument >= instrumentCount)
        return {};

    const auto &hits = instruments_[static_cast<size_t>(instrument)].hits;

    if (sample < 0 || sample >= static_cast<int>(hits.size()))
        return {};

    return hits[static_cast<size_t>(sample)].name;
}

//==============================================================================
void Sampler::setInstrumentParams(int index, const InstrumentParams &params)
{
    if (index < 0 || index >= static_cast<int>(channelParams.size()))
        return;

    auto &p = channelParams[static_cast<size_t>(index)];

    const float newPitch = std::clamp(params.pitchSemitones, -12.0f, 12.0f);
    const bool pitchChanged = (p.pitch != newPitch);
    const bool soloChanged = (p.solo != params.solo || p.mute != params.mute);

    p.level = std::max(0.0f, params.level);
    p.pan = std::clamp(params.pan, -1.0f, 1.0f);
    p.pitch = newPitch;
    p.spread = std::clamp(params.tuneSpread, 0.0f, 100.0f);
    p.dynamics = std::clamp(params.dynamics, 0.0f, 1.0f);
    p.attackMs = std::clamp(params.attackMs, 0.0f, 200.0f);
    p.attackAmount = std::clamp(params.attackAmount, -1.0f, 1.0f);
    p.damping = std::clamp(params.damping, 0.0f, 1.0f);
    p.velocityCurve = params.velocityCurve;
    p.useCustomVelocityCurve = params.useCustomVelocityCurve;
    p.customVelocityCurve = params.customVelocityCurve;
    p.roundRobin = params.autoRoundRobin;
    p.selectedSample = params.selectedSample;
    p.mute = params.mute;
    p.solo = params.solo;

    if (soloChanged)
    {
        bool currentAnySolo = false;
        for (const auto &cp : channelParams)
            if (cp.solo)
            {
                currentAnySolo = true;
                break;
            }
        anySolo.store(currentAnySolo, std::memory_order_relaxed);
        recomputeSoloFlags();
    }
}

InstrumentParams Sampler::getInstrumentParams(int index) const
{
    InstrumentParams out;

    if (index < 0 || index >= static_cast<int>(channelParams.size()))
        return out;

    const auto &p = channelParams[static_cast<size_t>(index)];

    out.level = p.level;
    out.pan = p.pan;
    out.pitchSemitones = p.pitch;
    out.tuneSpread = p.spread;
    out.dynamics = p.dynamics;
    out.attackMs = p.attackMs;
    out.attackAmount = p.attackAmount;
    out.damping = p.damping;
    out.velocityCurve = p.velocityCurve;
    out.useCustomVelocityCurve = p.useCustomVelocityCurve;
    out.customVelocityCurve = p.customVelocityCurve;
    out.autoRoundRobin = p.roundRobin;
    out.mute = p.mute;
    out.solo = p.solo;
    out.selectedSample = p.selectedSample;

    return out;
}

void Sampler::setInstrumentBus(int instrument, int busIndex)
{
    if (instrument >= 0 && instrument < static_cast<int>(instrumentBuses.size()))
        instrumentBuses[static_cast<size_t>(instrument)] = std::clamp(busIndex, 0, 15);
}

int Sampler::getInstrumentBus(int instrument) const noexcept
{
    if (instrument >= 0 && instrument < static_cast<int>(instrumentBuses.size()))
        return instrumentBuses[static_cast<size_t>(instrument)];
    return 0;
}

void Sampler::setGlobalParams(const GlobalParams &p)
{
    globals.masterLevel = std::max(0.0f, p.masterLevel);
    globals.masterPan = std::clamp(p.masterPan, -1.0f, 1.0f);
    globals.attackMs = std::clamp(p.attackMs, 0.0f, 500.0f);
    globals.decayScale = std::clamp(p.decayScale, 0.05f, 4.0f);
    globals.humanize = std::clamp(p.humanize, 0.0f, 1.0f);
    globals.velCurve = std::clamp(p.velCurve, 0.2f, 4.0f);
}

GlobalParams Sampler::getGlobalParams() const
{
    return {globals.masterLevel,
            globals.masterPan,
            globals.attackMs,
            globals.decayScale,
            globals.humanize,
            globals.velCurve};
}

//==============================================================================
void Sampler::resolveInstrumentChannels(const DrumKit &kit, const Instrument &instrument, InstrumentRuntime &out)
{
    out.name = instrument.displayName;
    out.group = instrument.group;
    out.piece = instrument.bleedPiece;

    for (size_t s = 0; s < instrument.samples.size(); ++s)
    {
        const auto &sources = instrument.resolvedChannels[s];

        if (sources.empty())
            continue;

        ResolvedHit hit;
        hit.name = instrument.samples[s].name;
        hit.power = instrument.samples[s].power;

        const auto channelCount = sources.size();
        // If the kit defines discrete mic channels, each source channel is routed
        // to its respective mic mixer track at unity gain (1.0f), matching DAW / REAPER behavior.
        // Only if kit.channels is empty (all audio files collapse into a single stereo pair)
        // do we normalize by sqrt(channelCount) to prevent digital clipping when summing.
        const bool hasDiscreteMics = !kit.channels.empty();
        const float normalisation =
            hasDiscreteMics ? 1.0f : (1.0f / std::sqrt(std::max(1.0f, static_cast<float>(channelCount))));

        for (size_t c = 0; c < channelCount; ++c)
        {
            const auto &source = sources[c];
            const auto &ref = instrument.samples[s].audioFiles[static_cast<size_t>(source.audioFileIndex)];

            ChannelRef channelRef;
            channelRef.handle = sampleCache.open(instrument.samplePath(ref));
            channelRef.channel = ref.fileChannel;
            channelRef.kitChannelIndex = 0;
            channelRef.isBleed = !source.main;
            channelRef.pieceIdx = static_cast<int>(instrument.bleedPiece);
            channelRef.channelBleedFactor = source.bleed;

            for (size_t k = 0; k < kit.channels.size(); ++k)
            {
                if (kit.channels[k].name == source.kitChannel || kit.channels[k].title == source.kitChannel)
                {
                    channelRef.kitChannelIndex = static_cast<int>(k);
                    break;
                }
            }

            if (!channelRef.handle.valid)
                continue;

            // When discrete mic channels exist, mic level is applied by the channel strip fader
            // (ch->params.level initialized from kit.factoryMicLevels).
            // For fallback single-track kits, apply factoryMicLevels directly here.
            float micLevel = 1.0f;
            if (!hasDiscreteMics)
            {
                auto itM = kit.factoryMicLevels.find(ref.channel);
                if (itM == kit.factoryMicLevels.end())
                    itM = kit.factoryMicLevels.find(source.kitChannel);
                if (itM != kit.factoryMicLevels.end())
                    micLevel = itM->second;
            }

            if (channelCount == 1)
            {
                channelRef.gainL = normalisation * micLevel;
                channelRef.gainR = normalisation * micLevel;
            }
            else
            {
                const auto chName = juce::String(ref.channel).toLowerCase();
                if (chName.contains("left") || chName.endsWith("l") || chName.contains("_l") || chName.contains(".l"))
                {
                    channelRef.gainL = normalisation * micLevel;
                    channelRef.gainR = 0.0f;
                }
                else if (chName.contains("right") || chName.endsWith("r") || chName.contains("_r")
                         || chName.contains(".r"))
                {
                    channelRef.gainL = 0.0f;
                    channelRef.gainR = normalisation * micLevel;
                }
                else
                {
                    channelRef.gainL = normalisation * micLevel;
                    channelRef.gainR = normalisation * micLevel;
                }
            }

            hit.channels.push_back(std::move(channelRef));
            hit.numFrames = std::max(hit.numFrames, channelRef.handle.numFrames);
        }

        if (!hit.channels.empty() && hit.numFrames > 0)
            out.hits.push_back(std::move(hit));
    }

    // DrumGizmo kit builders store raw recorded power, not a 0..1 value.
    // Cache the instrument's range so velocity selection can use the whole kit.
    if (!out.hits.empty())
    {
        out.minPower = out.hits.front().power;
        out.maxPower = out.hits.front().power;

        for (const auto &hit : out.hits)
        {
            out.minPower = std::min(out.minPower, hit.power);
            out.maxPower = std::max(out.maxPower, hit.power);
        }

        if (!(out.maxPower > out.minPower))
        {
            out.minPower = 0.0f;
            out.maxPower = 1.0f;
        }
    }
}

//==============================================================================
float Sampler::nextRandom()
{
    // xorshift: cheap, and good enough for humanising.
    auto x = randomState.load();
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    randomState.store(x);
    return static_cast<float>(x & 0xFFFFFF) / static_cast<float>(0xFFFFFF) * 2.0f - 1.0f;
}

Sampler::Voice *Sampler::allocateVoice()
{
    for (int attempt = 0; attempt < maxVoices; ++attempt)
    {
        const auto i = static_cast<size_t>(nextVoiceIndex.fetch_add(1, std::memory_order_relaxed) % maxVoices);

        if (!pool.voices[i]->active)
            return pool.voices[i].get();
    }

    // Everything is busy: steal the quietest, and oldest of those.
    stolenVoices.fetch_add(1, std::memory_order_relaxed);
    Voice *best = nullptr;

    for (auto &voicePtr : pool.voices)
    {
        auto &v = *voicePtr;

        if (!best || v.velocity < best->velocity || (v.velocity == best->velocity && v.startedAt < best->startedAt))
            best = &v;
    }

    return best;
}

//==============================================================================
int Sampler::layerSlotForInstrument(int instrument) const noexcept
{
    if (instrument < 0 || instrument >= instrumentCount)
        return -1;

    const auto n = instruments_[static_cast<size_t>(instrument)].name.toLowerCase();

    if (n.contains("kick") || n.contains("kdrum") || n.contains("bdrum") || n.contains("bassdrum") || n.contains("bass drum"))
        return 0;

    if (n.contains("snare") || n.contains("snr"))
    {
        if (n.contains("rim") || n.contains("click") || n.contains("side") || n.contains("stick")
            || n.contains("rest"))
            return -1;
        return 1;
    }

    return -1;
}

float Sampler::kitBlendForInstrument(int instrument) const noexcept
{
    const int slot = layerSlotForInstrument(instrument);
    if (slot < 0 || !userLayers[static_cast<size_t>(slot)].loaded)
        return 1.0f;

    return 1.0f - std::clamp(userLayers[static_cast<size_t>(slot)].params.blend, 0.0f, 1.0f);
}

bool Sampler::isUserLayerLoaded(int slot) const noexcept
{
    if (slot < 0 || slot > 1)
        return false;
    return userLayers[static_cast<size_t>(slot)].loaded;
}

juce::String Sampler::getUserLayerPath(int slot) const
{
    if (slot < 0 || slot > 1)
        return {};
    const juce::ScopedLock lock(kitLock);
    return userLayers[static_cast<size_t>(slot)].path;
}

juce::String Sampler::getUserLayerName(int slot) const
{
    if (slot < 0 || slot > 1)
        return {};
    const juce::ScopedLock lock(kitLock);
    return userLayers[static_cast<size_t>(slot)].name;
}

UserLayerParams Sampler::getUserLayerParams(int slot) const
{
    if (slot < 0 || slot > 1)
        return {};
    const juce::ScopedLock lock(kitLock);
    auto params = userLayers[static_cast<size_t>(slot)].params;
    const int mic = userLayers[static_cast<size_t>(slot)].micChannelIndex;
    if (mic >= 0 && mic < static_cast<int>(micChannels.size()))
    {
        params.level = micChannels[static_cast<size_t>(mic)]->params.level;
        params.pan = micChannels[static_cast<size_t>(mic)]->params.pan;
    }
    return params;
}

int Sampler::getUserLayerMicIndex(int slot) const noexcept
{
    if (slot < 0 || slot > 1)
        return -1;
    return userLayers[static_cast<size_t>(slot)].micChannelIndex;
}

void Sampler::appendUserLayerChannel(int slot)
{
    if (slot < 0 || slot > 1)
        return;

    auto &layer = userLayers[static_cast<size_t>(slot)];
    if (layer.micChannelIndex >= 0)
        return;

    if (micChannels.size() >= 32)
        return;

    auto ch = std::make_unique<MicChannelState>();
    ch->name = slot == 0 ? "kick_layer" : "snare_layer";
    ch->title = slot == 0 ? "Kick Layer" : "Snare Layer";
    ch->params = MicStripParams {};
    ch->params.level = std::clamp(layer.params.level, 0.0f, 2.0f);
    ch->params.pan = std::clamp(layer.params.pan, -1.0f, 1.0f);
    ch->fx.prepare(currentSampleRate, currentBlockSize);

    layer.micChannelIndex = static_cast<int>(micChannels.size());
    micChannels.push_back(std::move(ch));
}

void Sampler::removeUserLayerChannel(int slot)
{
    if (slot < 0 || slot > 1)
        return;

    auto &layer = userLayers[static_cast<size_t>(slot)];
    const int index = layer.micChannelIndex;
    if (index < 0 || index >= static_cast<int>(micChannels.size()))
    {
        layer.micChannelIndex = -1;
        return;
    }

    micChannels.erase(micChannels.begin() + index);
    layer.micChannelIndex = -1;

    for (int other = 0; other < 2; ++other)
    {
        auto &ol = userLayers[static_cast<size_t>(other)];
        if (other != slot && ol.micChannelIndex > index)
            --ol.micChannelIndex;
    }
}

bool Sampler::loadUserLayer(int slot, const juce::String &path, const UserLayerParams &params, juce::String &error)
{
    if (slot < 0 || slot > 1)
    {
        error = "invalid user layer slot";
        return false;
    }

    const juce::ScopedLock lock(kitLock);
    auto handle = sampleCache.open(path.toStdString());

    if (!handle.valid || handle.numFrames <= 0)
    {
        error = "could not read one-shot WAV: " + path;
        return false;
    }

    auto &layer = userLayers[static_cast<size_t>(slot)];
    layer.loaded = true;
    layer.path = path;
    layer.name = juce::File(path).getFileName();
    layer.sample = handle;

    for (auto &v : layerVoices)
        if (v.slot == slot)
            v.active = false;
    layer.sampleRate = handle.sampleRate > 0.0 ? handle.sampleRate : currentSampleRate;
    layer.params = params;
    layer.params.blend = std::clamp(layer.params.blend, 0.0f, 1.0f);
    layer.params.attackMs = std::clamp(layer.params.attackMs, 0.0f, 200.0f);
    layer.params.decayMs = std::clamp(layer.params.decayMs, 0.0f, 2000.0f);
    layer.params.sustain = std::clamp(layer.params.sustain, 0.0f, 1.0f);
    layer.params.releaseMs = std::clamp(layer.params.releaseMs, 0.0f, 2000.0f);
    layer.params.delayMs = std::clamp(layer.params.delayMs, 0.0f, 20.0f);
    layer.params.level = std::clamp(layer.params.level, 0.0f, 2.0f);
    layer.params.pan = std::clamp(layer.params.pan, -1.0f, 1.0f);

    if (layer.micChannelIndex < 0)
        appendUserLayerChannel(slot);

    if (layer.micChannelIndex >= 0 && layer.micChannelIndex < static_cast<int>(micChannels.size()))
    {
        auto &mp = micChannels[static_cast<size_t>(layer.micChannelIndex)]->params;
        mp.level = layer.params.level;
        mp.pan = layer.params.pan;
    }

    return true;
}

void Sampler::setUserLayerParams(int slot, const UserLayerParams &params)
{
    if (slot < 0 || slot > 1)
        return;

    const juce::ScopedLock lock(kitLock);
    auto &layer = userLayers[static_cast<size_t>(slot)];
    layer.params = params;
    layer.params.blend = std::clamp(layer.params.blend, 0.0f, 1.0f);
    layer.params.attackMs = std::clamp(layer.params.attackMs, 0.0f, 200.0f);
    layer.params.decayMs = std::clamp(layer.params.decayMs, 0.0f, 2000.0f);
    layer.params.sustain = std::clamp(layer.params.sustain, 0.0f, 1.0f);
    layer.params.releaseMs = std::clamp(layer.params.releaseMs, 0.0f, 2000.0f);
    layer.params.delayMs = std::clamp(layer.params.delayMs, 0.0f, 20.0f);
    layer.params.level = std::clamp(layer.params.level, 0.0f, 2.0f);
    layer.params.pan = std::clamp(layer.params.pan, -1.0f, 1.0f);
}

void Sampler::clearUserLayer(int slot)
{
    if (slot < 0 || slot > 1)
        return;

    const juce::ScopedLock lock(kitLock);
    for (auto &v : layerVoices)
        if (v.slot == slot)
            v.active = false;

    auto &layer = userLayers[static_cast<size_t>(slot)];
    layer.loaded = false;
    layer.path.clear();
    layer.name.clear();
    layer.sample = {};
    layer.sampleRate = 44100.0;
    layer.params = UserLayerParams {};
    removeUserLayerChannel(slot);
}

void Sampler::triggerUserLayer(int slot, float velocity, int64_t timeInSamples)
{
    if (slot < 0 || slot > 1)
        return;

    auto &layer = userLayers[static_cast<size_t>(slot)];
    if (!layer.loaded || layer.micChannelIndex < 0 || layer.params.blend <= 0.001f)
        return;

    LayerVoice *voice = nullptr;
    for (auto &v : layerVoices)
    {
        if (!v.active)
        {
            voice = &v;
            break;
        }
    }

    if (voice == nullptr)
    {
        voice = &layerVoices[0];
        for (auto &v : layerVoices)
            if (v.startedAt < voice->startedAt)
                voice = &v;
    }

    voice->active = true;
    voice->slot = slot;
    voice->startedAt = timeInSamples
                       + static_cast<int64_t>(layer.params.delayMs * 0.001 * currentSampleRate);
    voice->position = 0.0;
    voice->velocity = std::clamp(velocity, 0.0f, 1.0f);
}

void Sampler::renderUserLayerVoices(int numSamples)
{
    if (numSamples <= 0)
        return;

    for (auto &v : layerVoices)
    {
        if (!v.active)
            continue;

        auto &layer = userLayers[static_cast<size_t>(v.slot)];
        if (!layer.loaded || layer.micChannelIndex < 0
            || layer.micChannelIndex >= static_cast<int>(micScratchL.size()))
        {
            v.active = false;
            continue;
        }

        const int mic = layer.micChannelIndex;
        auto *dstL = micScratchL[static_cast<size_t>(mic)].data();
        auto *dstR = micScratchR[static_cast<size_t>(mic)].data();
        const auto sampleFrames = layer.sample.numFrames;
        const double rateRatio = layer.sampleRate > 0.0 ? layer.sampleRate / currentSampleRate : 1.0;
        const int attackSamples = static_cast<int>(layer.params.attackMs * 0.001 * currentSampleRate);
        const int decaySamples = static_cast<int>(layer.params.decayMs * 0.001 * currentSampleRate);
        const int releaseSamples = static_cast<int>(layer.params.releaseMs * 0.001 * currentSampleRate);
        const auto outFrames = static_cast<int64_t>(static_cast<double>(sampleFrames) / rateRatio);
        const int voiceReleaseSamples = std::min(releaseSamples, static_cast<int>(std::min<int64_t>(outFrames, 2000000)));
        const int startOffset = static_cast<int>(
            std::clamp<int64_t>(v.startedAt - sampleClock, 0, static_cast<int64_t>(numSamples)));

        if (startOffset >= numSamples)
            continue;

        const int channelCount = std::max(1, layer.sample.numChannels);
        const float polarity = layer.params.polarityInvert ? -1.0f : 1.0f;
        const float blend = std::clamp(layer.params.blend, 0.0f, 1.0f);
        const float gain = v.velocity * blend * polarity;

        for (int i = startOffset; i < numSamples; ++i)
        {
            if (v.position >= static_cast<double>(sampleFrames))
            {
                v.active = false;
                break;
            }

            const auto elapsed = (sampleClock + i) - v.startedAt;
            float env = 1.0f;
            if (attackSamples > 0 && elapsed < attackSamples)
                env = static_cast<float>(elapsed) / static_cast<float>(attackSamples);
            else if (decaySamples > 0 && elapsed < attackSamples + decaySamples)
            {
                const auto dt = static_cast<float>(elapsed - attackSamples) / static_cast<float>(decaySamples);
                env = 1.0f + (layer.params.sustain - 1.0f) * std::clamp(dt, 0.0f, 1.0f);
            }
            else
                env = layer.params.sustain;

            if (voiceReleaseSamples > 0)
            {
                const auto remaining = outFrames - elapsed;
                if (remaining < voiceReleaseSamples)
                    env *= std::clamp(static_cast<float>(remaining) / static_cast<float>(voiceReleaseSamples),
                                      0.0f,
                                      1.0f);
            }

            const auto pos = static_cast<int64_t>(v.position);
            const auto frac = static_cast<float>(v.position - static_cast<double>(pos));
            const float s0L = layer.sample.readSample(pos, 0);
            const float s1L = layer.sample.readSample(pos + 1, 0);
            const float l = s0L + frac * (s1L - s0L);
            float r = l;
            if (channelCount > 1)
            {
                const float s0R = layer.sample.readSample(pos, 1);
                const float s1R = layer.sample.readSample(pos + 1, 1);
                r = s0R + frac * (s1R - s0R);
            }

            const float outGain = gain * env;
            dstL[i] += l * outGain;
            dstR[i] += r * outGain;

            v.position += rateRatio;
        }
    }
}

void Sampler::autoAlignUserLayer(int slot)
{
    if (slot < 0 || slot > 1)
        return;

    const juce::ScopedLock lock(kitLock);
    auto &layer = userLayers[static_cast<size_t>(slot)];
    if (!layer.loaded)
        return;

    const SampleCache::Handle *reference = nullptr;
    int referenceChannel = 0;

    for (int i = 0; i < instrumentCount && reference == nullptr; ++i)
    {
        if (layerSlotForInstrument(i) != slot)
            continue;

        for (const auto &hit : instruments_[static_cast<size_t>(i)].hits)
        {
            for (const auto &channel : hit.channels)
            {
                if (channel.handle.valid && !channel.isBleed)
                {
                    reference = &channel.handle;
                    referenceChannel = channel.channel;
                    break;
                }
            }
            if (reference != nullptr)
                break;
        }
    }

    if (reference == nullptr)
        return;

    const int frames = static_cast<int>(std::min<int64_t>(
        std::min(reference->numFrames, layer.sample.numFrames), 4096));
    if (frames <= 16)
        return;

    std::vector<float> ref(static_cast<size_t>(frames), 0.0f);
    std::vector<float> oneShot(static_cast<size_t>(frames), 0.0f);
    reference->read(0, frames, referenceChannel, ref.data());
    layer.sample.read(0, frames, 0, oneShot.data());

    const int maxLag = std::min(frames / 3, static_cast<int>(0.02 * layer.sampleRate));
    float bestDot = -1.0e30f;
    int bestLag = 0;
    bool bestInvert = false;

    for (int lag = 0; lag <= maxLag; ++lag)
    {
        float dot = 0.0f;
        for (int i = maxLag; i < frames; ++i)
            dot += ref[static_cast<size_t>(i)] * oneShot[static_cast<size_t>(i - lag)];

        if (std::abs(dot) > bestDot)
        {
            bestDot = std::abs(dot);
            bestLag = lag;
            bestInvert = dot < 0.0f;
        }
    }

    layer.params.delayMs = static_cast<float>(bestLag) / static_cast<float>(layer.sampleRate) * 1000.0f;
    layer.params.polarityInvert = bestInvert;
}

void Sampler::getUserLayerPeaks(int slot, int numPeaks, std::vector<float> &peaks) const
{
    peaks.assign(static_cast<size_t>(std::max(1, numPeaks)), 0.0f);
    if (slot < 0 || slot > 1)
        return;

    const juce::ScopedLock lock(kitLock);
    const auto &layer = userLayers[static_cast<size_t>(slot)];
    if (!layer.loaded || layer.sample.numFrames <= 0)
        return;

    const auto frames = layer.sample.numFrames;
    for (int p = 0; p < numPeaks; ++p)
    {
        const auto start = frames * p / numPeaks;
        const auto end = std::max(start + 1, frames * (p + 1) / numPeaks);
        float peak = 0.0f;
        const auto step = std::max<int64_t>(1, (end - start) / 8);
        for (auto f = start; f < end; f += step)
            peak = std::max(peak, std::abs(layer.sample.readSample(f, 0)));
        peaks[static_cast<size_t>(p)] = peak;
    }
}

int Sampler::chooseHit(int instrumentIndex, float velocity)
{
    if (instrumentIndex < 0 || instrumentIndex >= instrumentCount)
        return 0;

    const auto &hits = instruments_[static_cast<size_t>(instrumentIndex)].hits;
    const auto numHits = static_cast<int>(hits.size());

    if (numHits <= 0)
        return 0;

    if (numHits == 1)
        return 0;

    const float vel = std::clamp(velocity, 0.05f, 1.0f);

    // Kits store raw recorded power (Muldjord snare: 0.3..46.6, Crocell:
    // 0.01..3.3), not a 0..1 value. Map velocity onto the instrument's own
    // power range and pick a hit near that target. Comparing raw power to
    // the 0..1 velocity used to land almost every normal hit on a ghost-note
    // sample, which is what makes a snare randomly sound like a dud.
    const auto &runtime = instruments_[static_cast<size_t>(instrumentIndex)];
    const float powerRange = std::max(1.0e-6f, runtime.maxPower - runtime.minPower);
    const float targetPower = runtime.minPower + vel * powerRange;

    float minDiff = std::numeric_limits<float>::max();
    int closestIdx = 0;

    for (int i = 0; i < numHits; ++i)
    {
        const float diff = std::abs(hits[static_cast<size_t>(i)].power - targetPower);
        if (diff < minDiff)
        {
            minDiff = diff;
            closestIdx = i;
        }
    }

    // Keep the round-robin pool close to the target so neighbouring hits vary
    // in sample choice without jumping to ghost notes or dropping loudness.
    std::vector<int> candidates;
    const float margin = powerRange * 0.08f;
    for (int i = 0; i < numHits; ++i)
    {
        if (std::abs(hits[static_cast<size_t>(i)].power - targetPower) <= margin)
            candidates.push_back(i);
    }

    if (candidates.empty())
    {
        // Kits without meaningful power values (all hits share the same
        // power) still need to round-robin instead of sticking on hit 0.
        const float closestPower = hits[static_cast<size_t>(closestIdx)].power;
        const float epsilon = 1.0e-4f * std::max(1.0f, std::abs(closestPower));

        for (int i = 0; i < numHits; ++i)
        {
            if (std::abs(hits[static_cast<size_t>(i)].power - closestPower) <= epsilon)
                candidates.push_back(i);
        }

        if (candidates.empty())
            candidates.push_back(closestIdx);
    }

    auto &cursor = roundRobinCursor[static_cast<size_t>(instrumentIndex)];
    const auto chosen = candidates[static_cast<size_t>(cursor % static_cast<int>(candidates.size()))];
    cursor = (cursor + 1) % 10000;

    return chosen;
}

//==============================================================================
void Sampler::noteOn(int instrument, float velocity, int64_t timeInSamples)
{
    noteOn(instrument, -1, velocity, timeInSamples);
}

void Sampler::noteOn(int instrument, int note, float velocity, int64_t timeInSamples)
{
    if (!kitLoaded.load())
        return;

    if (instrument < 0 || instrument >= instrumentCount)
        return;

    // E-drum continuous hi-hat articulation selection & crossfading based on pedal CC#4.
    if (hiHatMode.load(std::memory_order_relaxed) == HiHatMode::continuousCC4
        && cc4Active.load(std::memory_order_relaxed))
    {
        const bool isVariableHiHatHit = (instrument == hiHat.open || instrument == hiHat.openEdge
                                         || (instrument == hiHat.closed && (note == 46 || note < 0)));

        if (isVariableHiHatHit)
        {
            struct CrossfadeScope
            {
                int64_t &slot;
                CrossfadeScope(int64_t &s, int64_t value) : slot(s) { slot = value; }
                ~CrossfadeScope() { slot = -1; }
            } crossfadeScope(hiHatCrossfadeStart, timeInSamples);

            const float pedal = std::clamp(hiHatPedal.load(std::memory_order_relaxed), 0.0f, 1.0f);
            const bool wantsEdge = (instrument == hiHat.openEdge || instrument == hiHat.closedEdge);

            struct Stage
            {
                int inst;
                float pos;
            };
            std::vector<Stage> stages;

            if (wantsEdge)
            {
                if (hiHat.openEdge >= 0)
                    stages.push_back({hiHat.openEdge, 0.0f});
                else if (hiHat.open >= 0)
                    stages.push_back({hiHat.open, 0.0f});

                if (hiHat.semiOpen >= 0)
                    stages.push_back({hiHat.semiOpen, 0.5f});
                else if (hiHat.open2 >= 0)
                    stages.push_back({hiHat.open2, 0.5f});

                if (hiHat.closedEdge >= 0)
                    stages.push_back({hiHat.closedEdge, 1.0f});
                else if (hiHat.closed >= 0)
                    stages.push_back({hiHat.closed, 1.0f});
            }
            else
            {
                if (hiHat.open >= 0)
                    stages.push_back({hiHat.open, 0.0f});
                if (hiHat.open4 >= 0)
                    stages.push_back({hiHat.open4, 0.20f});
                if (hiHat.open3 >= 0)
                    stages.push_back({hiHat.open3, 0.40f});
                if (hiHat.open2 >= 0)
                    stages.push_back({hiHat.open2, 0.60f});
                if (hiHat.open1 >= 0)
                    stages.push_back({hiHat.open1, 0.80f});
                else if (hiHat.semiOpen >= 0)
                    stages.push_back({hiHat.semiOpen, 0.75f});
                if (hiHat.closed >= 0)
                    stages.push_back({hiHat.closed, 1.0f});
            }

            if (stages.size() >= 2)
            {
                std::sort(stages.begin(), stages.end(), [](const auto &a, const auto &b) { return a.pos < b.pos; });

                size_t idxA = 0;
                for (size_t s = 0; s + 1 < stages.size(); ++s)
                {
                    if (pedal >= stages[s].pos && pedal <= stages[s + 1].pos)
                    {
                        idxA = s;
                        break;
                    }
                    if (pedal > stages[s + 1].pos)
                        idxA = s;
                }
                const auto &stA = stages[idxA];
                const auto &stB = stages[std::min(idxA + 1, stages.size() - 1)];

                if (stA.inst == stB.inst)
                {
                    triggerVoice(stA.inst, note, velocity, timeInSamples);
                    return;
                }

                const float span = stB.pos - stA.pos;
                const float alpha = span > 0.001f ? std::clamp((pedal - stA.pos) / span, 0.0f, 1.0f) : 0.0f;

                const float wA = std::cos(0.5f * static_cast<float>(pi) * alpha);
                const float wB = std::sin(0.5f * static_cast<float>(pi) * alpha);

                if (wB <= 0.08f)
                {
                    triggerVoice(stA.inst, note, velocity, timeInSamples);
                }
                else if (wA <= 0.08f)
                {
                    triggerVoice(stB.inst, note, velocity, timeInSamples);
                }
                else
                {
                    triggerVoice(stA.inst, note, velocity * wA, timeInSamples);
                    triggerVoice(stB.inst, note, velocity * wB, timeInSamples);
                }
                return;
            }
        }
    }

    triggerVoice(instrument, note, velocity, timeInSamples);
}

namespace
{
static bool isSameCymbal(const juce::String &nameA, const juce::String &nameB)
{
    const auto a = nameA.toLowerCase().removeCharacters(" _-");
    const auto b = nameB.toLowerCase().removeCharacters(" _-");

    // Check specific numbered/positioned cymbals (e.g. crash1 vs crash2, ride1 vs ride2)
    for (const char *id : {"crash1",
                           "crash2",
                           "crash3",
                           "crash4",
                           "crashl",
                           "crashr",
                           "ride1",
                           "ride2",
                           "ride3",
                           "china1",
                           "china2",
                           "splash1",
                           "splash2"})
    {
        if (a.contains(id))
            return b.contains(id);
    }

    // Generic cymbal families when neither has a number/position
    if (a.contains("ride"))
        return b.contains("ride") && !b.contains("ride1") && !b.contains("ride2") && !b.contains("ride3");
    if (a.contains("china"))
        return b.contains("china") && !b.contains("china1") && !b.contains("china2");
    if (a.contains("splash"))
        return b.contains("splash") && !b.contains("splash1") && !b.contains("splash2");
    if (a.contains("crash"))
        return b.contains("crash") && !b.contains("crash1") && !b.contains("crash2") && !b.contains("crash3")
               && !b.contains("crash4") && !b.contains("crashl") && !b.contains("crashr");

    return false;
}
} // namespace

void Sampler::triggerVoice(int instrument, int note, float velocity, int64_t timeInSamples)
{
    if (instrument < 0 || instrument >= instrumentCount)
        return;

    const auto &runtime = instruments_[static_cast<size_t>(instrument)];
    if (runtime.hits.empty())
        return;

    // The only zones that stop playback when triggered are chokes (for the same cymbal)
    // and hi-hat articulation changes.
    const bool isChoke = runtime.name.containsIgnoreCase("choke");
    const bool isHiHatPedal = (instrument == hiHat.pedal);

    const auto hatName = runtime.name.toLowerCase();
    const bool isHat = hatName.contains("hihat") || hatName.contains("hi-hat") || hatName.startsWith("hh")
                       || runtime.group.equalsIgnoreCase("hihat");
    const bool isHiHatClosed = (isHat && (hatName.contains("closed") || hatName.contains("close")))
                               || instrument == hiHat.closed || instrument == hiHat.closedEdge;

    if (isChoke)
    {
        chokeCymbal(instrument, 30, timeInSamples);

        for (const auto &target : runtime.chokeTargets)
        {
            if (target.instrumentIndex >= 0 && target.instrumentIndex < instrumentCount)
            {
                const auto &targetInst = instruments_[static_cast<size_t>(target.instrumentIndex)];
                if (isSameCymbal(runtime.name, targetInst.name))
                    chokeInstrument(target.instrumentIndex, static_cast<int64_t>(target.chokeTimeMs), timeInSamples);
            }
        }
    }
    else if (isHiHatPedal)
    {
        chokeOpenHiHats(20, timeInSamples);
    }
    else if (isHiHatClosed)
    {
        // Striking a closed hat stops any ringing open hat. Only skip voices
        // from the same clock while a CC4 open/closed crossfade is being built;
        // a separate closed note at the same timestamp must still choke.
        const int64_t skipStartedAt = (timeInSamples == hiHatCrossfadeStart) ? timeInSamples : -1;
        chokeOpenHiHats(20, timeInSamples, skipStartedAt);
    }

    auto *voice = allocateVoice();
    if (voice == nullptr)
        return;

    crosstalk.triggerSympatheticHit(runtime.piece, velocity);

    const auto &p = channelParams[static_cast<size_t>(instrument)];
    if (p.muted)
        return;

    // Apply the per-hit velocity response. The eight-point curve is the
    // editable version; the enum presets remain as quick starting points.
    float effectiveVel = std::clamp(velocity, 0.0f, 1.0f);

    if (p.useCustomVelocityCurve)
    {
        effectiveVel = evaluateVelocityPoints(p.customVelocityCurve, effectiveVel);
    }
    else
    {
        switch (p.velocityCurve)
        {
            case VelocityCurve::logarithmic:
                effectiveVel = std::sqrt(effectiveVel);
                break;
            case VelocityCurve::exponential:
                effectiveVel = effectiveVel * effectiveVel;
                break;
            case VelocityCurve::sCurve:
                effectiveVel = effectiveVel * effectiveVel * (3.0f - 2.0f * effectiveVel);
                break;
            case VelocityCurve::linear:
            default:
                break;
        }
    }

    if (std::abs(p.dynamics - 0.5f) > 0.01f)
    {
        const float dynExp = std::pow(2.0f, (p.dynamics - 0.5f) * 3.0f);
        effectiveVel = std::pow(std::clamp(effectiveVel, 0.01f, 1.0f), dynExp);
    }

    const auto hitIndex = chooseHit(instrument, effectiveVel);
    const auto &hit = runtime.hits[static_cast<size_t>(hitIndex)];

    float pitch = p.pitch;
    float level = velocityWithCurve(effectiveVel, globals.velCurve);

    if (globals.humanize > 0.0f)
    {
        level *= 1.0f + nextRandom() * 0.15f * globals.humanize;
        pitch += nextRandom() * (p.spread + 1.5f * globals.humanize);
    }
    else if (p.spread > 0.0f)
    {
        pitch += nextRandom() * p.spread;
    }

    pitch = std::clamp(pitch, -12.0f, 12.0f);

    const int layerSlot = layerSlotForInstrument(instrument);
    if (layerSlot >= 0)
        triggerUserLayer(layerSlot, std::clamp(level, 0.0f, 1.0f), timeInSamples);

    voice->active = true;
    voice->instrument = instrument;
    voice->hit = hitIndex;
    voice->position = 0.0;

    const double rateRatio = (kitSampleRate > 0.0 && currentSampleRate > 0.0) ? kitSampleRate / currentSampleRate : 1.0;
    voice->pitchRatio = std::pow(2.0, pitch / 12.0) * rateRatio;

    voice->velocity = std::clamp(level, 0.0f, 2.0f);
    voice->pitchSemitones = pitch;
    voice->startedAt = timeInSamples;
    voice->endsAt = 0;
    voice->chokeFadeSamples = 0;
    voice->chokeStartSample = 0;
    const float attackMs = std::max(globals.attackMs, p.attackMs);
    voice->attackGain = attackMs > 0.0f ? 0.0f : 1.0f;
    voice->attackSamples = static_cast<int>(attackMs * 0.001 * currentSampleRate);
    voice->attackShape = p.attackAmount;
    voice->attackShapeWindow = std::max(1, static_cast<int>(0.020 * currentSampleRate));

    voice->startSkip = std::max<int64_t>(0, timeInSamples - sampleClock);

    const auto naturalEnd =
        timeInSamples + static_cast<int64_t>(static_cast<double>(hit.numFrames) / voice->pitchRatio);
    voice->endsAtAbsolute = naturalEnd;

    const float effectiveDecay = std::min(globals.decayScale, 1.0f - 0.8f * p.damping);
    if (effectiveDecay < 0.999f)
        voice->endsAt =
            timeInSamples
            + static_cast<int64_t>(static_cast<double>(hit.numFrames) * effectiveDecay / voice->pitchRatio);
    else
        voice->endsAt = 0;

    pushHitEvent({instrument, note, voice->velocity, pitch});
}

void Sampler::chokeInstrument(int instrument, int64_t fadeMs, int64_t timeInSamples, int64_t skipStartedAt)
{
    const auto fadeSamples =
        std::max<int64_t>(16, static_cast<int64_t>(static_cast<double>(fadeMs) * currentSampleRate * 0.001));

    for (auto &voicePtr : pool.voices)
    {
        auto &v = *voicePtr;

        if (v.active && v.instrument == instrument
            && (skipStartedAt < 0 || v.startedAt != skipStartedAt))
        {
            v.endsAt = timeInSamples + fadeSamples;
            v.chokeFadeSamples = fadeSamples;
            v.chokeStartSample = timeInSamples;
        }
    }
}

void Sampler::chokeOpenHiHats(int64_t fadeMs, int64_t timeInSamples, int64_t skipStartedAt)
{
    // Prefer the full articulation list: DRS names three open variants, so the
    // singular hiHat.open index alone would leave the others ringing.
    for (int index : hiHat.openArticulations)
        chokeInstrument(index, fadeMs, timeInSamples, skipStartedAt);

    const int singular[] = {
        hiHat.open, hiHat.openEdge, hiHat.open1, hiHat.open2, hiHat.open3, hiHat.open4, hiHat.semiOpen
    };

    for (int index : singular)
    {
        if (index >= 0 && std::find(hiHat.openArticulations.begin(), hiHat.openArticulations.end(), index)
                              == hiHat.openArticulations.end())
            chokeInstrument(index, fadeMs, timeInSamples, skipStartedAt);
    }
}

void Sampler::allNotesOff()
{
    for (auto &voicePtr : pool.voices)
    {
        auto &v = *voicePtr;

        v.active = false;
        v.instrument = 0;
        v.hit = 0;
        v.position = 0;
        v.velocity = 0.0f;
        v.pitchSemitones = 0.0f;
        v.startSkip = 0;
        v.endsAt = 0;
        v.chokeFadeSamples = 0;
        v.chokeStartSample = 0;
        v.attackShape = 0.0f;
        v.attackShapeWindow = 1;
    }

    for (auto &v : layerVoices)
    {
        v.active = false;
        v.slot = 0;
        v.startedAt = 0;
        v.position = 0.0;
        v.velocity = 0.0f;
    }
}

void Sampler::setHiHatPedal(float pedalValue) noexcept
{
    const float clamped = std::clamp(pedalValue, 0.0f, 1.0f);
    cc4Active.store(true, std::memory_order_relaxed);
    const float prev = hiHatPedal.exchange(clamped, std::memory_order_relaxed);

    // Choke open hi-hats when pedal moves from open towards closed
    if (prev < 0.50f && clamped >= 0.55f)
        chokeOpenHiHats(20, sampleClock);

    // Firm downward foot stomp: trigger pedal chick
    if (clamped - prev > 0.22f && clamped >= 0.70f)
    {
        lastPedalDownClock = sampleClock;
        lastPedalDownSpeed = clamped - prev;

        const int chickInst = hiHat.pedal >= 0 ? hiHat.pedal : hiHat.closed;
        if (chickInst >= 0)
        {
            const float vel = std::clamp((clamped - prev) * 2.2f, 0.25f, 1.0f);
            noteOn(chickInst, -1, vel, sampleClock);
        }
    }
    // Quick foot release immediately after a stomp: trigger Foot Splash!
    else if (prev - clamped > 0.22f && clamped < 0.40f)
    {
        const auto timeSinceDown = sampleClock - lastPedalDownClock;
        if (lastPedalDownClock > 0 && timeSinceDown < static_cast<int64_t>(currentSampleRate * 0.22)
            && lastPedalDownSpeed > 0.20f)
        {
            // Choke previous chick sound
            if (hiHat.pedal >= 0)
                chokeInstrument(hiHat.pedal, 15, sampleClock);
            if (hiHat.closed >= 0)
                chokeInstrument(hiHat.closed, 15, sampleClock);

            // Ring open splash articulation
            const int splashInst = hiHat.splash >= 0 ? hiHat.splash : (hiHat.open >= 0 ? hiHat.open : hiHat.open1);
            if (splashInst >= 0)
            {
                const float vel = std::clamp(lastPedalDownSpeed * 2.0f, 0.30f, 1.0f);
                noteOn(splashInst, -1, vel, sampleClock);
            }
            lastPedalDownClock = 0; // reset
        }
    }
}

void Sampler::chokeCymbal(int instrumentIndex, int64_t fadeMs, int64_t timeInSamples)
{
    if (instrumentIndex < 0 || instrumentIndex >= instrumentCount)
        return;

    chokeInstrument(instrumentIndex, fadeMs, timeInSamples);

    const auto name = instruments_[static_cast<size_t>(instrumentIndex)].name;

    for (int i = 0; i < instrumentCount; ++i)
    {
        if (i == instrumentIndex)
            continue;

        const auto otherName = instruments_[static_cast<size_t>(i)].name;
        if (isSameCymbal(name, otherName))
            chokeInstrument(i, fadeMs, timeInSamples);
    }
}

void Sampler::chokeAllCymbals(int64_t fadeMs, int64_t timeInSamples)
{
    for (int i = 0; i < instrumentCount; ++i)
    {
        const auto n = instruments_[static_cast<size_t>(i)].name.toLowerCase();
        if (n.contains("crash") || n.contains("ride") || n.contains("splash") || n.contains("china")
            || n.contains("cymbal"))
            chokeInstrument(i, fadeMs, timeInSamples);
    }
}

int Sampler::activeVoiceCount() const noexcept
{
    int count = 0;

    for (const auto &voicePtr : pool.voices)
        if (voicePtr->active)
            ++count;

    return count;
}

//==============================================================================
void Sampler::pushHitEvent(const HitEvent &event) noexcept
{
    const auto write = hitWrite.load(std::memory_order_relaxed);
    const auto read = hitRead.load(std::memory_order_relaxed);

    // If the GUI has fallen far behind, drop the event rather than overwrite
    // history the reader has not seen yet.
    if (write - read >= hitRingSize)
        return;

    hitRing[static_cast<size_t>(write) % hitRingSize] = event;
    hitWrite.store(write + 1, std::memory_order_release);
}

bool Sampler::popHit(HitEvent &event) noexcept
{
    const auto read = hitRead.load(std::memory_order_relaxed);
    const auto write = hitWrite.load(std::memory_order_acquire);

    if (read == write)
        return false;

    event = hitRing[static_cast<size_t>(read) % hitRingSize];
    hitRead.store(read + 1, std::memory_order_release);
    return true;
}

bool Sampler::hasPendingHits() const noexcept
{
    return hitRead.load(std::memory_order_relaxed) != hitWrite.load(std::memory_order_relaxed);
}

//==============================================================================
void Sampler::render(float *const *output, int numOutputChannels, int numSamples)
{
    if (numSamples <= 0 || numOutputChannels < 2)
        return;

    // A kit being swapped out must not be read mid-render.
    if (!kitLock.tryEnter())
    {
        for (int c = 0; c < numOutputChannels; ++c)
            if (output[c] != nullptr)
                juce::FloatVectorOperations::clear(output[c], numSamples);

        return;
    }

    KitUnlock unlock {kitLock};

    const auto totalBuses = numOutputChannels / 2;
    for (int c = 0; c < numOutputChannels; ++c)
        if (output[c] != nullptr)
            juce::FloatVectorOperations::clear(output[c], numSamples);

    if (!kitLoaded.load() || totalBuses <= 0)
    {
        sampleClock += numSamples;
        return;
    }

    const auto endOfBlock = sampleClock + numSamples;
    const int numMics = std::min(static_cast<int>(micChannels.size()), 32);

    if (numMics > 0)
    {
        if (static_cast<size_t>(numSamples) > reverbAccumL.size())
        {
            for (size_t i = 0; i < micScratchL.size(); ++i)
            {
                micScratchL[i].resize(static_cast<size_t>(numSamples), 0.0f);
                micScratchR[i].resize(static_cast<size_t>(numSamples), 0.0f);
            }
            reverbAccumL.resize(static_cast<size_t>(numSamples), 0.0f);
            reverbAccumR.resize(static_cast<size_t>(numSamples), 0.0f);
        }

        // 1. Clear intermediate mic scratch buffers
        for (int m = 0; m < numMics; ++m)
        {
            std::fill(micScratchL[static_cast<size_t>(m)].begin(),
                      micScratchL[static_cast<size_t>(m)].begin() + numSamples,
                      0.0f);
            std::fill(micScratchR[static_cast<size_t>(m)].begin(),
                      micScratchR[static_cast<size_t>(m)].begin() + numSamples,
                      0.0f);
        }
        std::fill(reverbAccumL.begin(), reverbAccumL.begin() + numSamples, 0.0f);
        std::fill(reverbAccumR.begin(), reverbAccumR.begin() + numSamples, 0.0f);

        // 2. Voices accumulate directly into per-mic scratch buffers
        for (auto &voicePtr : pool.voices)
        {
            auto &v = *voicePtr;
            if (!v.active)
                continue;

            if (v.instrument < 0 || v.instrument >= instrumentCount)
            {
                v.active = false;
                continue;
            }

            const auto &runtime = instruments_[static_cast<size_t>(v.instrument)];
            if (v.hit < 0 || v.hit >= static_cast<int>(runtime.hits.size()))
            {
                v.active = false;
                continue;
            }

            if (v.endsAt > 0 && endOfBlock > v.endsAt)
            {
                v.active = false;
                continue;
            }

            const auto &hit = runtime.hits[static_cast<size_t>(v.hit)];
            if (v.position >= hit.numFrames)
            {
                v.active = false;
                continue;
            }

            const auto &p = channelParams[static_cast<size_t>(v.instrument)];
            if (p.muted)
                continue;

            float panL = 1.0f, panR = 1.0f;
            if (p.pan != 0.0f)
                panGains(p.pan, panL, panR);
            const float kitBlend = kitBlendForInstrument(v.instrument);
            const float instGainL = v.velocity * p.level * panL * kitBlend;
            const float instGainR = v.velocity * p.level * panR * kitBlend;

            const double ratio = v.pitchRatio;
            const bool shifting = std::abs(ratio - 1.0) > 1.0e-5;

            const auto first = static_cast<size_t>(std::min<int64_t>(v.startSkip, numSamples));
            v.startSkip = std::max<int64_t>(0, v.startSkip - numSamples);
            const bool fading = v.attackSamples > 0 && v.attackGain < 1.0f;

            const int instBus = (v.instrument >= 0 && v.instrument < static_cast<int>(instrumentBuses.size()))
                                    ? instrumentBuses[static_cast<size_t>(v.instrument)]
                                    : 0;
            const bool hasAuxBus = multiOutMode.load() && (instBus > 0) && (instBus < totalBuses)
                                   && (output[instBus * 2] != nullptr) && (output[instBus * 2 + 1] != nullptr);
            float *const auxDestL = hasAuxBus ? output[instBus * 2] : nullptr;
            float *const auxDestR = hasAuxBus ? output[instBus * 2 + 1] : nullptr;

            // Fast path for rate-only conversion (e.g. 44.1 kHz samples in a
            // 48 kHz host): read one contiguous source block per mic and use
            // linear interpolation. Drums do not need cubic interpolation for
            // a pure sample-rate change; this avoids four random reads per
            // output sample on 16/21-mic kits.
            const bool rateOnly = shifting && std::abs(v.pitchSemitones) < 0.01f;
            const auto resamplePos = static_cast<int64_t>(std::floor(v.position));
            const auto sourceFrames =
                static_cast<int>(std::ceil(std::max(0, numSamples - 1) * v.pitchRatio)) + 3;
            const bool shaped = std::abs(v.attackShape) > 0.001f;
            const bool canBlockResample = rateOnly && !fading && v.endsAt == 0 && first == 0 && !hasAuxBus
                                          && !shaped
                                          && sourceFrames > 0
                                          && static_cast<size_t>(sourceFrames) <= v.mixL.size();

            if (canBlockResample)
            {
                struct ChannelPlan
                {
                    const SampleCache::Handle *handle = nullptr;
                    int channel = 0;
                    int mic = 0;
                    float gainL = 0.0f;
                    float gainR = 0.0f;
                };

                ChannelPlan plans[64];
                int planCount = 0;

                for (const auto &channelRef : hit.channels)
                {
                    if (planCount >= static_cast<int>(sizeof(plans) / sizeof(plans[0])))
                        break;

                    const int micIdx = std::clamp(channelRef.kitChannelIndex, 0, numMics - 1);
                    float bleedGain = 1.0f;
                    if (channelRef.isBleed)
                    {
                        bleedGain = bleedMatrix.getEffectiveGain(channelRef.pieceIdx, micIdx)
                                    * channelRef.channelBleedFactor;
                        if (bleedGain <= 0.0f)
                            continue;
                    }

                    auto &plan = plans[planCount++];
                    plan.handle = &channelRef.handle;
                    plan.channel = channelRef.channel;
                    plan.mic = micIdx;
                    plan.gainL = channelRef.gainL * instGainL * bleedGain;
                    plan.gainR = channelRef.gainR * instGainR * bleedGain;
                }

                for (int pIndex = 0; pIndex < planCount; ++pIndex)
                {
                    const auto &plan = plans[pIndex];
                    plan.handle->read(resamplePos, sourceFrames, plan.channel, v.mixL.data());

                    auto *dstL = micScratchL[static_cast<size_t>(plan.mic)].data();
                    auto *dstR = micScratchR[static_cast<size_t>(plan.mic)].data();
                    const double startOffset = v.position - static_cast<double>(resamplePos);

                    for (int i = 0; i < numSamples; ++i)
                    {
                        const double srcPos = startOffset + static_cast<double>(i) * v.pitchRatio;
                        const auto idx = static_cast<int>(srcPos);
                        const auto frac = static_cast<float>(srcPos - static_cast<double>(idx));
                        const float s0 = v.mixL[static_cast<size_t>(idx)];
                        const float s1 = v.mixL[static_cast<size_t>(idx + 1)];
                        const float sample = s0 + frac * (s1 - s0);
                        dstL[i] += sample * plan.gainL;
                        dstR[i] += sample * plan.gainR;
                    }
                }

                v.position += static_cast<double>(numSamples) * v.pitchRatio;
                continue;
            }

            // Fast path for steady one-shots: no pitch shift, no attack ramp,
            // no choke/decay end, and an integer sample position. Read one whole
            // block per mic and mix with vector operations. This is the common
            // case for every sustained tail and is much cheaper on 16/21-mic kits.
            const bool steadyPosition = std::abs(v.position - std::floor(v.position)) < 1.0e-9;
            const bool canBlockMix = !shifting && !fading && v.endsAt == 0 && first == 0 && !hasAuxBus
                                     && !shaped
                                     && steadyPosition
                                     && static_cast<size_t>(numSamples) <= v.mixL.size();

            if (canBlockMix)
            {
                struct ChannelPlan
                {
                    const SampleCache::Handle *handle = nullptr;
                    int channel = 0;
                    int mic = 0;
                    float gainL = 0.0f;
                    float gainR = 0.0f;
                };

                ChannelPlan plans[64];
                int planCount = 0;
                const auto blockPos = static_cast<int64_t>(v.position);

                for (const auto &channelRef : hit.channels)
                {
                    if (planCount >= static_cast<int>(sizeof(plans) / sizeof(plans[0])))
                        break;

                    const int micIdx = std::clamp(channelRef.kitChannelIndex, 0, numMics - 1);
                    float bleedGain = 1.0f;
                    if (channelRef.isBleed)
                    {
                        bleedGain = bleedMatrix.getEffectiveGain(channelRef.pieceIdx, micIdx)
                                    * channelRef.channelBleedFactor;
                        if (bleedGain <= 0.0f)
                            continue;
                    }

                    auto &plan = plans[planCount++];
                    plan.handle = &channelRef.handle;
                    plan.channel = channelRef.channel;
                    plan.mic = micIdx;
                    plan.gainL = channelRef.gainL * instGainL * bleedGain;
                    plan.gainR = channelRef.gainR * instGainR * bleedGain;
                }

                const auto framesLeft = hit.numFrames - blockPos;
                const int blockCount =
                    static_cast<int>(std::clamp<int64_t>(framesLeft, 0, static_cast<int64_t>(numSamples)));

                if (blockCount > 0)
                {
                    for (int pIndex = 0; pIndex < planCount; ++pIndex)
                    {
                        const auto &plan = plans[pIndex];
                        plan.handle->read(blockPos, blockCount, plan.channel, v.mixL.data());

                        auto *dstL = micScratchL[static_cast<size_t>(plan.mic)].data();
                        auto *dstR = micScratchR[static_cast<size_t>(plan.mic)].data();

                        juce::FloatVectorOperations::addWithMultiply(dstL, v.mixL.data(), plan.gainL, blockCount);
                        juce::FloatVectorOperations::addWithMultiply(dstR, v.mixL.data(), plan.gainR, blockCount);
                    }

                    v.position += blockCount;
                    continue;
                }
            }

            for (int i = 0; i < numSamples; ++i)
            {
                if (v.position >= static_cast<double>(hit.numFrames))
                {
                    v.active = false;
                    break;
                }

                if (i < static_cast<int>(first))
                {
                    v.position += ratio;
                    continue;
                }

                float gain = 1.0f;
                if (fading)
                {
                    v.attackGain = std::min(1.0f, v.attackGain + 1.0f / static_cast<float>(v.attackSamples));
                    gain = v.attackGain;
                }

                // Smooth half-cosine choke rampdown
                if (v.endsAt > 0 && v.chokeFadeSamples > 0)
                {
                    const auto elapsed = (sampleClock + i) - v.chokeStartSample;
                    if (elapsed >= v.chokeFadeSamples)
                        gain = 0.0f;
                    else if (elapsed >= 0)
                    {
                        const float t = static_cast<float>(elapsed) / static_cast<float>(v.chokeFadeSamples);
                        gain *= 0.5f * (1.0f + std::cos(static_cast<float>(pi) * t));
                    }
                }
                else if (v.endsAt > 0)
                {
                    const auto remaining = v.endsAt - (sampleClock + i);
                    if (remaining <= 0)
                        gain = 0.0f;
                    else if (remaining < static_cast<int64_t>(currentSampleRate * 0.015))
                    {
                        const float t = static_cast<float>(remaining) / static_cast<float>(currentSampleRate * 0.015);
                        gain *= 0.5f * (1.0f - std::cos(static_cast<float>(pi) * t));
                    }
                }

                if (std::abs(v.attackShape) > 0.001f)
                {
                    const auto elapsed = (sampleClock + i) - v.startedAt;
                    const float t = std::clamp(static_cast<float>(elapsed) / static_cast<float>(v.attackShapeWindow),
                                               0.0f,
                                               1.0f);
                    if (v.attackShape > 0.0f)
                    {
                        gain *= 1.0f + 0.9f * v.attackShape * (1.0f - t);
                    }
                    else
                    {
                        const float soften = -v.attackShape;
                        gain *= 1.0f - 0.75f * soften * (1.0f - t);
                        gain *= 1.0f + 0.35f * soften * t;
                    }
                }

                if (shifting)
                {
                    const int64_t pos = static_cast<int64_t>(v.position);
                    const float t = static_cast<float>(v.position - static_cast<double>(pos));

                    for (const auto &channelRef : hit.channels)
                    {
                        const int micIdx = std::clamp(channelRef.kitChannelIndex, 0, numMics - 1);
                        float bleedGain = 1.0f;
                        if (channelRef.isBleed)
                        {
                            bleedGain = bleedMatrix.getEffectiveGain(channelRef.pieceIdx, micIdx)
                                        * channelRef.channelBleedFactor;
                            if (bleedGain <= 0.0f)
                                continue;
                        }

                        const float y0 = channelRef.handle.readSample(pos - 1, channelRef.channel);
                        const float y1 = channelRef.handle.readSample(pos, channelRef.channel);
                        const float y2 = channelRef.handle.readSample(pos + 1, channelRef.channel);
                        const float y3 = channelRef.handle.readSample(pos + 2, channelRef.channel);

                        const float c1 = 0.5f * (y2 - y0);
                        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
                        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
                        const float sample = ((c3 * t + c2) * t + c1) * t + y1;

                        const float effGain = gain * bleedGain;

                        if (hasAuxBus)
                        {
                            float micLvlL = 1.0f, micLvlR = 1.0f;
                            if (micIdx < numMics)
                            {
                                const auto &mp = micChannels[static_cast<size_t>(micIdx)]->params;
                                float mPanL = 1.0f, mPanR = 1.0f;
                                if (mp.pan < 0.0f)
                                {
                                    mPanL = 1.0f;
                                    mPanR = 1.0f + mp.pan;
                                }
                                else if (mp.pan > 0.0f)
                                {
                                    mPanL = 1.0f - mp.pan;
                                    mPanR = 1.0f;
                                }
                                const float phase = mp.phaseInvert ? -1.0f : 1.0f;
                                micLvlL = mp.level * mPanL * phase;
                                micLvlR = mp.level * mPanR * phase;
                            }
                            auxDestL[i] += sample * channelRef.gainL * instGainL * effGain * micLvlL;
                            auxDestR[i] += sample * channelRef.gainR * instGainR * effGain * micLvlR;
                        }
                        else
                        {
                            micScratchL[static_cast<size_t>(micIdx)][static_cast<size_t>(i)] +=
                                sample * channelRef.gainL * instGainL * effGain;
                            micScratchR[static_cast<size_t>(micIdx)][static_cast<size_t>(i)] +=
                                sample * channelRef.gainR * instGainR * effGain;
                        }
                    }
                }
                else
                {
                    const int64_t pos = static_cast<int64_t>(v.position);

                    for (const auto &channelRef : hit.channels)
                    {
                        const int micIdx = std::clamp(channelRef.kitChannelIndex, 0, numMics - 1);
                        float bleedGain = 1.0f;
                        if (channelRef.isBleed)
                        {
                            bleedGain = bleedMatrix.getEffectiveGain(channelRef.pieceIdx, micIdx)
                                        * channelRef.channelBleedFactor;
                            if (bleedGain <= 0.0f)
                                continue;
                        }

                        const float sample = channelRef.handle.readSample(pos, channelRef.channel);
                        const float effGain = gain * bleedGain;

                        if (hasAuxBus)
                        {
                            float micLvlL = 1.0f, micLvlR = 1.0f;
                            if (micIdx < numMics)
                            {
                                const auto &mp = micChannels[static_cast<size_t>(micIdx)]->params;
                                float mPanL = 1.0f, mPanR = 1.0f;
                                if (mp.pan < 0.0f)
                                {
                                    mPanL = 1.0f;
                                    mPanR = 1.0f + mp.pan;
                                }
                                else if (mp.pan > 0.0f)
                                {
                                    mPanL = 1.0f - mp.pan;
                                    mPanR = 1.0f;
                                }
                                const float phase = mp.phaseInvert ? -1.0f : 1.0f;
                                micLvlL = mp.level * mPanL * phase;
                                micLvlR = mp.level * mPanR * phase;
                            }
                            auxDestL[i] += sample * channelRef.gainL * instGainL * effGain * micLvlL;
                            auxDestR[i] += sample * channelRef.gainR * instGainR * effGain * micLvlR;
                        }
                        else
                        {
                            micScratchL[static_cast<size_t>(micIdx)][static_cast<size_t>(i)] +=
                                sample * channelRef.gainL * instGainL * effGain;
                            micScratchR[static_cast<size_t>(micIdx)][static_cast<size_t>(i)] +=
                                sample * channelRef.gainR * instGainR * effGain;
                        }
                    }
                }

                v.position += ratio;
            }

            if (v.position >= static_cast<double>(hit.numFrames))
                v.active = false;
        }

        // 2b. Acoustic Crosstalk & Sympathetic Snare Wire Simulation (Phase 4)
        for (int m = 0; m < numMics; ++m)
        {
            micScratchLPtrs[static_cast<size_t>(m)] = micScratchL[static_cast<size_t>(m)].data();
            micScratchRPtrs[static_cast<size_t>(m)] = micScratchR[static_cast<size_t>(m)].data();
        }
        renderUserLayerVoices(numSamples);

        crosstalk.render(micScratchLPtrs.data(),
                         micScratchRPtrs.data(),
                         numMics,
                         numSamples,
                         bleedMatrix,
                         snareMicIndex,
                         snareBotMicIndex);

        // 3. Process each mic channel strip (Phase, EQ, Comp, Saturation, Transient, Reverb send)
        bool anyMicSolo = false;
        for (int m = 0; m < numMics; ++m)
        {
            if (micChannels[static_cast<size_t>(m)]->params.solo)
            {
                anyMicSolo = true;
                break;
            }
        }

        for (int m = 0; m < numMics; ++m)
        {
            auto &state = *micChannels[static_cast<size_t>(m)];
            const auto &p = state.params;

            if (p.mute || (anyMicSolo && !p.solo))
            {
                state.peakL.store(state.peakL.load() * 0.85f, std::memory_order_relaxed);
                state.peakR.store(state.peakR.load() * 0.85f, std::memory_order_relaxed);
                state.rmsL.store(0.0f, std::memory_order_relaxed);
                state.rmsR.store(0.0f, std::memory_order_relaxed);
                continue;
            }

            state.fx.process(micScratchL[static_cast<size_t>(m)].data(),
                             micScratchR[static_cast<size_t>(m)].data(),
                             numSamples,
                             p,
                             reverbAccumL.data(),
                             reverbAccumR.data());

            float micPanL = 1.0f, micPanR = 1.0f;
            if (p.pan < 0.0f)
            {
                micPanL = 1.0f;
                micPanR = 1.0f + p.pan;
            }
            else if (p.pan > 0.0f)
            {
                micPanL = 1.0f - p.pan;
                micPanR = 1.0f;
            }
            const float micGainL = p.level * micPanL;
            const float micGainR = p.level * micPanR;

            float blkPeakL = 0.0f, blkPeakR = 0.0f;
            float blkRmsSumL = 0.0f, blkRmsSumR = 0.0f;

            auto *sL = micScratchL[static_cast<size_t>(m)].data();
            auto *sR = micScratchR[static_cast<size_t>(m)].data();

            for (int i = 0; i < numSamples; ++i)
            {
                float valL;
                float valR;

                if (p.mono)
                {
                    // Sum the stereo pair and place it dead centre. This also
                    // fixes kits whose trigger channel only has audio on one side.
                    valL = valR = (sL[i] + sR[i]) * 0.5f * p.level;
                }
                else
                {
                    valL = sL[i] * micGainL;
                    valR = sR[i] * micGainR;
                }

                sL[i] = valL;
                sR[i] = valR;

                blkPeakL = std::max(blkPeakL, std::abs(valL));
                blkPeakR = std::max(blkPeakR, std::abs(valR));
                blkRmsSumL += valL * valL;
                blkRmsSumR += valR * valR;
            }

            const float blkRmsL = std::sqrt(blkRmsSumL / static_cast<float>(numSamples));
            const float blkRmsR = std::sqrt(blkRmsSumR / static_cast<float>(numSamples));
            const float prevL = state.peakL.load(std::memory_order_relaxed);
            const float prevR = state.peakR.load(std::memory_order_relaxed);
            state.peakL.store(std::max(blkPeakL, prevL * 0.88f), std::memory_order_relaxed);
            state.peakR.store(std::max(blkPeakR, prevR * 0.88f), std::memory_order_relaxed);
            state.rmsL.store(blkRmsL, std::memory_order_relaxed);
            state.rmsR.store(blkRmsR, std::memory_order_relaxed);

            // Output bus routing
            const int targetBus = (totalBuses > 1) ? std::clamp(p.outputBus, 0, totalBuses - 1) : 0;
            if (targetBus == 0 || !multiOutMode.load() || p.sendToMaster)
            {
                if (output[0] != nullptr)
                    juce::FloatVectorOperations::add(output[0], sL, numSamples);
                if (output[1] != nullptr)
                    juce::FloatVectorOperations::add(output[1], sR, numSamples);
            }

            if (multiOutMode.load() && totalBuses > 1 && targetBus > 0)
            {
                if (output[targetBus * 2] != nullptr)
                    juce::FloatVectorOperations::add(output[targetBus * 2], sL, numSamples);
                if (output[targetBus * 2 + 1] != nullptr)
                    juce::FloatVectorOperations::add(output[targetBus * 2 + 1], sR, numSamples);
            }
        }

        // 4. Master processing: Reverb return, Master Pan/Level, Soft Limiter
        if (output[0] != nullptr && output[1] != nullptr)
        {
            MasterStripParams effectiveMasterParams = masterStripParams;
            effectiveMasterParams.level *= globals.masterLevel;
            effectiveMasterParams.pan = std::clamp(masterStripParams.pan + globals.masterPan, -1.0f, 1.0f);
            masterFX.process(
                output[0], output[1], numSamples, reverbAccumL.data(), reverbAccumR.data(), effectiveMasterParams);

            // Master metering
            float mPeakL = 0.0f, mPeakR = 0.0f;
            float mRmsSumL = 0.0f, mRmsSumR = 0.0f;
            for (int i = 0; i < numSamples; ++i)
            {
                mPeakL = std::max(mPeakL, std::abs(output[0][i]));
                mPeakR = std::max(mPeakR, std::abs(output[1][i]));
                mRmsSumL += output[0][i] * output[0][i];
                mRmsSumR += output[1][i] * output[1][i];
            }
            const float prevML = masterPeakL.load(std::memory_order_relaxed);
            const float prevMR = masterPeakR.load(std::memory_order_relaxed);
            masterPeakL.store(std::max(mPeakL, prevML * 0.88f), std::memory_order_relaxed);
            masterPeakR.store(std::max(mPeakR, prevMR * 0.88f), std::memory_order_relaxed);
            masterRmsL.store(std::sqrt(mRmsSumL / static_cast<float>(numSamples)), std::memory_order_relaxed);
            masterRmsR.store(std::sqrt(mRmsSumR / static_cast<float>(numSamples)), std::memory_order_relaxed);
        }

        if (multiOutMode.load() && totalBuses > 1 && globals.masterLevel != 1.0f)
        {
            for (int b = 1; b < totalBuses; ++b)
            {
                if (output[b * 2] != nullptr)
                    juce::FloatVectorOperations::multiply(output[b * 2], globals.masterLevel, numSamples);
                if (output[b * 2 + 1] != nullptr)
                    juce::FloatVectorOperations::multiply(output[b * 2 + 1], globals.masterLevel, numSamples);
            }
        }
    }
    else
    {
        // Fallback for kits without microphone channel layout
        float masterL = 0.0f, masterR = 0.0f;
        panGains(globals.masterPan, masterL, masterR);
        const auto masterGainL = globals.masterLevel * masterL;
        const auto masterGainR = globals.masterLevel * masterR;

        for (auto &voicePtr : pool.voices)
        {
            auto &v = *voicePtr;
            if (!v.active)
                continue;
            if (v.instrument < 0 || v.instrument >= instrumentCount)
            {
                v.active = false;
                continue;
            }

            int targetBus = 0;
            if (multiOutMode.load() && totalBuses > 1)
            {
                const auto b = busForInstrument(v.instrument);
                targetBus = (b >= 0 && b < totalBuses) ? b : 0;
            }

            const auto &runtime = instruments_[static_cast<size_t>(v.instrument)];
            if (v.hit < 0 || v.hit >= static_cast<int>(runtime.hits.size()))
            {
                v.active = false;
                continue;
            }
            if (v.endsAt > 0 && endOfBlock > v.endsAt)
            {
                v.active = false;
                continue;
            }

            const auto &hit = runtime.hits[static_cast<size_t>(v.hit)];
            if (v.position >= hit.numFrames)
            {
                v.active = false;
                continue;
            }

            const auto &p = channelParams[static_cast<size_t>(v.instrument)];
            if (p.muted)
                continue;

            std::fill(v.mixL.begin(), v.mixL.begin() + numSamples, 0.0f);
            std::fill(v.mixR.begin(), v.mixR.begin() + numSamples, 0.0f);

            const double ratio = v.pitchRatio;
            const bool shifting = std::abs(ratio - 1.0) > 1.0e-5;

            for (int i = 0; i < numSamples; ++i)
            {
                if (v.position >= static_cast<double>(hit.numFrames))
                    break;
                float sampleL = 0.0f, sampleR = 0.0f;

                if (shifting)
                {
                    const int64_t pos = static_cast<int64_t>(v.position);
                    const float t = static_cast<float>(v.position - static_cast<double>(pos));
                    for (const auto &channelRef : hit.channels)
                    {
                        const float y0 = channelRef.handle.readSample(pos - 1, channelRef.channel);
                        const float y1 = channelRef.handle.readSample(pos, channelRef.channel);
                        const float y2 = channelRef.handle.readSample(pos + 1, channelRef.channel);
                        const float y3 = channelRef.handle.readSample(pos + 2, channelRef.channel);

                        const float c1 = 0.5f * (y2 - y0);
                        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
                        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
                        const float sample = ((c3 * t + c2) * t + c1) * t + y1;

                        sampleL += sample * channelRef.gainL;
                        sampleR += sample * channelRef.gainR;
                    }
                }
                else
                {
                    const int64_t pos = static_cast<int64_t>(v.position);
                    for (const auto &channelRef : hit.channels)
                    {
                        const float sample = channelRef.handle.readSample(pos, channelRef.channel);
                        sampleL += sample * channelRef.gainL;
                        sampleR += sample * channelRef.gainR;
                    }
                }

                v.mixL[static_cast<size_t>(i)] = sampleL;
                v.mixR[static_cast<size_t>(i)] = sampleR;
                v.position += ratio;
            }

            if (v.position >= static_cast<double>(hit.numFrames))
                v.active = false;

            float panL = 1.0f, panR = 1.0f;
            if (p.pan != 0.0f)
                panGains(p.pan, panL, panR);
            const float kitBlend = kitBlendForInstrument(v.instrument);
            const auto gainL = v.velocity * p.level * panL * masterGainL * kitBlend;
            const auto gainR = v.velocity * p.level * panR * masterGainR * kitBlend;

            float *destL = output[targetBus * 2];
            float *destR = output[targetBus * 2 + 1];
            if (destL == nullptr || destR == nullptr)
            {
                destL = output[0];
                destR = output[1];
            }
            if (destL == nullptr || destR == nullptr)
                continue;

            const auto first = static_cast<size_t>(std::min<int64_t>(v.startSkip, numSamples));
            v.startSkip = std::max<int64_t>(0, v.startSkip - numSamples);
            const bool fading = v.attackSamples > 0 && v.attackGain < 1.0f;

            for (int i = static_cast<int>(first); i < numSamples; ++i)
            {
                auto gain = 1.0f;
                if (fading)
                {
                    v.attackGain = std::min(1.0f, v.attackGain + 1.0f / static_cast<float>(v.attackSamples));
                    gain = v.attackGain;
                }

                if (v.endsAt > 0 && v.chokeFadeSamples > 0)
                {
                    const auto elapsed = (sampleClock + i) - v.chokeStartSample;
                    if (elapsed >= v.chokeFadeSamples)
                        gain = 0.0f;
                    else if (elapsed >= 0)
                    {
                        const float t = static_cast<float>(elapsed) / static_cast<float>(v.chokeFadeSamples);
                        gain *= 0.5f * (1.0f + std::cos(static_cast<float>(pi) * t));
                    }
                }
                else if (v.endsAt > 0)
                {
                    const auto remaining = v.endsAt - (sampleClock + i);
                    if (remaining <= 0)
                        gain = 0.0f;
                    else if (remaining < static_cast<int64_t>(currentSampleRate * 0.015))
                    {
                        const float t = static_cast<float>(remaining) / static_cast<float>(currentSampleRate * 0.015);
                        gain *= 0.5f * (1.0f - std::cos(static_cast<float>(pi) * t));
                    }
                }

                if (std::abs(v.attackShape) > 0.001f)
                {
                    const auto elapsed = (sampleClock + i) - v.startedAt;
                    const float t = std::clamp(static_cast<float>(elapsed) / static_cast<float>(v.attackShapeWindow),
                                               0.0f,
                                               1.0f);
                    if (v.attackShape > 0.0f)
                    {
                        gain *= 1.0f + 0.9f * v.attackShape * (1.0f - t);
                    }
                    else
                    {
                        const float soften = -v.attackShape;
                        gain *= 1.0f - 0.75f * soften * (1.0f - t);
                        gain *= 1.0f + 0.35f * soften * t;
                    }
                }

                const auto index = static_cast<size_t>(i);
                destL[i] += v.mixL[index] * gain * gainL;
                destR[i] += v.mixR[index] * gain * gainR;
            }
        }
    }

    sampleClock = endOfBlock;
}

} // namespace glorp
