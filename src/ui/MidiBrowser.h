/*
    MidiBrowser.h - PACK / SONG / LOOP MIDI library browser.

    The browser scans a user-selected folder of MIDI files arranged as
    Pack/Song/*.mid, remaps General MIDI drum notes to the current kit mapping,
    auditions loops through the sampler, and exports a remapped .mid file that
    can be dragged into the host.
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

class MidiBrowser : public juce::Component, private juce::Timer
{
  public:
    explicit MidiBrowser(PluginProcessor &processor);
    ~MidiBrowser() override;

    void setTheme(const Theme &theme);
    void refreshLibrary();
    void paint(juce::Graphics &g) override;
    void resized() override;

    /// Writes the current loop as a remapped MIDI file and returns its path.
    juce::File exportSelectedLoop() { return exportSelected(); }

    std::function<void()> onCloseRequested;
    std::function<void(juce::Component *, const juce::File &)> onExportDrag;

  private:
    class Column;

    class DragButton : public juce::TextButton
    {
      public:
        explicit DragButton(const juce::String &name) : juce::TextButton(name) {}

        std::function<void(juce::Component *)> onDragStart;

        void mouseDown(const juce::MouseEvent &e) override
        {
            dragStarted = false;
            juce::TextButton::mouseDown(e);
        }

        void mouseDrag(const juce::MouseEvent &e) override
        {
            if (!dragStarted && e.getDistanceFromDragStart() > 4)
            {
                dragStarted = true;
                if (onDragStart)
                    onDragStart(this);
            }
            juce::TextButton::mouseDrag(e);
        }

        void mouseUp(const juce::MouseEvent &e) override
        {
            dragStarted = false;
            juce::TextButton::mouseUp(e);
        }

      private:
        bool dragStarted = false;
    };

    struct SongEntry
    {
        juce::String name;
        juce::File directory;
        std::vector<juce::File> loops;
    };

    struct PackEntry
    {
        juce::String name;
        juce::File directory;
        std::vector<SongEntry> songs;
    };

    void timerCallback() override;
    juce::File libraryRoot() const;
    void chooseLibrary();
    void selectPack(int index);
    void selectSong(int index);
    void selectLoop(int index);
    void auditionSelected();
    void pauseAudition();
    void resumeAudition();
    void stopAudition();
    void updateTransportButtons();
    struct Remap
    {
        std::array<int, 128> instrument {{-1}};
        std::array<int, 128> targetNote {{-1}};
        bool useChannel10 = false;
        int mappedNotes = 0;
        int unmappedNotes = 0;
    };

    bool buildRemap(const juce::File &source, Remap &remap) const;
    juce::File exportSelected();
    void startExportDrag(juce::Component *source);
    void setStatus(const juce::String &text, bool warning = false);

    PluginProcessor &processor;
    Theme theme = Theme::suite();

    juce::File root;
    std::vector<PackEntry> packs;

    std::unique_ptr<Column> packColumn;
    std::unique_ptr<Column> songColumn;
    std::unique_ptr<Column> loopColumn;

    juce::TextButton chooseButton {"CHOOSE LIBRARY..."};
    juce::TextButton rescanButton {"RESCAN"};
    juce::TextButton closeButton {"BACK TO KIT"};
    juce::ComboBox sourceBox;
    juce::TextButton playButton {"PLAY"};
    juce::TextButton pauseButton {"PAUSE"};
    juce::TextButton stopButton {"STOP"};
    juce::TextButton loopButton {"LOOP"};
    DragButton exportButton {"DRAG MIDI"};

    int selectedPack = -1;
    int selectedSong = -1;
    int selectedLoop = -1;
    juce::File selectedLoopFile;

    struct AuditionEvent
    {
        double timeSeconds = 0.0;
        int instrument = -1;
        int note = -1;
        float velocity = 0.8f;
    };

    std::vector<AuditionEvent> auditionEvents;
    int nextAuditionEvent = 0;
    double auditionStartMs = 0.0;
    bool auditioning = false;
    bool auditionPaused = false;
    double auditionLengthSeconds = 0.0;
    double auditionLoopSeconds = 0.0;
    double pausedAtSeconds = 0.0;

    juce::String statusText;
    bool statusIsWarning = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiBrowser)
};

} // namespace glorp
