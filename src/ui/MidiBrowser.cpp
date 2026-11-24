#include "MidiBrowser.h"

#include "Config.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace glorp
{

namespace
{
bool isMidiFile(const juce::File &file)
{
    const auto ext = file.getFileExtension().toLowerCase();
    return ext == ".mid" || ext == ".midi";
}

juce::String displayNameForFile(const juce::File &file)
{
    return file.getFileNameWithoutExtension();
}

double fileBpm(const juce::MidiFile &midi)
{
    for (int track = 0; track < midi.getNumTracks(); ++track)
    {
        if (const auto *sequence = midi.getTrack(track))
        {
            for (int event = 0; event < sequence->getNumEvents(); ++event)
            {
                const auto &message = sequence->getEventPointer(event)->message;

                if (message.isTempoMetaEvent())
                {
                    const auto secondsPerQuarter = message.getTempoSecondsPerQuarterNote();

                    if (secondsPerQuarter > 0.0)
                        return 60.0 / secondsPerQuarter;
                }
            }
        }
    }

    return 120.0;
}

juce::String sanitise(const juce::String &text)
{
    return text.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")
        .trim();
}
} // namespace

//==============================================================================
class MidiBrowser::Column : public juce::Component, private juce::ListBoxModel
{
  public:
    Column(MidiBrowser &ownerIn, juce::String titleIn) : owner(ownerIn), title(std::move(titleIn))
    {
        list.setModel(this);
        list.setRowHeight(25);
        list.setMultipleSelectionEnabled(false);
        list.setOutlineThickness(0);
        list.setColour(juce::ListBox::backgroundColourId, owner.theme.trough);
        list.setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
        addAndMakeVisible(list);
    }

    void setTheme(const Theme &t)
    {
        theme = t;
        list.setColour(juce::ListBox::backgroundColourId, theme.trough);
        repaint();
    }

    void setItems(juce::StringArray newItems)
    {
        items = std::move(newItems);
        suppressCallback = true;
        list.updateContent();
        if (items.isEmpty())
            list.deselectAllRows();
        else
            list.selectRow(0);
        suppressCallback = false;
        repaint();

        if (onSelectionChanged)
            onSelectionChanged(items.isEmpty() ? -1 : 0);
    }

    void clearSelection()
    {
        list.deselectAllRows();
    }

    int getSelectedRow() const { return list.getSelectedRow(); }

    void selectRow(int row)
    {
        if (row < 0 || row >= items.size())
            return;

        suppressCallback = true;
        list.selectRow(row);
        suppressCallback = false;

        if (onSelectionChanged)
            onSelectionChanged(row);
    }

    int getNumRows() override { return items.size(); }

    void paintListBoxItem(int row, juce::Graphics &g, int width, int height, bool rowIsSelected) override
    {
        if (row < 0 || row >= items.size())
            return;

        if (rowIsSelected)
        {
            g.setColour(theme.accent.withAlpha(0.20f));
            g.fillRoundedRectangle(2.0f, 1.0f, static_cast<float>(width) - 4.0f, static_cast<float>(height) - 2.0f, 4.0f);
            g.setColour(theme.accent);
            g.drawRoundedRectangle(2.5f, 1.5f, static_cast<float>(width) - 5.0f, static_cast<float>(height) - 3.0f, 4.0f, 1.0f);
        }

        g.setColour(rowIsSelected ? theme.ink : theme.inkSoft);
        g.setFont(uiFont(11.0f));
        g.drawFittedText(items[row],
                         juce::Rectangle<int>(10, 0, width - 20, height),
                         juce::Justification::centredLeft,
                         1,
                         0.82f);
    }

    void selectedRowsChanged(int row) override
    {
        if (!suppressCallback && onSelectionChanged)
            onSelectionChanged(row);
    }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent &) override
    {
        if (row >= 0 && onDoubleClick)
            onDoubleClick(row);
    }

    void paint(juce::Graphics &g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(theme.panel);
        g.fillRoundedRectangle(b, 6.0f);
        g.setColour(theme.panelEdge);
        g.drawRoundedRectangle(b.reduced(0.5f), 6.0f, 1.0f);

        g.setColour(theme.inkSoft);
        g.setFont(displayFont(11.0f));
        g.drawText(title, juce::Rectangle<int>(10, 4, getWidth() - 20, 18), juce::Justification::centredLeft, false);

        g.setColour(theme.accent);
        g.setFont(uiFont(9.0f).boldened());
        g.drawText(juce::String(items.size()), juce::Rectangle<int>(getWidth() - 42, 4, 30, 18),
                   juce::Justification::centredRight, false);

        g.setColour(theme.panelEdge);
        g.drawHorizontalLine(24, 6.0f, static_cast<float>(getWidth() - 6));
    }

    void resized() override { list.setBounds(6, 28, getWidth() - 12, getHeight() - 36); }

    std::function<void(int)> onSelectionChanged;
    std::function<void(int)> onDoubleClick;

  private:
    MidiBrowser &owner;
    Theme theme = Theme::suite();
    juce::String title;
    juce::StringArray items;
    juce::ListBox list;
    bool suppressCallback = false;
};

//==============================================================================
MidiBrowser::MidiBrowser(PluginProcessor &processorIn) : processor(processorIn)
{
    setOpaque(true);

    packColumn = std::make_unique<Column>(*this, "PACK");
    songColumn = std::make_unique<Column>(*this, "SONG");
    loopColumn = std::make_unique<Column>(*this, "LOOP");
    addAndMakeVisible(*packColumn);
    addAndMakeVisible(*songColumn);
    addAndMakeVisible(*loopColumn);

    packColumn->onSelectionChanged = [this](int row) { selectPack(row); };
    songColumn->onSelectionChanged = [this](int row) { selectSong(row); };
    loopColumn->onSelectionChanged = [this](int row) { selectLoop(row); };
    loopColumn->onDoubleClick = [this](int row)
    {
        selectLoop(row);
        auditionSelected();
    };

    addAndMakeVisible(chooseButton);
    addAndMakeVisible(rescanButton);
    addAndMakeVisible(closeButton);
    addAndMakeVisible(sourceBox);
    addAndMakeVisible(speedBox);
    addAndMakeVisible(playButton);
    addAndMakeVisible(pauseButton);
    addAndMakeVisible(stopButton);
    addAndMakeVisible(loopButton);
    addAndMakeVisible(exportButton);

    chooseButton.onClick = [this] { chooseLibrary(); };
    rescanButton.onClick = [this] { refreshLibrary(true, true); };
    closeButton.onClick = [this]
    {
        stopAudition();
        if (onCloseRequested)
            onCloseRequested();
    };
    playButton.onClick = [this] { auditionSelected(); };
    pauseButton.onClick = [this]
    {
        if (auditionPaused)
            resumeAudition();
        else
            pauseAudition();
    };
    stopButton.onClick = [this] { stopAudition(); };
    loopButton.setClickingTogglesState(true);
    loopButton.onClick = [this] { updateTransportButtons(); };
    sourceBox.onChange = [this]
    {
        processor.getSettings().midiSourceProfile = sourceBox.getText();
        processor.getSettings().markDirty();
        processor.getSettings().saveIfChanged();
        setStatus("Source map: " + sourceBox.getText());
    };

    speedBox.addItem("0.5x", 1);
    speedBox.addItem("1.0x", 2);
    speedBox.addItem("2.0x", 3);
    speedBox.setSelectedId(2, juce::dontSendNotification);
    speedBox.onChange = [this]
    {
        const auto text = speedBox.getText();
        playbackSpeed = text.startsWith("0.5") ? 0.5 : (text.startsWith("2") ? 2.0 : 1.0);
        processor.getSettings().midiPreviewSpeed = playbackSpeed;
        processor.getSettings().markDirty();
        processor.getSettings().saveIfChanged();
        setStatus("Speed: " + text);
    };
    exportButton.onClick = [this] { setStatus("Drag the DRAG MIDI button into your DAW."); };
    exportButton.onDragStart = [this](juce::Component *source) { startExportDrag(source); };

    updateTransportButtons();

    setTheme(theme);
    refreshLibrary();
}

MidiBrowser::~MidiBrowser()
{
    stopAudition();
}

void MidiBrowser::setTheme(const Theme &newTheme)
{
    theme = newTheme;
    packColumn->setTheme(theme);
    songColumn->setTheme(theme);
    loopColumn->setTheme(theme);

    for (auto *button : {&chooseButton,
                         &rescanButton,
                         &closeButton,
                         &playButton,
                         &pauseButton,
                         &stopButton,
                         &loopButton,
                         static_cast<juce::TextButton *>(&exportButton)})
    {
        button->setColour(juce::TextButton::textColourOffId, theme.ink);
        button->setColour(juce::TextButton::textColourOnId, theme.background);
        button->setColour(juce::TextButton::buttonColourId, theme.panel);
        button->setColour(juce::TextButton::buttonOnColourId, theme.accent);
    }

    sourceBox.setColour(juce::ComboBox::backgroundColourId, theme.panel);
    sourceBox.setColour(juce::ComboBox::textColourId, theme.ink);
    sourceBox.setColour(juce::ComboBox::outlineColourId, theme.panelEdge);
    sourceBox.setColour(juce::ComboBox::arrowColourId, theme.accent);
    sourceBox.setColour(juce::ComboBox::focusedOutlineColourId, theme.accent);

    speedBox.setColour(juce::ComboBox::backgroundColourId, theme.panel);
    speedBox.setColour(juce::ComboBox::textColourId, theme.ink);
    speedBox.setColour(juce::ComboBox::outlineColourId, theme.panelEdge);
    speedBox.setColour(juce::ComboBox::arrowColourId, theme.accent);
    speedBox.setColour(juce::ComboBox::focusedOutlineColourId, theme.accent);

    repaint();
}

juce::File MidiBrowser::libraryRoot() const
{
    const auto &settings = processor.getSettings();

    if (settings.midiLibraryPath.isNotEmpty())
        return juce::File(settings.midiLibraryPath);

    return settings.getConfigDirectory().getChildFile("midi_library");
}

void MidiBrowser::refreshLibrary(bool preserveSelection, bool stopPlayback)
{
    if (stopPlayback)
        stopAudition();

    const juce::String previousPack =
        selectedPack >= 0 && selectedPack < static_cast<int>(packs.size()) ? packs[static_cast<size_t>(selectedPack)].name : juce::String();
    const juce::String previousSong =
        selectedPack >= 0 && selectedPack < static_cast<int>(packs.size())
                && selectedSong >= 0 && selectedSong < static_cast<int>(packs[static_cast<size_t>(selectedPack)].songs.size())
            ? packs[static_cast<size_t>(selectedPack)].songs[static_cast<size_t>(selectedSong)].name
            : juce::String();
    const juce::String previousLoop = selectedLoopFile.getFileNameWithoutExtension();

    root = libraryRoot();
    root.createDirectory();

    sourceBox.clear(juce::dontSendNotification);

    int sourceId = 0;
    int selectedSourceId = 0;
    const auto &sourceName = processor.getSettings().midiSourceProfile;

    for (const auto &preset : MidiMap::availablePresets())
    {
        ++sourceId;
        sourceBox.addItem(preset.name, sourceId);

        if (preset.name.equalsIgnoreCase(sourceName))
            selectedSourceId = sourceId;
    }

    if (selectedSourceId == 0)
    {
        for (int i = 1; i <= sourceBox.getNumItems(); ++i)
        {
            if (sourceBox.getItemText(i - 1).equalsIgnoreCase("General MIDI (extended)"))
            {
                selectedSourceId = i;
                break;
            }
        }
    }

    if (selectedSourceId == 0 && sourceBox.getNumItems() > 0)
        selectedSourceId = 1;

    sourceBox.setSelectedId(selectedSourceId, juce::dontSendNotification);

    playbackSpeed = juce::jlimit(0.5, 2.0, processor.getSettings().midiPreviewSpeed);
    speedBox.setSelectedId(playbackSpeed < 0.75 ? 1 : (playbackSpeed > 1.5 ? 3 : 2), juce::dontSendNotification);

    packs.clear();
    selectedPack = -1;
    selectedSong = -1;
    selectedLoop = -1;
    selectedLoopFile = juce::File();

    auto scanLoops = [](const juce::File &directory)
    {
        std::vector<juce::File> files;

        for (const auto &file : directory.findChildFiles(juce::File::findFiles, false, "*.mid"))
            if (isMidiFile(file))
                files.push_back(file);

        for (const auto &file : directory.findChildFiles(juce::File::findFiles, false, "*.midi"))
            if (isMidiFile(file))
                files.push_back(file);

        std::sort(files.begin(),
                  files.end(),
                  [](const juce::File &a, const juce::File &b)
                  { return a.getFileName().compareIgnoreCase(b.getFileName()) < 0; });

        return files;
    };

    for (const auto &entry : juce::RangedDirectoryIterator(root, false, "*", juce::File::findDirectories))
    {
        PackEntry pack;
        pack.name = entry.getFile().getFileName();
        pack.directory = entry.getFile();

        for (const auto &songEntry :
             juce::RangedDirectoryIterator(pack.directory, false, "*", juce::File::findDirectories))
        {
            auto loops = scanLoops(songEntry.getFile());

            if (!loops.empty())
            {
                SongEntry song;
                song.name = songEntry.getFile().getFileName();
                song.directory = songEntry.getFile();
                song.loops = std::move(loops);
                pack.songs.push_back(std::move(song));
            }
        }

        auto looseLoops = scanLoops(pack.directory);

        if (!looseLoops.empty())
        {
            SongEntry song;
            song.name = "(root)";
            song.directory = pack.directory;
            song.loops = std::move(looseLoops);
            pack.songs.push_back(std::move(song));
        }

        if (!pack.songs.empty())
        {
            std::sort(pack.songs.begin(),
                      pack.songs.end(),
                      [](const SongEntry &a, const SongEntry &b)
                      { return a.name.compareIgnoreCase(b.name) < 0; });
            packs.push_back(std::move(pack));
        }
    }

    // A flat library with MIDI files directly in the root still works.
    auto rootLoops = scanLoops(root);
    if (!rootLoops.empty())
    {
        PackEntry pack;
        pack.name = root.getFileName().isNotEmpty() ? root.getFileName() : "Library";
        pack.directory = root;

        SongEntry song;
        song.name = "(root)";
        song.directory = root;
        song.loops = std::move(rootLoops);
        pack.songs.push_back(std::move(song));
        packs.push_back(std::move(pack));
    }

    std::sort(packs.begin(),
              packs.end(),
              [](const PackEntry &a, const PackEntry &b) { return a.name.compareIgnoreCase(b.name) < 0; });

    juce::StringArray packNames;
    for (const auto &pack : packs)
        packNames.add(pack.name);

    if (packs.empty())
    {
        packColumn->setItems({});
        songColumn->setItems({});
        loopColumn->setItems({});
    }
    else
    {
        packColumn->setItems(packNames);

        if (preserveSelection && previousPack.isNotEmpty())
        {
            for (int i = 0; i < static_cast<int>(packs.size()); ++i)
            {
                if (packs[static_cast<size_t>(i)].name == previousPack)
                {
                    packColumn->selectRow(i);
                    break;
                }
            }

            if (previousSong.isNotEmpty() && selectedPack >= 0
                && selectedPack < static_cast<int>(packs.size()))
            {
                const auto &songs = packs[static_cast<size_t>(selectedPack)].songs;

                for (int i = 0; i < static_cast<int>(songs.size()); ++i)
                {
                    if (songs[static_cast<size_t>(i)].name == previousSong)
                    {
                        songColumn->selectRow(i);
                        break;
                    }
                }

                if (previousLoop.isNotEmpty() && selectedSong >= 0
                    && selectedSong < static_cast<int>(songs.size()))
                {
                    const auto &loops = songs[static_cast<size_t>(selectedSong)].loops;

                    for (int i = 0; i < static_cast<int>(loops.size()); ++i)
                    {
                        if (loops[static_cast<size_t>(i)].getFileNameWithoutExtension() == previousLoop)
                        {
                            loopColumn->selectRow(i);
                            break;
                        }
                    }
                }
            }
        }
    }

    if (packs.empty())
        setStatus("No MIDI packs found in " + root.getFullPathName() + ". Use CHOOSE LIBRARY.", true);
    else
        setStatus("Library: " + root.getFullPathName());
}

void MidiBrowser::refreshIfNeeded()
{
    const auto desiredRoot = libraryRoot();

    if (packs.empty() || root != desiredRoot)
    {
        refreshLibrary(false, false);
        return;
    }

    repaint();
}

void MidiBrowser::chooseLibrary()
{
    auto chooser = std::make_shared<juce::FileChooser>("Choose MIDI library folder", root, "");
    auto safeThis = juce::Component::SafePointer<MidiBrowser>(this);

    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                         [safeThis, chooser](const juce::FileChooser &fc)
                         {
                             if (safeThis == nullptr)
                                 return;

                             const auto result = fc.getResult();

                             if (result.isDirectory())
                             {
                                 safeThis->processor.getSettings().midiLibraryPath = result.getFullPathName();
                                 safeThis->processor.getSettings().markDirty();
                                 safeThis->processor.getSettings().saveIfChanged();
                                 safeThis->refreshLibrary(false, true);
                             }
                         });
}

void MidiBrowser::selectPack(int index)
{
    selectedPack = index;
    selectedSong = -1;
    selectedLoop = -1;
    selectedLoopFile = juce::File();
    stopAudition();

    juce::StringArray songs;
    if (index >= 0 && index < static_cast<int>(packs.size()))
        for (const auto &song : packs[static_cast<size_t>(index)].songs)
            songs.add(song.name);

    songColumn->setItems(songs);
}

void MidiBrowser::selectSong(int index)
{
    selectedSong = index;
    selectedLoop = -1;
    selectedLoopFile = juce::File();
    stopAudition();

    juce::StringArray loops;

    if (selectedPack >= 0 && selectedPack < static_cast<int>(packs.size()))
    {
        const auto &songs = packs[static_cast<size_t>(selectedPack)].songs;

        if (index >= 0 && index < static_cast<int>(songs.size()))
            for (const auto &loop : songs[static_cast<size_t>(index)].loops)
                loops.add(displayNameForFile(loop));
    }

    loopColumn->setItems(loops);
}

void MidiBrowser::selectLoop(int index)
{
    selectedLoop = index;
    selectedLoopFile = juce::File();

    if (selectedPack >= 0 && selectedPack < static_cast<int>(packs.size()))
    {
        const auto &songs = packs[static_cast<size_t>(selectedPack)].songs;

        if (selectedSong >= 0 && selectedSong < static_cast<int>(songs.size()))
        {
            const auto &loops = songs[static_cast<size_t>(selectedSong)].loops;

            if (index >= 0 && index < static_cast<int>(loops.size()))
            {
                selectedLoopFile = loops[static_cast<size_t>(index)];
                setStatus("Selected: " + selectedLoopFile.getFileName());
            }
        }
    }
}

bool MidiBrowser::buildRemap(const juce::File &source, Remap &remap) const
{
    remap.instrument.fill(-1);
    remap.targetNote.fill(-1);
    remap.mappedNotes = 0;
    remap.unmappedNotes = 0;

    juce::MidiFile midi;
    juce::FileInputStream stream(source);

    if (!stream.openedOk() || !midi.readFrom(stream))
    {
        return false;
    }

    std::vector<juce::String> kitNames;
    for (int i = 0; i < processor.getSampler().numInstruments(); ++i)
        kitNames.push_back(processor.getSampler().instrumentName(i));

    const auto requestedSource = sourceBox.getText().isNotEmpty()
                                     ? sourceBox.getText()
                                     : processor.getSettings().midiSourceProfile;
    const MapPreset *sourcePreset = nullptr;
    std::vector<MapPreset> presets = MidiMap::availablePresets();

    for (const auto &preset : presets)
        if (preset.name.equalsIgnoreCase(requestedSource))
            sourcePreset = &preset;

    if (sourcePreset == nullptr)
        for (const auto &preset : presets)
            if (preset.name.equalsIgnoreCase("General MIDI (extended)"))
                sourcePreset = &preset;

    if (sourcePreset == nullptr)
        for (const auto &preset : presets)
            if (preset.name.equalsIgnoreCase("General MIDI"))
                sourcePreset = &preset;

    if (sourcePreset == nullptr)
        return false;

    MidiMap sourceMap;
    sourceMap.applyPreset(*sourcePreset, kitNames);

    for (int note = 0; note < 128; ++note)
    {
        const int instrument = sourceMap.instrumentForNote(note);

        if (instrument < 0 || instrument >= processor.getSampler().numInstruments())
            continue;

        const int target = processor.getMidiMap().noteForInstrument(instrument);

        if (target >= 0)
        {
            remap.instrument[static_cast<size_t>(note)] = instrument;
            remap.targetNote[static_cast<size_t>(note)] = target;
            ++remap.mappedNotes;
        }
        else
        {
            ++remap.unmappedNotes;
        }
    }

    // Prefer the GM drum channel if the file uses it. Melodic-only files fall
    // back to all channels so percussion sketches still audition.
    for (int track = 0; track < midi.getNumTracks(); ++track)
    {
        if (const auto *sequence = midi.getTrack(track))
        {
            for (int event = 0; event < sequence->getNumEvents(); ++event)
            {
                const auto message = sequence->getEventPointer(event)->message;

                if (message.isNoteOn() && message.getChannel() == 10)
                {
                    remap.useChannel10 = true;
                    break;
                }
            }
        }

        if (remap.useChannel10)
            break;
    }

    return true;
}

void MidiBrowser::auditionSelected()
{
    if (auditionPaused)
    {
        resumeAudition();
        return;
    }

    if (!selectedLoopFile.existsAsFile())
    {
        setStatus("Select a loop first.", true);
        return;
    }

    stopAudition();

    Remap remap;
    if (!buildRemap(selectedLoopFile, remap))
    {
        setStatus("Could not read MIDI file.", true);
        return;
    }

    juce::MidiFile midi;
    juce::FileInputStream stream(selectedLoopFile);

    if (!stream.openedOk() || !midi.readFrom(stream))
    {
        setStatus("Could not read MIDI file.", true);
        return;
    }

    const double sourceBpm = fileBpm(midi);
    const double hostBpm = std::clamp(processor.getCurrentHostBpm(), 20.0, 300.0);
    const double speed = std::clamp(playbackSpeed, 0.5, 2.0);
    const double tempoScale = (sourceBpm / hostBpm) / speed;

    midi.convertTimestampTicksToSeconds();

    auditionEvents.clear();
    auditionLengthSeconds = 0.0;

    for (int track = 0; track < midi.getNumTracks(); ++track)
    {
        const auto *sequence = midi.getTrack(track);

        if (sequence == nullptr)
            continue;

        for (int event = 0; event < sequence->getNumEvents(); ++event)
        {
            const auto &message = sequence->getEventPointer(event)->message;

            if (!message.isNoteOn() || message.getVelocity() <= 0)
                continue;

            if (remap.useChannel10 && message.getChannel() != 10)
                continue;

            const auto note = message.getNoteNumber();
            const auto instrument = remap.instrument[static_cast<size_t>(note)];
            const auto target = remap.targetNote[static_cast<size_t>(note)];

            if (instrument < 0 || target < 0)
                continue;

            AuditionEvent audition;
            audition.timeSeconds =
                sequence->getEventPointer(event)->message.getTimeStamp() * tempoScale;
            audition.instrument = instrument;
            audition.note = target;
            audition.velocity = message.getFloatVelocity();
            auditionEvents.push_back(audition);
            auditionLengthSeconds = std::max(auditionLengthSeconds, audition.timeSeconds);
        }
    }

    std::sort(auditionEvents.begin(),
              auditionEvents.end(),
              [](const AuditionEvent &a, const AuditionEvent &b) { return a.timeSeconds < b.timeSeconds; });

    if (auditionEvents.empty())
    {
        setStatus("No source-map notes matched the current kit mapping.", true);
        return;
    }

    auditionLoopSeconds = std::max(0.25, auditionLengthSeconds);
    auditionLengthSeconds = auditionLoopSeconds + 2.0;
    nextAuditionEvent = 0;
    auditionPaused = false;
    pausedAtSeconds = 0.0;

    double startDelayMs = 0.0;

    if (processor.getCurrentHostIsPlaying())
    {
        const double ppq = processor.getCurrentHostPpqPosition();
        const double phase = ppq - std::floor(ppq);
        double beatsToNext = 1.0 - phase;

        if (beatsToNext < 0.05)
            beatsToNext += 1.0;

        startDelayMs = beatsToNext * 60000.0 / hostBpm;
    }

    auditionStartMs = juce::Time::getMillisecondCounterHiRes() + startDelayMs;
    auditioning = true;
    startTimerHz(120);
    updateTransportButtons();
    setStatus("Playing " + selectedLoopFile.getFileName() + " at " + juce::String(hostBpm, 1)
              + " BPM, " + juce::String(speed, 1) + "x (source " + sourceBox.getText() + ")");
}

void MidiBrowser::pauseAudition()
{
    if (!auditioning || auditionPaused)
        return;

    pausedAtSeconds = (juce::Time::getMillisecondCounterHiRes() - auditionStartMs) / 1000.0;
    auditionPaused = true;
    stopTimer();
    processor.getSampler().allNotesOff();
    updateTransportButtons();
    setStatus("Paused.");
}

void MidiBrowser::resumeAudition()
{
    if (!auditioning || !auditionPaused)
        return;

    auditionStartMs = juce::Time::getMillisecondCounterHiRes() - pausedAtSeconds * 1000.0;
    auditionPaused = false;
    startTimerHz(120);
    updateTransportButtons();
    setStatus("Playing.");
}

void MidiBrowser::stopAudition()
{
    stopTimer();
    auditioning = false;
    auditionPaused = false;
    nextAuditionEvent = 0;
    pausedAtSeconds = 0.0;
    processor.getSampler().allNotesOff();
    updateTransportButtons();
    setStatus("Stopped.");
}

void MidiBrowser::updateTransportButtons()
{
    pauseButton.setButtonText(auditionPaused ? "RESUME" : "PAUSE");
    pauseButton.setEnabled(auditioning);
    stopButton.setEnabled(auditioning || auditionPaused);
    repaint();
}

void MidiBrowser::timerCallback()
{
    if (!auditioning || auditionPaused)
        return;

    double elapsed = (juce::Time::getMillisecondCounterHiRes() - auditionStartMs) / 1000.0;

    if (loopButton.getToggleState() && elapsed >= auditionLoopSeconds)
    {
        auditionStartMs += auditionLoopSeconds * 1000.0;
        elapsed -= auditionLoopSeconds;
        nextAuditionEvent = 0;
    }

    while (nextAuditionEvent < static_cast<int>(auditionEvents.size())
           && auditionEvents[static_cast<size_t>(nextAuditionEvent)].timeSeconds <= elapsed)
    {
        const auto &event = auditionEvents[static_cast<size_t>(nextAuditionEvent)];
        processor.getSampler().noteOn(event.instrument,
                                      event.note,
                                      event.velocity,
                                      processor.getSampler().currentSamplePosition());
        ++nextAuditionEvent;
    }

    if (!loopButton.getToggleState() && elapsed > auditionLengthSeconds)
    {
        auditioning = false;
        stopTimer();
        updateTransportButtons();
        setStatus("Audition finished.");
    }
}

juce::File MidiBrowser::exportSelected()
{
    if (!selectedLoopFile.existsAsFile())
    {
        setStatus("Select a loop first.", true);
        return {};
    }

    Remap remap;
    if (!buildRemap(selectedLoopFile, remap))
    {
        setStatus("Could not read MIDI file.", true);
        return {};
    }

    if (remap.mappedNotes == 0)
    {
        setStatus("No source-map notes matched the current kit mapping.", true);
        return {};
    }

    juce::MidiFile input;
    juce::FileInputStream inputStream(selectedLoopFile);

    if (!inputStream.openedOk() || !input.readFrom(inputStream))
    {
        setStatus("Could not read MIDI file.", true);
        return {};
    }

    juce::MidiFile output;
    output.setTicksPerQuarterNote(input.getTimeFormat() > 0 ? input.getTimeFormat() : 960);

    const double hostBpm = std::clamp(processor.getCurrentHostBpm(), 20.0, 300.0);
    const double speed = std::clamp(playbackSpeed, 0.5, 2.0);
    const int microsecondsPerQuarter = juce::roundToInt(60000000.0 / hostBpm);
    std::cout << "export speed=" << speed << " sourceBpm=" << fileBpm(input) << " hostBpm=" << hostBpm << std::endl;
    int exportedNotes = 0;

    for (int track = 0; track < input.getNumTracks(); ++track)
    {
        const auto *sequence = input.getTrack(track);
        juce::MidiMessageSequence outTrack;

        if (track == 0)
            outTrack.addEvent(juce::MidiMessage::tempoMetaEvent(microsecondsPerQuarter), 0.0);

        if (sequence != nullptr)
        {
            for (int event = 0; event < sequence->getNumEvents(); ++event)
            {
                const auto *eventPointer = sequence->getEventPointer(event);
                auto message = eventPointer->message;

                if (message.isTempoMetaEvent())
                    continue;

                if ((message.isNoteOn() || message.isNoteOff())
                    && (!remap.useChannel10 || message.getChannel() == 10))
                {
                    const auto sourceNote = message.getNoteNumber();
                    const auto target = remap.targetNote[static_cast<size_t>(sourceNote)];

                    if (target < 0)
                        continue;

                    message.setNoteNumber(target);
                    ++exportedNotes;
                }

                // MidiMessageSequence::addEvent adds the supplied offset to the
                // message's existing timestamp, so clear it first.
                message.setTimeStamp(0.0);
                outTrack.addEvent(message, eventPointer->message.getTimeStamp() / speed);
            }

            outTrack.updateMatchedPairs();
        }

        output.addTrack(outTrack);
    }

    const auto exportDir = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                               .getChildFile(".cache")
                               .getChildFile(config::CONFIG_DIR_NAME)
                               .getChildFile("midi_exports");
    exportDir.createDirectory();

    juce::String baseName = "loop";

    if (selectedPack >= 0 && selectedPack < static_cast<int>(packs.size()))
        baseName = sanitise(packs[static_cast<size_t>(selectedPack)].name) + "_" + sanitise(selectedLoopFile.getFileNameWithoutExtension());

    if (baseName.isEmpty())
        baseName = "loop";

    auto targetFile = exportDir.getChildFile(baseName + ".mid");
    targetFile.deleteFile();

    if (auto outputStream = targetFile.createOutputStream())
        output.writeTo(*outputStream);

    if (!targetFile.existsAsFile())
    {
        setStatus("Could not write exported MIDI.", true);
        return {};
    }

    setStatus("Export ready: " + juce::String(exportedNotes) + " notes -> " + targetFile.getFileName());
    return targetFile;
}

void MidiBrowser::startExportDrag(juce::Component *source)
{
    const auto file = exportSelected();

    if (file.existsAsFile() && onExportDrag)
        onExportDrag(source, file);
}

void MidiBrowser::setStatus(const juce::String &text, bool warning)
{
    statusText = text;
    statusIsWarning = warning;
    repaint();
}

void MidiBrowser::paint(juce::Graphics &g)
{
    g.fillAll(theme.background);

    g.setColour(theme.ink);
    g.setFont(displayFont(30.0f));
    g.drawText("MIDI LOOP BROWSER", juce::Rectangle<int>(20, 8, 360, 34), juce::Justification::centredLeft, false);

    g.setColour(theme.inkSoft);
    g.setFont(uiFont(10.5f));
    g.drawFittedText(root.getFullPathName(),
                     juce::Rectangle<int>(20, 41, getWidth() - 420, 15),
                     juce::Justification::centredLeft,
                     1,
                     0.8f);

    g.setColour(theme.accent);
    g.setFont(uiFont(9.5f).boldened());
    g.drawText("SYNC: " + juce::String(std::clamp(processor.getCurrentHostBpm(), 20.0, 300.0), 1) + " BPM  |  "
                   + juce::String(std::clamp(playbackSpeed, 0.5, 2.0), 1) + "x",
               getWidth() - 320,
               41,
               160,
               15,
               juce::Justification::centredRight,
               false);

    g.setColour(theme.inkSoft);
    g.setFont(uiFont(8.5f).boldened());
    g.drawText("SOURCE MAP", juce::Rectangle<int>(16, getHeight() - 71, 100, 12),
               juce::Justification::centredLeft, false);
    g.drawText("SPEED", juce::Rectangle<int>(204, getHeight() - 71, 64, 12),
               juce::Justification::centredLeft, false);

    g.setColour(statusIsWarning ? theme.warning : theme.inkSoft);
    g.setFont(uiFont(10.0f));
    g.drawFittedText(statusText,
                     juce::Rectangle<int>(20, getHeight() - 21, getWidth() - 40, 16),
                     juce::Justification::centredLeft,
                     1,
                     0.8f);

    g.setColour(theme.panelEdge);
    g.drawHorizontalLine(60, 20.0f, static_cast<float>(getWidth() - 20));
    g.drawHorizontalLine(getHeight() - 84, 20.0f, static_cast<float>(getWidth() - 20));
}

void MidiBrowser::resized()
{
    auto b = getLocalBounds();

    closeButton.setBounds(getWidth() - 146, 12, 128, 28);
    chooseButton.setBounds(getWidth() - 146 - 8 - 146, 12, 146, 28);
    rescanButton.setBounds(chooseButton.getX() - 8 - 86, 12, 86, 28);

    auto columns = b.withTrimmedTop(68).withTrimmedBottom(100).reduced(16, 0);
    const int gap = 10;
    const int columnWidth = std::max(120, (columns.getWidth() - gap * 2) / 3);

    packColumn->setBounds(columns.removeFromLeft(columnWidth));
    columns.removeFromLeft(gap);
    songColumn->setBounds(columns.removeFromLeft(columnWidth));
    columns.removeFromLeft(gap);
    loopColumn->setBounds(columns.removeFromLeft(columnWidth));

    const int buttonY = getHeight() - 52;
    sourceBox.setBounds(16, buttonY, 180, 28);
    speedBox.setBounds(204, buttonY, 64, 28);
    playButton.setBounds(276, buttonY, 58, 28);
    pauseButton.setBounds(342, buttonY, 68, 28);
    stopButton.setBounds(418, buttonY, 56, 28);
    loopButton.setBounds(482, buttonY, 56, 28);
    exportButton.setBounds(546, buttonY, 104, 28);
}

} // namespace glorp
