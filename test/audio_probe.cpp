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
