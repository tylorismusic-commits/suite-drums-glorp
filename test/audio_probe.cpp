/*
    audio_probe.cpp - headless render check for the sample engine.

    Loads a kit, sends one note and reports the peak level of the main stereo
    output. Used to catch silent-output regressions without a DAW.
*/

#include "PluginProcessor.h"

#include <iostream>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;

    glorp::PluginProcessor proc;
    juce::String kitPath = argc > 1 ? juce::String (argv[1])
                                    : juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                          .getChildFile ("SuiteDrumsGlorp/kits/GlorpDemoKit/drumkit.xml")
                                          .getFullPathName();

    std::cout << "Loading kit: " << kitPath.toStdString() << std::endl;
    proc.loadKit (kitPath);
    std::cout << "Load error: " << proc.getLastKitError().toStdString() << std::endl;
    std::cout << "Instruments: " << proc.getSampler().numInstruments()
              << ", mics: " << proc.getCurrentKit().channels.size() << std::endl;

    if (const auto* midiPath = std::getenv ("GLORP_PROBE_MIDI"))
    {
        juce::MidiFile mf;
        const juce::File midiFile = juce::File (juce::String (midiPath));
        juce::FileInputStream in (midiFile);

        if (in.openedOk() && mf.readFrom (in))
        {
            for (int tr = 0; tr < mf.getNumTracks(); ++tr)
            {
                const auto* seq = mf.getTrack (tr);
                for (int ev = 0; ev < seq->getNumEvents(); ++ev)
                {
                    const auto& m = seq->getEventPointer (ev)->message;
                    if (m.isNoteOn() || m.isNoteOff())
                        std::cout << "midi tr=" << tr << " t=" << seq->getEventPointer (ev)->message.getTimeStamp()
                                  << " ch=" << m.getChannel() << " note=" << m.getNoteNumber()
                                  << " vel=" << m.getVelocity() << " on=" << m.isNoteOn() << std::endl;
                }
            }
        }
        return 0;
    }

    if (std::getenv ("GLORP_PROBE_MONO") != nullptr)
    {
        int triggerIndex = -1;
        for (int i = 0; i < proc.getSampler().numMicStrips(); ++i)
            if (proc.getSampler().micStripName (i).containsIgnoreCase ("trigger"))
                triggerIndex = i;

        int kickIndex = -1;
        for (int i = 0; i < proc.getSampler().numInstruments(); ++i)
        {
            std::cout << "inst " << i << " " << proc.getSampler().instrumentName (i).toStdString() << std::endl;
            if (proc.getSampler().instrumentName (i).containsIgnoreCase ("kick")
                || proc.getSampler().instrumentName (i).containsIgnoreCase ("kdrum"))
            {
                kickIndex = i;
                break;
            }
        }

        std::cout << "trigger=" << triggerIndex << " kick=" << kickIndex << std::endl;

        if (triggerIndex >= 0 && kickIndex >= 0)
        {
            proc.prepareToPlay (48000.0, 512);
            juce::AudioBuffer<float> buffer (32, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 36, static_cast<juce::uint8> (110)), 0);
            proc.processBlock (buffer, midi);
            std::cout << "before mono L=" << proc.getSampler().getMicPeakL (triggerIndex)
                      << " R=" << proc.getSampler().getMicPeakR (triggerIndex) << std::endl;

            auto params = proc.getSampler().getMicStripParams (triggerIndex);
            params.mono = true;
            proc.getSampler().setMicStripParams (triggerIndex, params);

            midi.addEvent (juce::MidiMessage::noteOn (1, 36, static_cast<juce::uint8> (110)), 0);
            proc.processBlock (buffer, midi);
            std::cout << "after mono L=" << proc.getSampler().getMicPeakL (triggerIndex)
                      << " R=" << proc.getSampler().getMicPeakR (triggerIndex) << std::endl;
        }
        return 0;
    }

    if (std::getenv ("GLORP_PROBE_REMAP") != nullptr)
    {
        std::vector<juce::String> names;
        for (int i = 0; i < proc.getSampler().numInstruments(); ++i)
            names.push_back (proc.getSampler().instrumentName (i));

        for (const auto& preset : glorp::MidiMap::availablePresets())
        {
            if (! preset.name.equalsIgnoreCase ("General MIDI (extended)"))
                continue;

            glorp::MidiMap temp;
            temp.applyPreset (preset, names);

            for (const auto& [note, sourceName] : preset.noteToInstrumentName)
            {
                const auto inst = temp.instrumentForNote (note);
                if (inst < 0)
                    std::cout << "UNMAPPED source note " << note << " " << sourceName.toStdString() << std::endl;
            }

            for (const int note : {47, 50, 52, 55, 59, 39, 54, 56})
            {
                const auto inst = temp.instrumentForNote (note);
                std::cout << "remap note " << note << " -> inst " << inst;
                if (inst >= 0)
                    std::cout << " " << proc.getSampler().instrumentName (inst).toStdString();
                std::cout << std::endl;
            }
            break;
        }
    }

    if (std::getenv ("GLORP_PROBE_MAP") != nullptr)
    {
        for (int i = 0; i < proc.getSampler().numInstruments(); ++i)
        {
            const auto slots = proc.getMidiMap().noteSlotsForInstrument (i);
            std::cout << i << " " << proc.getSampler().instrumentName (i).toStdString()
                      << " -> " << slots[0] << "," << slots[1] << "," << slots[2] << std::endl;
        }
    }

    if (std::getenv ("GLORP_PROBE_SELFTEST") != nullptr)
    {
        auto &map = proc.getMidiMap();
        map.assignNote (36, 0);

        if (! map.setNoteSlot (0, 1, 50) || ! map.setNoteSlot (0, 2, 51))
        {
            std::cout << "SELFTEST: could not fill three note slots" << std::endl;
            return 2;
        }

        const auto slots = map.noteSlotsForInstrument (0);
        std::cout << "SELFTEST slots: " << slots[0] << ", " << slots[1] << ", " << slots[2] << std::endl;

        if (slots[0] != 36 || slots[1] != 50 || slots[2] != 51)
            return 3;

        map.setNoteSlot (0, 1, -1);
        if (map.noteSlotsForInstrument (0)[1] != -1)
            return 4;

        auto params = proc.getSampler().getInstrumentParams (0);
        params.useCustomVelocityCurve = true;
        params.customVelocityCurve.fill (1.0f);
        proc.getSampler().setInstrumentParams (0, params);

        if (! proc.getSampler().getInstrumentParams (0).useCustomVelocityCurve)
            return 5;

        std::cout << "SELFTEST before state: " << map.noteSlotsForInstrument (0)[0] << ", "
                  << map.noteSlotsForInstrument (0)[1] << ", " << map.noteSlotsForInstrument (0)[2] << std::endl;

        juce::MemoryBlock state;
        proc.getStateInformation (state);
        proc.setStateInformation (state.getData(), static_cast<int> (state.getSize()));

        std::cout << "SELFTEST after state: " << proc.getMidiMap().noteSlotsForInstrument (0)[0] << ", "
                  << proc.getMidiMap().noteSlotsForInstrument (0)[1] << ", "
                  << proc.getMidiMap().noteSlotsForInstrument (0)[2] << std::endl;

        if (! proc.getSampler().getInstrumentParams (0).useCustomVelocityCurve)
            return 6;

        std::cout << "SELFTEST: MIDI slots, velocity curve and state OK" << std::endl;
    }

    proc.prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> buffer (32, 512);
    juce::MidiBuffer midi;

    const int note = proc.getMidiMap().noteForInstrument (0);
    std::cout << "Triggering instrument 0, note " << note << std::endl;
    midi.addEvent (juce::MidiMessage::noteOn (1, note, static_cast<juce::uint8> (110)), 0);

    proc.processBlock (buffer, midi);

    float peak = 0.0f;

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        peak = std::max (peak, buffer.getMagnitude (ch, 0, buffer.getNumSamples()));

    std::cout << "Main output peak: " << peak << std::endl;
    return peak > 1.0e-5f ? 0 : 1;
}
