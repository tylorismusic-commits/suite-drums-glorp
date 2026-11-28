/*
    PluginProcessor.h - the JUCE bridge.

    Buses
    -----
    One MIDI input, and GLORP_MAX_INSTRUMENT_BUSES stereo outputs with no audio
    input. The output buses cannot change while the plugin is loaded: an LV2
    bundle's port list is baked into its .ttl when the host scans the plugin, and
    JUCE's LV2 wrapper has no way to renegotiate it afterwards. VST3 could, but
    having the two formats behave differently would be worse than a few idle
    buses. Unused buses stay silent and are named for whatever kit is loaded.
*/

#pragma once

#include "AudioEngine.h"
#include "KitScanner.h"
#include "MidiMap.h"
#include "MidiRouter.h"
#include "ParameterManager.h"
#include "Settings.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include "Config.h"

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <string>

namespace glorp
{

class PluginProcessor : public juce::AudioProcessor
{
  public:
    static constexpr int maxSupportedInstruments = config::MAX_SUPPORTED_INSTRUMENTS;
    static constexpr int maxOutputBuses = config::MAX_OUTPUT_BUSES;

    PluginProcessor();
    ~PluginProcessor() override;

    // -- juce::AudioProcessor ----------------------------------------------
    const juce::String getName() const override { return JucePlugin_Name; }
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    bool isBusesLayoutSupported(const BusesLayout &layouts) const override;
    bool canAddBus(bool /*isInput*/) const override { return false; }
    bool canRemoveBus(bool /*isInput*/) const override { return false; }

    void processBlock(juce::AudioBuffer<float> &, juce::MidiBuffer &) override;

    /// VST3 hosts use these names for the piano-roll keys.
    std::optional<juce::String> getNameForMidiNoteNumber(int note, int midiChannel) override;

    juce::AudioProcessorEditor *createEditor() override;
    bool hasEditor() const override { return true; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String &) override {}

    void getStateInformation(juce::MemoryBlock &) override;
    void setStateInformation(const void *, int) override;

    // -- hosts -------------------------------------------------------------
    void sampleRateChanged(double);
    void blockSizeChanged(int);

    juce::AudioProcessorParameter *getParameterForMidiControl(int cc, int channel);

    juce::AudioProcessorParameter *findParameter(const juce::String &paramID) const;

    // -- Suite Drums Glorp --------------------------------------------------
    Sampler &getSampler() noexcept { return audioEngine.getSampler(); }
    MidiMap &getMidiMap() noexcept { return midiRouter.getMidiMap(); }
    Settings &getSettings() noexcept { return settings; }
    AudioEngine &getAudioEngine() noexcept { return audioEngine; }
    const AudioEngine &getAudioEngine() const noexcept { return audioEngine; }

    /// The stable identifier a knob uses when it appears in a MIDI map.
    static juce::String controlIdForInstrument(const juce::String &suffix, int index);

    void loadKit(const juce::String &drumkitPath);
    void loadKitImpl(const juce::String &drumkitPath);

    /// Routes a mixer mic strip and its matching drum instruments together.
    void setMicOutputBus(int micStripIndex, int bus);
    void setInstrumentOutputBus(int instrumentIndex, int bus);
    void refreshKitList();

    const std::vector<KitEntry> &getKitList() const noexcept { return kitList; }
    juce::String getCurrentKitPath() const { return currentKitPath; }
    const DrumKit &getCurrentKit() const noexcept { return currentKit; }

    /// Applies a MIDI-map preset to the loaded kit.
    void applyMapPreset(const juce::String &presetName);

    // -- MIDI thru (no-kit mapper) -----------------------------------------
    /// When enabled, incoming notes are remapped from the source profile to the
    /// selected destination profile and sent to the plugin's MIDI output. The
    /// kit, if any, still plays locally.
    void setMidiThruEnabled(bool enabled);
    bool isMidiThruEnabled() const noexcept { return midiThruEnabled.load(std::memory_order_relaxed); }

    void setMidiThruDestination(const juce::String &presetName);
    juce::String getMidiThruDestination() const { return midiThruDestination; }

    /// True when there is no sample kit and MIDI Thru is driving another instrument.
    bool isMidiThruNoKit() const noexcept
    {
        return midiThruEnabled.load(std::memory_order_relaxed)
               && audioEngine.getSampler().numInstruments() == 0;
    }

    /// Instrument names the MIDI editor should show. With no kit loaded this is
    /// the active MIDI Thru destination profile rather than an empty list.
    int getMidiMapInstrumentCount() const noexcept;
    juce::String getMidiMapInstrumentName(int index) const;
    std::vector<juce::String> getMidiMapInstrumentNames() const;

    /// Destination note for a source-map note, or -1 when no matching
    /// destination instrument exists.
    int getMidiThruMappedNote(int sourceNote) const noexcept;

    /// Recomputes the source -> destination note map. Call after changing the
    /// source or destination profile.
    void rebuildMidiThruMap();

    /// Queues a note for the MIDI output. Used by the loop browser when the
    /// plugin is acting as a no-kit MIDI mapper.
    void queueMidiThruNote(int note, float velocity, bool noteOn);
    void queueMidiThruAllNotesOff();

    /// Unloads the current kit so the plugin can be used as a pure MIDI mapper.
    void closeKit();

    /// Saves the current MIDI map to the user preset folder (defaults to "Custom")
    /// and selects it as the active preset. Returns the saved file path.
    juce::String saveCustomMapPreset(const juce::String &presetName = "Custom");

    /// Suggests a default name when prompting the user to save the active mapping.
    juce::String suggestedPresetSaveName() const;

    /// E-drum continuous controls
    float getHiHatCC4() const noexcept { return lastHiHatCC4.load(std::memory_order_relaxed); }
    float getSnarePositionCC16() const noexcept { return lastSnareCC16.load(std::memory_order_relaxed); }

    /// Tempo/transport snapshot from the last processBlock, used by the loop browser.
    double getHostBpm() const noexcept { return hostBpm.load(std::memory_order_relaxed); }
    double getHostPpqPosition() const noexcept { return hostPpqPosition.load(std::memory_order_relaxed); }
    bool isHostPlaying() const noexcept { return hostIsPlaying.load(std::memory_order_relaxed); }

    /// Reads the host playhead directly, so the loop browser follows the
    /// project tempo even before the host has processed another block.
    double getCurrentHostBpm() const;
    double getCurrentHostPpqPosition() const;
    bool getCurrentHostIsPlaying() const;

    /// Notifies the editor that kit-dependent state changed.
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void kitChanged() = 0;
        virtual void parametersChanged() = 0;
        virtual void busRoutingChanged() {}
    };

    using juce::AudioProcessor::addListener;
    using juce::AudioProcessor::removeListener;
    void addListener(Listener *l) { listeners.add(l); }
    void removeListener(Listener *l) { listeners.remove(l); }

    /// Latest kit load error, for the UI to show.
    juce::String getLastKitError() const { return lastKitError; }

    /// Sets a named parameter and notifies the editor, used by MIDI learn.
    void setControlValue(const juce::String &controlId, float normalised);

    /// Called from the editor's timer so MIDI-learned CCs reach the host
    /// parameters on the message thread rather than the audio thread.
    void drainControlQueue();

  private:
    static BusesProperties createBusesProperties();
    void notifyParametersChanged();

    ParameterManager parameterManager;
    MidiRouter midiRouter;
    AudioEngine audioEngine;

    Settings settings;

    std::atomic<bool> midiThruEnabled {false};
    std::array<std::atomic<int>, 128> midiThruMap;
    std::map<int, int> midiThruActive; // (channel << 8) | input note -> output note
    juce::String midiThruDestination {"Ugritone KVLT (native)"};
    std::vector<juce::String> midiThruVirtualInstrumentNames;
    juce::MidiMessageCollector midiThruCollector;
    std::atomic<double> midiThruSampleRate {0.0};

    std::vector<KitEntry> kitList;
    juce::String currentKitPath;
    juce::String lastKitError;
    DrumKit currentKit;

    std::atomic<float> lastHiHatCC4 {0.0f};
    std::atomic<float> lastSnareCC16 {0.5f};
    std::atomic<double> hostBpm {120.0};
    std::atomic<double> hostPpqPosition {0.0};
    std::atomic<bool> hostIsPlaying {false};

    juce::ListenerList<Listener> listeners;

    juce::UndoManager *undoManager = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginProcessor)
};

} // namespace glorp
