/*
    AudioEngine.h - Audio rendering pipeline and engine management.

    Extracted from PluginProcessor to separate audio processing concerns.
    Handles the core audio thread operations: rendering, MIDI processing,
    and coordination of all audio sub-components.
*/

#pragma once

#include "Config.h"
#include "Sampler.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>

namespace glorp
{

class ParameterManager;
class MidiRouter;

class AudioEngine
{
  public:
    AudioEngine();
    ~AudioEngine() = default;

    AudioEngine(const AudioEngine &) = delete;
    AudioEngine &operator=(const AudioEngine &) = delete;

    // -- Lifecycle ------------------------------------------------------------

    void prepareToPlay(double sampleRate, int samplesPerBlock);
    void releaseResources();

    void sampleRateChanged(double newRate);
    void blockSizeChanged(int);

    // -- Audio rendering ------------------------------------------------------

    // PluginProcessor calls render() directly; processBlock() is owned by the
    // JUCE wrapper so the engine stays independent of host plumbing.

    // -- Sub-component access -------------------------------------------------

    Sampler &getSampler() noexcept { return sampler; }
    const Sampler &getSampler() const noexcept { return sampler; }

    // -- Engine state ---------------------------------------------------------

    double getCurrentSampleRate() const noexcept { return currentSampleRate; }
    int getCurrentBlockSize() const noexcept { return currentBlockSize; }

    /// Returns the current latency in samples (from sampler).
    int getLatencySamples() const noexcept { return sampler.latencySamples(); }

    // -- Core rendering -------------------------------------------------------

    /// Processes audio with parameters, MIDI, and renders to bus outputs.
    /// Call from PluginProcessor::processBlock after setting up bus outputs.
    void render(float *const *busOutputs,
                int numBusOutputs,
                int numSamples,
                juce::MidiBuffer &midi,
                ParameterManager &parameterManager,
                MidiRouter &midiRouter,
                std::atomic<float> &lastHiHatCC4,
                std::atomic<float> &lastSnareCC16);

    // -- Configuration --------------------------------------------------------

    void setMultiOutMode(bool enabled) { sampler.setMultiOutMode(enabled); }
    bool isMultiOutMode() const noexcept { return sampler.isMultiOutMode(); }
    void invalidateParamCache() noexcept { hasCachedParams = false; }

  private:
    // -- Sub-components -------------------------------------------------------
    Sampler sampler;

    // -- Engine state ---------------------------------------------------------
    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;

    // -- Parameter change cache (avoids redundant per-block re-writes) --------
    bool hasCachedParams = false;
    GlobalParams cachedGlobals {};
    std::array<float, config::MAX_SUPPORTED_INSTRUMENTS> cachedLevel {};
    std::array<float, config::MAX_SUPPORTED_INSTRUMENTS> cachedPan {};
    std::array<float, config::MAX_SUPPORTED_INSTRUMENTS> cachedPitch {};
    std::array<float, config::MAX_SUPPORTED_INSTRUMENTS> cachedDynamics {};
    std::array<int, config::MAX_SUPPORTED_INSTRUMENTS> cachedBus {};

};

} // namespace glorp