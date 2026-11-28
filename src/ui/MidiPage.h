/*
    MidiPage.h - full-page note mapping and velocity response editor.

    Each sampler hit gets three MIDI note slots. Notes can be typed/selected or
    learned from a pad, and the eight-point velocity curve is editable directly.
*/

#pragma once

#include "PluginProcessor.h"
#include "Theme.h"

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace glorp
{

class MidiPage : public juce::Component, private juce::Timer
{
  public:
    explicit MidiPage(PluginProcessor &processor);
    ~MidiPage() override;

    void setTheme(const Theme &theme);
    void refresh();
    void paint(juce::Graphics &g) override;
    void resized() override;

    void noteSlotChanged(int instrumentIndex, int slot, int note);
    void armNoteLearn(int instrumentIndex, int slot);
    void cancelNoteLearn();
    void curveChanged(int instrumentIndex, const std::array<float, 8> &points);

    std::function<void()> onCloseRequested;

  private:
    class HitRow;

    void timerCallback() override;
    void rebuildRows();
    void refreshRows();
    void syncScrollContent();

    PluginProcessor &processor;
    Theme theme = Theme::suite();

    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<HitRow>> rows;
    juce::TextButton closeButton {"BACK TO KIT"};

    int learningInstrument = -1;
    int learningSlot = -1;
    int lastAssignmentCount = -1;
    int lastInstrumentCount = -1;
    bool lastLearningState = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiPage)
};

} // namespace glorp
