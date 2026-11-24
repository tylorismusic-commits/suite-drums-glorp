/*
    audio_probe.cpp - headless render check for the sample engine.

    Loads a kit, sends one note and reports the peak level of the main stereo
    output. Used to catch silent-output regressions without a DAW.
*/

#include "PluginProcessor.h"

#include <chrono>
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

    if (std::getenv ("GLORP_PROBE_HHCHOKE_MIDI") != nullptr)
    {
        proc.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buffer (2, 512);
        const bool sameBlock = std::getenv ("GLORP_HHCHOKE_SAME") != nullptr;

        std::cout << "note42=" << (proc.getNameForMidiNoteNumber (42, 1) ? proc.getNameForMidiNoteNumber (42, 1)->toStdString() : "<none>")
                  << " note46=" << (proc.getNameForMidiNoteNumber (46, 1) ? proc.getNameForMidiNoteNumber (46, 1)->toStdString() : "<none>")
                  << " sameBlock=" << (sameBlock ? 1 : 0) << std::endl;

        auto renderTail = [&] (bool addClosed)
        {
            proc.getSampler().allNotesOff();

            double tailSum = 0.0;
            int tailCount = 0;

            for (int block = 0; block < 60; ++block)
            {
                juce::MidiBuffer midi;
                if (block == 0)
                {
                    midi.addEvent (juce::MidiMessage::noteOn (1, 46, static_cast<juce::uint8> (110)), 0);
                    if (addClosed && sameBlock)
                        midi.addEvent (juce::MidiMessage::noteOn (1, 42, static_cast<juce::uint8> (110)), 0);
                }
                else if (! sameBlock && addClosed && block == 10)
                {
                    midi.addEvent (juce::MidiMessage::noteOn (1, 42, static_cast<juce::uint8> (110)), 0);
                }

                buffer.clear();
                proc.processBlock (buffer, midi);

                const auto startSample = block * 512;
                if (startSample >= 14400 && startSample < 28800)
                {
                    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    {
                        const auto* d = buffer.getReadPointer (ch);
                        for (int i = 0; i < buffer.getNumSamples(); ++i)
                        {
                            tailSum += static_cast<double> (d[i]) * d[i];
                            ++tailCount;
                        }
                    }
                }
            }

            const auto rms = std::sqrt (tailSum / std::max (1, tailCount));
            return juce::Decibels::gainToDecibels (static_cast<float> (rms));
        };

        const auto openTailDb = renderTail (false);
        const auto chokedTailDb = renderTail (true);
        std::cout << "midi open tail=" << openTailDb << " dB, with closed note=" << chokedTailDb
                  << " dB, reduction=" << (openTailDb - chokedTailDb) << " dB" << std::endl;
        return 0;
    }

    if (std::getenv ("GLORP_PROBE_HHCHOKE") != nullptr)
    {
        int openIdx = -1;
        int closedIdx = -1;
        for (int i = 0; i < proc.getSampler().numInstruments(); ++i)
        {
            const auto name = proc.getSampler().instrumentName (i);
            const auto lower = name.toLowerCase();
            if (openIdx < 0 && lower.contains ("hat")
                && (lower.contains ("open") || lower.contains ("semi") || lower.contains ("swish"))
                && ! lower.contains ("pedal") && ! lower.contains ("splash"))
                openIdx = i;
            if (closedIdx < 0 && lower.contains ("hat") && lower.contains ("close")
                && ! lower.contains ("open"))
                closedIdx = i;
        }

        std::cout << "open=" << openIdx << " closed=" << closedIdx << std::endl;
        if (openIdx < 0 || closedIdx < 0)
            return 2;

        proc.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buffer (2, 512);

        auto renderTail = [&] (bool addClosed)
        {
            proc.getSampler().allNotesOff();
            proc.getSampler().noteOn (openIdx, 46, 0.95f, proc.getSampler().currentSamplePosition());

            double tailSum = 0.0;
            int tailCount = 0;

            for (int block = 0; block < 60; ++block)
            {
                buffer.clear();
                juce::MidiBuffer noMidi;
                proc.processBlock (buffer, noMidi);

                const auto startSample = block * 512;
                if (addClosed && startSample >= 4800 && startSample < 5312)
                    proc.getSampler().noteOn (closedIdx, 42, 0.95f, proc.getSampler().currentSamplePosition());

                // Measure the open-hat tail after the closed hit has decayed.
                if (startSample >= 14400 && startSample < 28800)
                {
                    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    {
                        const auto* d = buffer.getReadPointer (ch);
                        for (int i = 0; i < buffer.getNumSamples(); ++i)
                        {
                            tailSum += static_cast<double> (d[i]) * d[i];
                            ++tailCount;
                        }
                    }
                }
            }

            const auto rms = std::sqrt (tailSum / std::max (1, tailCount));
            return juce::Decibels::gainToDecibels (static_cast<float> (rms));
        };

        const auto openTailDb = renderTail (false);
        const auto chokedTailDb = renderTail (true);

        std::cout << "open tail=" << openTailDb << " dB, with closed hat=" << chokedTailDb
                  << " dB, reduction=" << (openTailDb - chokedTailDb) << " dB" << std::endl;
        return 0;
    }

    if (const auto* attackEnv = std::getenv ("GLORP_PROBE_ATTACK"))
    {
        const float amount = juce::String (attackEnv).getFloatValue();
        for (int i = 0; i < proc.getSampler().numInstruments(); ++i)
        {
            auto p = proc.getSampler().getInstrumentParams (i);
            p.attackAmount = amount;
            p.damping = 0.0f;
            proc.getSampler().setInstrumentParams (i, p);
        }

        proc.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 38, static_cast<juce::uint8> (110)), 0);
        double early = 0.0, late = 0.0;
        int earlyN = 0, lateN = 0;
        for (int block = 0; block < 120; ++block)
        {
            buffer.clear();
            proc.processBlock (buffer, midi);
            midi.clear();
            const auto start = block * 512;
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const auto* d = buffer.getReadPointer (ch);
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                {
                    const auto pos = start + i;
                    if (pos < 960)
                    {
                        early += static_cast<double> (d[i]) * d[i];
                        ++earlyN;
                    }
                    else if (pos >= 24000 && pos < 48000)
                    {
                        late += static_cast<double> (d[i]) * d[i];
                        ++lateN;
                    }
                }
            }
        }
        std::cout << "attack=" << amount
                  << " earlyDb=" << juce::Decibels::gainToDecibels (static_cast<float> (std::sqrt (early / std::max (1, earlyN))))
                  << " lateDb=" << juce::Decibels::gainToDecibels (static_cast<float> (std::sqrt (late / std::max (1, lateN))))
                  << std::endl;
        return 0;
    }

    if (std::getenv ("GLORP_PROBE_REVERB") != nullptr)
    {
        const bool drySendTest = std::getenv ("GLORP_REVERB_DRY") != nullptr;
        for (int i = 0; i < proc.getSampler().numMicStrips(); ++i)
        {
            auto p = proc.getSampler().getMicStripParams (i);
            p.reverbSend = drySendTest ? 0.0f : 1.0f;
            proc.getSampler().setMicStripParams (i, p);
        }

        auto master = proc.getSampler().getMasterStripParams();
        master.reverbReturn = 1.0f;
        master.reverbSize = 0.9f;
        master.reverbDamp = 0.1f;
        master.reverbWidth = 1.0f;
        proc.getSampler().setMasterStripParams (master);

        proc.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 38, static_cast<juce::uint8> (110)), 0);
        double tailSum = 0.0;
        int tailCount = 0;
        float peak = 0.0f;
        for (int block = 0; block < 120; ++block)
        {
            buffer.clear();
            proc.processBlock (buffer, midi);
            midi.clear();
            const auto start = block * 512;
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const auto* d = buffer.getReadPointer (ch);
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                {
                    peak = std::max (peak, std::abs (d[i]));
                    if (start >= 24000 && start < 48000)
                    {
                        tailSum += static_cast<double> (d[i]) * d[i];
                        ++tailCount;
                    }
                }
            }
        }
        std::cout << "reverb peak=" << peak << " tailRmsDb="
                  << juce::Decibels::gainToDecibels (static_cast<float> (std::sqrt (tailSum / std::max (1, tailCount))))
                  << std::endl;
        return 0;
    }

    if (std::getenv ("GLORP_PROBE_FILTER") != nullptr)
    {
        for (int i = 0; i < proc.getSampler().numMicStrips(); ++i)
        {
            auto p = proc.getSampler().getMicStripParams (i);
            p.hpfHz = 120.0f;
            p.lpfHz = 8000.0f;
            proc.getSampler().setMicStripParams (i, p);
        }

        proc.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 38, static_cast<juce::uint8> (110)), 0);
        float peak = 0.0f;
        for (int block = 0; block < 60; ++block)
        {
            buffer.clear();
            proc.processBlock (buffer, midi);
            midi.clear();
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const auto* d = buffer.getReadPointer (ch);
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                    peak = std::max (peak, std::abs (d[i]));
            }
        }
        std::cout << "filter peak=" << peak << " peakDb=" << juce::Decibels::gainToDecibels (peak) << std::endl;
        return 0;
    }

    if (std::getenv ("GLORP_PROBE_SHAPE") != nullptr)
    {
        for (int i = 0; i < proc.getSampler().numInstruments(); ++i)
        {
            auto p = proc.getSampler().getInstrumentParams (i);
            p.attackMs = 40.0f;
            p.damping = 0.6f;
            proc.getSampler().setInstrumentParams (i, p);
        }

        proc.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 38, static_cast<juce::uint8> (110)), 0);
        float peak = 0.0f;
        for (int block = 0; block < 60; ++block)
        {
            buffer.clear();
            proc.processBlock (buffer, midi);
            midi.clear();
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const auto* d = buffer.getReadPointer (ch);
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                    peak = std::max (peak, std::abs (d[i]));
            }
        }
        std::cout << "shape peak=" << peak << " peakDb=" << juce::Decibels::gainToDecibels (peak) << std::endl;
        return 0;
    }

    if (std::getenv ("GLORP_PROBE_LAYER") != nullptr)
    {
        int kickIndex = -1;
        for (int i = 0; i < proc.getSampler().numInstruments(); ++i)
        {
            const auto n = proc.getSampler().instrumentName (i);
            if (n.containsIgnoreCase ("kick") || n.containsIgnoreCase ("kdrum"))
            {
                kickIndex = i;
                break;
            }
        }

        if (kickIndex < 0)
        {
            std::cout << "layer probe: no kick instrument" << std::endl;
            return 0;
        }

        const auto wav = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("glorp_layer_probe.wav");
        wav.deleteFile();

        {
            juce::WavAudioFormat format;
            auto stream = wav.createOutputStream();
            if (stream != nullptr)
            {
                std::unique_ptr<juce::AudioFormatWriter> writer (
                    format.createWriterFor (stream.get(), 48000.0, 1, 16, {}, 0));

                if (writer != nullptr)
                {
                    stream.release();
                    constexpr int frames = 19200;
                    juce::AudioBuffer<float> tone (1, frames);
                    for (int i = 0; i < frames; ++i)
                    {
                        const float click = i < 40 ? 0.8f * (1.0f - static_cast<float> (i) / 40.0f) : 0.0f;
                        const float body = 0.6f
                                           * std::sin (juce::MathConstants<float>::twoPi * 82.0f
                                                       * static_cast<float> (i) / 48000.0f)
                                           * std::exp (-static_cast<float> (i) / 4200.0f);
                        tone.setSample (0, i, click + body);
                    }
                    writer->writeFromAudioSampleBuffer (tone, 0, frames);
                    writer.reset();
                }
            }
        }

        proc.prepareToPlay (48000.0, 512);

        auto renderKick = [&] (int blocks)
        {
            juce::AudioBuffer<float> buffer (2, 512);
            float peak = 0.0f;
            for (int block = 0; block < blocks; ++block)
            {
                buffer.clear();
                juce::MidiBuffer midi;
                if (block == 0)
                    proc.getSampler().noteOn (kickIndex, 36, 1.0f, proc.getSampler().currentSamplePosition());
                proc.processBlock (buffer, midi);
                peak = std::max (peak, buffer.getMagnitude (0, buffer.getNumSamples()));
            }
            return peak;
        };

        proc.getSampler().allNotesOff();
        const float dryPeak = renderKick (40);

        glorp::UserLayerParams params;
        params.blend = 1.0f;
        params.attackMs = 0.0f;
        params.decayMs = 400.0f;
        params.sustain = 0.35f;
        params.releaseMs = 120.0f;

        juce::String error;
        const bool loaded = proc.getSampler().loadUserLayer (0, wav.getFullPathName(), params, error);
        const int layerMic = proc.getSampler().getUserLayerMicIndex (0);

        proc.getSampler().allNotesOff();
        const float wetPeak = renderKick (40);

        proc.getSampler().autoAlignUserLayer (0);
        const auto aligned = proc.getSampler().getUserLayerParams (0);

        std::cout << "layer loaded=" << loaded << " error=" << error.toStdString()
                  << " mic=" << layerMic
                  << " name=" << (layerMic >= 0 ? proc.getSampler().micStripName (layerMic).toStdString() : "<none>")
                  << "\nlayer dryPeak=" << dryPeak
                  << " wetPeak=" << wetPeak
                  << " layerMicPeak=" << (layerMic >= 0 ? proc.getSampler().getMicPeakL (layerMic) : -1.0f)
                  << "\naligned delayMs=" << aligned.delayMs
                  << " invert=" << (aligned.polarityInvert ? 1 : 0)
                  << " micStrips=" << proc.getSampler().numMicStrips() << std::endl;

        proc.getSampler().clearUserLayer (0);
        std::cout << "after clear micStrips=" << proc.getSampler().numMicStrips()
                  << " mic=" << proc.getSampler().getUserLayerMicIndex (0) << std::endl;
        return 0;
    }

    if (std::getenv ("GLORP_PROBE_LAYERSTATE") != nullptr)
    {
        const auto wav = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("glorp_layer_state_probe.wav");
        wav.deleteFile();

        {
            juce::WavAudioFormat format;
            auto stream = wav.createOutputStream();
            if (stream != nullptr)
            {
                std::unique_ptr<juce::AudioFormatWriter> writer (
                    format.createWriterFor (stream.get(), 48000.0, 1, 16, {}, 0));
                if (writer != nullptr)
                {
                    stream.release();
                    constexpr int frames = 9600;
                    juce::AudioBuffer<float> tone (1, frames);
                    for (int i = 0; i < frames; ++i)
                        tone.setSample (0, i, 0.5f * std::sin (juce::MathConstants<float>::twoPi * 90.0f
                                                               * static_cast<float> (i) / 48000.0f)
                                                   * std::exp (-static_cast<float> (i) / 2400.0f));
                    writer->writeFromAudioSampleBuffer (tone, 0, frames);
                    writer.reset();
                }
            }
        }

        proc.prepareToPlay (48000.0, 512);

        glorp::UserLayerParams params;
        params.blend = 0.63f;
        params.attackMs = 7.0f;
        params.decayMs = 222.0f;
        params.sustain = 0.44f;
        params.releaseMs = 99.0f;
        params.polarityInvert = true;
        params.delayMs = 3.25f;
        params.level = 0.77f;
        params.pan = 0.33f;

        juce::String error;
        const bool loaded = proc.getSampler().loadUserLayer (0, wav.getFullPathName(), params, error);
        const int layerMic = proc.getSampler().getUserLayerMicIndex (0);

        if (layerMic >= 0)
        {
            auto mp = proc.getSampler().getMicStripParams (layerMic);
            mp.hpfHz = 432.0f;
            mp.lpfHz = 12345.0f;
            mp.compEnabled = true;
            mp.compRatio = 3.75f;
            proc.getSampler().setMicStripParams (layerMic, mp);
        }

        juce::MemoryBlock state;
        proc.getStateInformation (state);

        glorp::PluginProcessor restored;
        restored.setStateInformation (state.getData(), static_cast<int> (state.getSize()));

        const int restoredMic = restored.getSampler().getUserLayerMicIndex (0);
        const auto restoredParams = restored.getSampler().getUserLayerParams (0);
        const auto restoredStrip = restoredMic >= 0 ? restored.getSampler().getMicStripParams (restoredMic)
                                                    : glorp::MicStripParams {};

        std::cout << "layerstate loaded=" << loaded << " error=" << error.toStdString()
                  << " mic=" << layerMic << " restoredMic=" << restoredMic
                  << "\nrestored blend=" << restoredParams.blend
                  << " attack=" << restoredParams.attackMs
                  << " decay=" << restoredParams.decayMs
                  << " sustain=" << restoredParams.sustain
                  << " release=" << restoredParams.releaseMs
                  << " delay=" << restoredParams.delayMs
                  << " invert=" << (restoredParams.polarityInvert ? 1 : 0)
                  << "\nrestored level=" << restoredParams.level
                  << " pan=" << restoredParams.pan
                  << " hpf=" << restoredStrip.hpfHz
                  << " lpf=" << restoredStrip.lpfHz
                  << " compRatio=" << restoredStrip.compRatio << std::endl;
        return 0;
    }

    if (std::getenv ("GLORP_PROBE_BUS") != nullptr)
    {
        auto params = proc.getSampler().getMasterStripParams();
        params.busEqLowDb = 3.0f;
        params.busEqMidDb = -2.0f;
        params.busEqHighDb = 1.5f;
        params.busCompEnabled = true;
        params.busCompThresholdDb = -20.0f;
        params.busCompRatio = 4.0f;
        params.busCompMix = 0.5f;
        params.busCompMakeupDb = 2.0f;
        params.busSaturationDrive = 0.2f;
        proc.getSampler().setMasterStripParams (params);

        proc.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 38, static_cast<juce::uint8> (110)), 0);

        float peak = 0.0f;
        for (int block = 0; block < 60; ++block)
        {
            buffer.clear();
            proc.processBlock (buffer, midi);
            midi.clear();
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const auto* d = buffer.getReadPointer (ch);
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                    peak = std::max (peak, std::abs (d[i]));
            }
        }

        std::cout << "bus peak=" << peak << " peakDb=" << juce::Decibels::gainToDecibels (peak) << std::endl;
        return 0;
    }

    if (std::getenv ("GLORP_PROBE_STRESS") != nullptr)
    {
        constexpr double sr = 48000.0;
        constexpr int blockSize = 512;
        const double seconds = 15.0;
        const int blocks = static_cast<int>((seconds * sr) / blockSize);

        proc.prepareToPlay(sr, blockSize);

        if (std::getenv ("GLORP_STRESS_MIXREADY") != nullptr)
        {
            std::vector<juce::String> names;
            for (int i = 0; i < proc.getSampler().numMicStrips(); ++i)
                names.push_back (proc.getSampler().micStripTitle (i));
            proc.getSampler().applyMixerPreset (
                glorp::MixerPreset::createMixReady (static_cast<int>(names.size()), names));
        }

        juce::AudioBuffer<float> buffer (2, blockSize);
        double nextHat = 0.0, nextSnare = 0.0, nextKick = 0.0, nextCrash = 0.0;
        float peak = 0.0f;
        const auto start = std::chrono::steady_clock::now();

        for (int block = 0; block < blocks; ++block)
        {
            juce::MidiBuffer midi;
            const double t = (block * blockSize) / sr;

            while (nextHat <= t)
            {
                midi.addEvent (juce::MidiMessage::noteOn (10, 42, static_cast<juce::uint8> (100)),
                               juce::jlimit (0, blockSize - 1, static_cast<int> ((nextHat - t) * sr)));
                nextHat += 0.04;
            }
            while (nextSnare <= t)
            {
                midi.addEvent (juce::MidiMessage::noteOn (10, 38, static_cast<juce::uint8> (110)),
                               juce::jlimit (0, blockSize - 1, static_cast<int> ((nextSnare - t) * sr)));
                nextSnare += 0.25;
            }
            while (nextKick <= t)
            {
                midi.addEvent (juce::MidiMessage::noteOn (10, 36, static_cast<juce::uint8> (120)),
                               juce::jlimit (0, blockSize - 1, static_cast<int> ((nextKick - t) * sr)));
                nextKick += 0.5;
            }
            while (nextCrash <= t)
            {
                midi.addEvent (juce::MidiMessage::noteOn (10, 49, static_cast<juce::uint8> (115)),
                               juce::jlimit (0, blockSize - 1, static_cast<int> ((nextCrash - t) * sr)));
                nextCrash += 2.0;
            }

            buffer.clear();
            proc.processBlock (buffer, midi);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const auto* d = buffer.getReadPointer (ch);
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                    peak = std::max (peak, std::abs (d[i]));
            }
        }

        const auto elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
        std::cout << "stress audio=" << seconds << "s wall=" << elapsed << "s rtf=" << (elapsed / seconds)
                  << " peak=" << peak << " peakDb=" << juce::Decibels::gainToDecibels (peak)
                  << " activeVoices=" << proc.getSampler().activeVoiceCount()
                  << " stolen=" << proc.getSampler().stolenVoiceCount() << std::endl;
        return 0;
    }

    if (std::getenv ("GLORP_PROBE_HITS") != nullptr)
    {
        proc.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buffer (2, 512);

        for (int vel : { 30, 60, 90, 110, 127 })
        {
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 38, static_cast<juce::uint8> (vel)), 0);

            float peak = 0.0f;
            double sum = 0.0;
            int count = 0;

            for (int block = 0; block < 180; ++block)
            {
                buffer.clear();
                proc.processBlock (buffer, midi);
                midi.clear();

                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                {
                    const auto* d = buffer.getReadPointer (ch);
                    for (int i = 0; i < buffer.getNumSamples(); ++i)
                    {
                        peak = std::max (peak, std::abs (d[i]));
                        sum += static_cast<double> (d[i]) * d[i];
                        ++count;
                    }
                }
            }

            const auto rms = std::sqrt (sum / std::max (1, count));
            std::cout << "vel=" << vel << " peak=" << peak << " peakDb=" << juce::Decibels::gainToDecibels (peak)
                      << " rmsDb=" << juce::Decibels::gainToDecibels (static_cast<float> (rms)) << std::endl;
        }
        return 0;
    }

    if (const auto* presetName = std::getenv ("GLORP_PROBE_PRESET"))
    {
        std::vector<juce::String> names;
        for (int i = 0; i < proc.getSampler().numMicStrips(); ++i)
            names.push_back (proc.getSampler().micStripTitle (i));

        const int n = static_cast<int> (names.size());
        const juce::String requested (presetName);
        glorp::MixerPreset preset;

        if (requested.equalsIgnoreCase ("Default Clean"))
            preset = glorp::MixerPreset::createDefaultClean (n, names);
        else if (requested.equalsIgnoreCase ("Massive Rock"))
            preset = glorp::MixerPreset::createMassiveRock (n, names);
        else if (requested.equalsIgnoreCase ("Vintage Warmth"))
            preset = glorp::MixerPreset::createVintageWarmth (n, names);
        else if (requested.equalsIgnoreCase ("Punchy Studio"))
            preset = glorp::MixerPreset::createPunchyStudio (n, names);
        else if (requested.equalsIgnoreCase ("Dry & Tight"))
            preset = glorp::MixerPreset::createDryTight (n, names);
        else
            preset = glorp::MixerPreset::createMixReady (n, names);

        if (std::getenv ("GLORP_PROBE_PRESET_FULL") != nullptr)
        {
            std::cout << preset.toXmlString().toStdString() << std::endl;
            return 0;
        }

        for (size_t i = 0; i < preset.micStrips.size(); ++i)
        {
            const auto& s = preset.micStrips[i];
            std::cout << names[i].toStdString() << " level=" << s.level << " mono=" << s.mono
                      << " pan=" << s.pan << std::endl;
        }
        return 0;
    }

    if (std::getenv ("GLORP_PROBE_MIXREADY") != nullptr)
    {
        std::vector<juce::String> names;
        for (int i = 0; i < proc.getSampler().numMicStrips(); ++i)
            names.push_back (proc.getSampler().micStripTitle (i));

        auto preset = glorp::MixerPreset::createMixReady (static_cast<int> (names.size()), names);

        for (size_t i = 0; i < preset.micStrips.size(); ++i)
        {
            const auto& s = preset.micStrips[i];
            std::cout << names[i].toStdString() << " level=" << s.level << " mono=" << s.mono
                      << " pan=" << s.pan << std::endl;
        }
        return 0;
    }

    if (std::getenv ("GLORP_PROBE_BUSLINK") != nullptr)
    {
        int snareMic = -1;
        for (int i = 0; i < proc.getSampler().numMicStrips(); ++i)
            if (proc.getSampler().micStripName (i).containsIgnoreCase ("snare"))
                snareMic = i;

        std::cout << "snareMic=" << snareMic << std::endl;
        if (snareMic >= 0)
        {
            proc.setMicOutputBus (snareMic, 2);
            for (int i = 0; i < proc.getSampler().numInstruments(); ++i)
                if (proc.getSampler().instrumentName (i).containsIgnoreCase ("snare"))
                    std::cout << "inst " << i << " " << proc.getSampler().instrumentName (i).toStdString()
                              << " bus=" << proc.getSampler().getInstrumentBus (i) << std::endl;

            proc.setInstrumentOutputBus (11, 3);
            std::cout << "after instrument->mic, snare mic bus=" << proc.getSampler().getMicOutputBus (snareMic) << std::endl;
        }
        return 0;
    }

    if (std::getenv ("GLORP_PROBE_NAMES") != nullptr)
    {
        for (const int note : {36, 38, 42, 46, 48, 49, 51, 52, 53, 55, 57})
        {
            if (const auto name = proc.getNameForMidiNoteNumber (note, 1))
                std::cout << "name " << note << " = " << name->toStdString() << std::endl;
            else
                std::cout << "name " << note << " = <none>" << std::endl;
        }
        return 0;
    }

    if (std::getenv ("GLORP_PROBE_REMAP") != nullptr)
    {
        std::vector<juce::String> names;
        for (int i = 0; i < proc.getSampler().numInstruments(); ++i)
            names.push_back (proc.getSampler().instrumentName (i));

        const auto requestedProfile = std::getenv ("GLORP_PROBE_REMAP_PROFILE") != nullptr
                                          ? juce::String (std::getenv ("GLORP_PROBE_REMAP_PROFILE"))
                                          : juce::String ("General MIDI (extended)");

        for (const auto& preset : glorp::MidiMap::availablePresets())
        {
            if (! preset.name.equalsIgnoreCase (requestedProfile))
                continue;

            glorp::MidiMap temp;
            temp.applyPreset (preset, names);

            for (const auto& [note, sourceName] : preset.noteToInstrumentName)
            {
                const auto inst = temp.instrumentForNote (note);
                if (inst < 0)
                    std::cout << "UNMAPPED source note " << note << " " << sourceName.toStdString() << std::endl;
            }

            for (const int note : {36, 38, 42, 43, 44, 46, 47, 48, 49, 51, 52, 53, 55, 57})
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
