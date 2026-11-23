#include "MidiMap.h"
#include "Config.h"

#include <algorithm>

namespace glorp
{

namespace
{
juce::File gUserPresetDirectoryOverride;
}

MidiMap::MidiMap() = default;

//==============================================================================
bool MidiMap::assignNote(int note, int instrumentIndex, bool steal)
{
    if (note < 0 || note > 127 || instrumentIndex < 0)
        return false;

    for (auto it = noteToInstrument.begin(); it != noteToInstrument.end();)
    {
        if (it->second == instrumentIndex)
            it = noteToInstrument.erase(it);
        else
            ++it;
    }

    instrumentToNotes[instrumentIndex] = {{-1, -1, -1}};
    return addNote(note, instrumentIndex, steal);
}

bool MidiMap::addNote(int note, int instrumentIndex, bool steal)
{
    if (note < 0 || note > 127 || instrumentIndex < 0)
        return false;

    auto [entry, inserted] = instrumentToNotes.try_emplace(instrumentIndex, std::array<int, 3> {{-1, -1, -1}});
    auto &slots = entry->second;
    (void) inserted;

    for (const auto slot : slots)
        if (slot == note)
            return true;

    int freeSlot = -1;
    for (int i = 0; i < static_cast<int>(slots.size()); ++i)
    {
        if (slots[static_cast<size_t>(i)] < 0)
        {
            freeSlot = i;
            break;
        }
    }

    if (freeSlot < 0)
        return false;

    if (const auto existing = noteToInstrument.find(note);
        existing != noteToInstrument.end() && existing->second != instrumentIndex)
    {
        if (!steal)
            return false;

        // Remove the note from the instrument that currently owns it.
        if (auto owner = instrumentToNotes.find(existing->second); owner != instrumentToNotes.end())
            for (auto &slot : owner->second)
                if (slot == note)
                    slot = -1;
    }

    noteToInstrument[note] = instrumentIndex;
    slots[static_cast<size_t>(freeSlot)] = note;

    if (onChanged)
        onChanged();

    return true;
}

bool MidiMap::setNoteSlot(int instrumentIndex, int slot, int note, bool steal)
{
    if (instrumentIndex < 0 || slot < 0 || slot >= 3 || note < -1 || note > 127)
        return false;

    auto [entry, inserted] = instrumentToNotes.try_emplace(instrumentIndex, std::array<int, 3> {{-1, -1, -1}});
    auto &slots = entry->second;
    (void) inserted;
    const auto oldNote = slots[static_cast<size_t>(slot)];

    if (oldNote == note)
        return true;

    if (oldNote >= 0 && noteToInstrument.count(oldNote) > 0
        && noteToInstrument[oldNote] == instrumentIndex)
        noteToInstrument.erase(oldNote);

    slots[static_cast<size_t>(slot)] = -1;

    if (note < 0)
    {
        if (onChanged)
            onChanged();
        return true;
    }

    // If the note is already in another slot on this instrument, move it here.
    for (int i = 0; i < static_cast<int>(slots.size()); ++i)
        if (slots[static_cast<size_t>(i)] == note)
            slots[static_cast<size_t>(i)] = -1;

    if (const auto existing = noteToInstrument.find(note);
        existing != noteToInstrument.end() && existing->second != instrumentIndex)
    {
        if (!steal)
        {
            slots[static_cast<size_t>(slot)] = oldNote;
            if (oldNote >= 0)
                noteToInstrument[oldNote] = instrumentIndex;
            return false;
        }

        if (auto owner = instrumentToNotes.find(existing->second); owner != instrumentToNotes.end())
            for (auto &ownerSlot : owner->second)
                if (ownerSlot == note)
                    ownerSlot = -1;
    }

    slots[static_cast<size_t>(slot)] = note;
    noteToInstrument[note] = instrumentIndex;

    if (onChanged)
        onChanged();

    return true;
}

std::array<int, 3> MidiMap::noteSlotsForInstrument(int instrumentIndex) const
{
    const auto it = instrumentToNotes.find(instrumentIndex);
    if (it != instrumentToNotes.end())
        return it->second;

    return {{-1, -1, -1}};
}

void MidiMap::clearInstrument(int instrumentIndex)
{
    bool removed = false;

    for (auto it = noteToInstrument.begin(); it != noteToInstrument.end();)
    {
        if (it->second == instrumentIndex)
        {
            it = noteToInstrument.erase(it);
            removed = true;
        }
        else
        {
            ++it;
        }
    }

    if (const auto it = instrumentToNotes.find(instrumentIndex); it != instrumentToNotes.end())
    {
        it->second = {{-1, -1, -1}};
        removed = true;
    }

    if (removed && onChanged)
        onChanged();
}

void MidiMap::setBindings(std::vector<MidiBinding> newBindings)
{
    bindings_ = std::move(newBindings);
    if (onChanged)
        onChanged();
}

int MidiMap::instrumentForNote(int note) const noexcept
{
    const auto it = noteToInstrument.find(note);
    return it != noteToInstrument.end() ? it->second : -1;
}

int MidiMap::noteForInstrument(int instrumentIndex) const noexcept
{
    const auto it = instrumentToNotes.find(instrumentIndex);
    if (it == instrumentToNotes.end())
        return -1;

    for (const auto note : it->second)
        if (note >= 0)
            return note;

    return -1;
}

std::vector<int> MidiMap::notesForInstrument(int instrumentIndex) const
{
    std::vector<int> notes;
    const auto it = instrumentToNotes.find(instrumentIndex);
    if (it == instrumentToNotes.end())
        return notes;

    for (const auto note : it->second)
        if (note >= 0)
            notes.push_back(note);

    return notes;
}

void MidiMap::clearAssignments()
{
    noteToInstrument.clear();
    instrumentToNotes.clear();

    if (onChanged)
        onChanged();
}

void MidiMap::rebuildReverseIndex()
{
    instrumentToNotes.clear();

    for (const auto &[note, instrument] : noteToInstrument)
    {
        if (note < 0 || note > 127 || instrument < 0)
            continue;

        auto [entry, inserted] = instrumentToNotes.try_emplace(instrument, std::array<int, 3> {{-1, -1, -1}});
        (void) inserted;

        for (auto &slot : entry->second)
        {
            if (slot < 0)
            {
                slot = note;
                break;
            }
        }
    }
}

void MidiMap::clearBindings()
{
    bindings_.clear();
}

bool MidiMap::addBinding(const MidiBinding &binding)
{
    if (!binding.isValid())
        return false;

    removeBindingForControl(binding.controlId);
    removeBindingForMessage(binding.cc, binding.useNote, binding.channel);
    bindings_.push_back(binding);
    return true;
}

void MidiMap::removeBindingForControl(const std::string &controlId)
{
    bindings_.erase(std::remove_if(bindings_.begin(),
                                   bindings_.end(),
                                   [&](const MidiBinding &b) { return b.controlId == controlId; }),
                    bindings_.end());
}

void MidiMap::removeBindingForMessage(int cc, bool useNote, int channel)
{
    bindings_.erase(std::remove_if(bindings_.begin(),
                                   bindings_.end(),
                                   [&](const MidiBinding &b)
                                   {
                                       return b.cc == cc && b.useNote == useNote
                                              && (b.channel == 0 || channel == 0 || b.channel == channel);
                                   }),
                    bindings_.end());
}

juce::String MidiMap::controlForMessage(int cc, bool useNote, int channel) const
{
    for (const auto &b : bindings_)
    {
        if (b.cc == cc && b.useNote == useNote)
            if (b.channel == 0 || b.channel == channel)
                return juce::String(b.controlId);
    }

    return {};
}

//==============================================================================
void MidiMap::beginLearn(const std::string &controlId, bool useNote, int channel)
{
    cancelLearn();
    learningControlId_ = std::make_unique<std::string>(controlId);
    learningUsesNote = useNote;
    learningChannel = channel;
}

void MidiMap::cancelLearn()
{
    learningControlId_.reset();
    learningUsesNote = false;
    learningChannel = 0;
}

bool MidiMap::offerMessage(const juce::MidiMessage &message, juce::String *learnedControlId)
{
    if (learningControlId_ == nullptr)
        return false;

    if (message.isAllNotesOff() || message.isAllSoundOff() || message.isMetaEvent())
        return false;

    const bool isNote = message.isNoteOn() || message.isNoteOff();

    if (isNote != learningUsesNote)
        return false;

    // Ignore NoteOff / zero-velocity NoteOn during note learn so releasing a pad or
    // re-triggering doesn't lock onto a spurious event, and ignore CC/pedal traffic.
    if (learningUsesNote && !(message.isNoteOn() && message.getVelocity() > 0))
        return false;

    if (learningChannel > 1 && message.getChannel() + 1 != learningChannel)
        return false;

    MidiBinding binding;
    binding.controlId = *learningControlId_;
    binding.cc = isNote ? message.getNoteNumber() : message.getControllerNumber();
    binding.channel = message.getChannel() + 1;
    binding.useNote = isNote;

    const auto captured = binding.controlId;

    cancelLearn();

    // Note-to-instrument triggers ("note<idx>") live in `noteToInstrument`, not
    // in CC/control automation `bindings_`. Adding them to `bindings_` would make
    // MidiRouter intercept and swallow subsequent drum hits on that pad.
    if (captured.rfind("note", 0) != 0)
        addBinding(binding);

    if (learnedControlId != nullptr)
        *learnedControlId = juce::String(captured);

    if (onChanged)
        onChanged();

    return true;
}

//==============================================================================
juce::File MidiMap::bundledPresetDirectory()
{
#if defined(GLORP_PRESET_DIR)
    return juce::File(GLORP_PRESET_DIR);
#else
    return {};
#endif
}

juce::File MidiMap::userPresetDirectory()
{
    if (gUserPresetDirectoryOverride != juce::File())
        return gUserPresetDirectoryOverride;

    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile(config::CONFIG_DIR_NAME)
        .getChildFile("presets");
}

void MidiMap::setUserPresetDirectoryOverrideForTest(const juce::File &dir)
{
    gUserPresetDirectoryOverride = dir;
}

juce::File MidiMap::customPresetFile()
{
    return userPresetDirectory().getChildFile("Custom.xml");
}

juce::String MidiMap::sanitizePresetFileName(const juce::String &name)
{
    auto clean = name.trim();
    const juce::String invalid = "/\\:*?\"<>|";
    for (int i = 0; i < invalid.length(); ++i)
        clean = clean.replaceCharacter(invalid[i], '_');

    clean = clean.trimCharactersAtStart(".").trimCharactersAtEnd(".");
    return clean.isEmpty() ? juce::String("Custom") : clean;
}

std::vector<MapPreset> MidiMap::availablePresets()
{
    std::vector<MapPreset> presets;

    struct BuiltIn
    {
        const char *name;
        std::vector<std::pair<int, const char *>> notes;
    };

    static const BuiltIn builtIns[] = {
        {"General MIDI",
         {{35, "Kick"},
          {36, "Kick"},
          {37, "SideStick"},
          {38, "Snare"},
          {40, "Rimshot"},
          {42, "HihatClosed"},
          {44, "HihatPedal"},
          {46, "HihatOpen"},
          {48, "Tom1"},
          {45, "Tom2"},
          {43, "FloorTom"},
          {41, "FloorTom"},
          {49, "Crash"},
          {51, "Ride"},
          {53, "RideBell"}}},

        {"General MIDI (extended)",
         {{35, "Kick"},    {36, "Kick"},      {37, "SideStick"},   {38, "Snare"},       {39, "Clap"},
          {40, "Rimshot"}, {41, "FloorTom"},  {42, "HihatClosed"}, {43, "LowFloorTom"}, {44, "HihatPedal"},
          {45, "Tom2"},    {46, "HihatOpen"}, {47, "Tom3"},        {48, "Tom1"},        {49, "Crash"},
          {50, "HighTom"}, {51, "Ride"},      {53, "RideBell"},    {54, "Tambourine"},  {55, "Splash"},
          {56, "Cowbell"}, {57, "Crash2"},    {52, "China"}}},

        {"Toontrack / EZdrummer",
         {{36, "Kick"},          {35, "Kick"},          {38, "SnareHead"},     {40, "Rimshot"},    {37, "CrossStick"},
          {42, "HihatClosed"},   {22, "HihatClosed"},   {44, "HihatPedal"},    {46, "HihatOpen"},  {26, "HihatSemiOpen"},
          {48, "Tom1Head"},      {50, "Tom1Rim"},       {45, "Tom2Head"},      {47, "Tom2Rim"},    {43, "FTomHead"},
          {41, "FTomHead"},      {58, "FTomRim"},       {49, "Crash1Bow"},     {55, "Crash1Edge"}, {57, "Crash2Bow"},
          {52, "Crash2Edge"},    {51, "RideBow"},       {53, "RideBell"},      {59, "RideEdge"},   {54, "Tambourine"},
          {56, "Cowbell"}}},

        {"GetGood Drums",
         {{36, "Kick"},          {35, "Kick"},          {38, "Snare"},         {40, "Rimshot"},    {37, "CrossStick"},
          {42, "HihatClosed"},   {44, "HihatPedal"},    {46, "HihatOpen"},     {48, "Tom1"},       {50, "Tom1"},
          {45, "Tom2"},          {47, "Tom2"},          {43, "FloorTom"},      {41, "FloorTom"},   {49, "Crash"},
          {57, "Crash2"},        {51, "Ride"},          {53, "RideBell"},      {52, "China"},      {55, "Splash"}}},

        {"Urgitone",
         {{36, "Kick"},          {35, "Kick"},          {38, "Snare"},         {40, "Rimshot"},    {37, "CrossStick"},
          {42, "HihatClosed"},   {44, "HihatPedal"},    {46, "HihatOpen"},     {48, "Tom1"},       {50, "Tom1Rim"},
          {45, "Tom2"},          {47, "Tom2Rim"},       {43, "FloorTom"},      {41, "FloorTom"},   {49, "Crash"},
          {57, "Crash2"},        {51, "Ride"},          {53, "RideBell"},      {52, "China"},      {55, "Splash"}}},

        {"Roland V-Drums",
         {{36, "Kick"},          {35, "Kick"},          {38, "SnareHead"},     {40, "Rimshot"},   {37, "CrossStick"},
          {42, "HihatClosed"},   {22, "HihatClosed"},   {26, "HihatSemiOpen"}, {46, "HihatOpen"}, {44, "HihatPedal"},
          {80, "HihatSemiOpen"}, {23, "HihatSemiOpen"}, {48, "Tom1Head"},      {50, "Tom1Rim"},   {45, "Tom2Head"},
          {47, "Tom2Rim"},       {43, "FTomHead"},      {41, "FTomHead"},      {58, "FTomRim"},   {49, "Crash1Bow"},
          {55, "Crash1Edge"},    {57, "Crash2Bow"},     {52, "Crash2Edge"},    {51, "RideBow"},   {53, "RideBell"},
          {59, "RideEdge"},      {54, "Tambourine"},    {56, "Cowbell"}}},

        {"Yamaha DTX",
         {{36, "Kick"},        {35, "Kick"},          {38, "SnareHead"}, {40, "Rimshot"},    {37, "CrossStick"},
          {42, "HihatClosed"}, {26, "HihatSemiOpen"}, {46, "HihatOpen"}, {44, "HihatPedal"}, {48, "Tom1Head"},
          {50, "Tom1Rim"},     {45, "Tom2Head"},      {47, "Tom2Rim"},   {43, "FTomHead"},   {41, "FTomRim"},
          {49, "Crash1Bow"},   {55, "Crash1Edge"},    {57, "Crash2Bow"}, {52, "Crash2Edge"}, {51, "RideBow"},
          {53, "RideBell"},    {59, "RideEdge"}}},

        {"Alesis (Strike / Nitro / Surge)",
         {{36, "Kick"},          {38, "SnareHead"}, {40, "Rimshot"},    {37, "CrossStick"}, {42, "HihatClosed"},
          {23, "HihatSemiOpen"}, {46, "HihatOpen"}, {44, "HihatPedal"}, {48, "Tom1Head"},   {60, "Tom1Rim"},
          {45, "Tom2Head"},      {62, "Tom2Rim"},   {43, "FTomHead"},   {64, "FTomRim"},    {49, "Crash1Bow"},
          {55, "Crash1Edge"},    {57, "Crash2Bow"}, {52, "Crash2Edge"}, {51, "RideBow"},    {53, "RideBell"},
          {59, "RideEdge"}}},

        {"Millenium / 2box",
         {{36, "Kick"},
          {38, "SnareHead"},
          {40, "Rimshot"},
          {37, "CrossStick"},
          {42, "HihatClosed"},
          {46, "HihatOpen"},
          {44, "HihatPedal"},
          {48, "Tom1Head"},
          {50, "Tom1Rim"},
          {45, "Tom2Head"},
          {47, "Tom2Rim"},
          {43, "FTomHead"},
          {41, "FTomRim"},
          {49, "Crash1Bow"},
          {55, "Crash1Edge"},
          {57, "Crash2Bow"},
          {51, "RideBow"},
          {53, "RideBell"}}},

        {"EFNote (3 / 5 / 7)",
         {{36, "Kick"},
          {38, "SnareHead"},
          {40, "Rimshot"},
          {37, "CrossStick"},
          {42, "HihatClosed"},
          {46, "HihatOpen"},
          {44, "HihatPedal"},
          {48, "Tom1Head"},
          {45, "Tom2Head"},
          {43, "FTomHead"},
          {49, "Crash1Bow"},
          {55, "Crash1Edge"},
          {57, "Crash2Bow"},
          {52, "Crash2Edge"},
          {51, "RideBow"},
          {53, "RideBell"},
          {59, "RideEdge"}}},

        {"Ableton Push",
         {{36, "Kick"},
          {37, "Clap"},
          {38, "Snare"},
          {42, "HihatClosed"},
          {46, "HihatOpen"},
          {45, "Tom1"},
          {47, "Tom2"},
          {48, "Tom3"},
          {43, "FloorTom"},
          {49, "Crash"},
          {51, "Ride"}}},

        {"ReaperDrummer", {{36, "Kick"},           {38, "SnareHead"},      {40, "Rimshot"},
                           {37, "RimClick"},       {39, "SnareEdge"},      {33, "SnareMuted"},
                           {29, "SnareFat"},       {42, "HihatClosed"},    {22, "HihatClosedEdge"},
                           {23, "HihatOpen1Bow"},  {24, "HihatOpen1Edge"}, {31, "HihatOpen2Bow"},
                           {32, "HihatOpen2Edge"}, {34, "HihatOpen3Bow"},  {27, "HihatOpen3Edge"},
                           {30, "HihatOpen4Bow"},  {25, "HihatOpen4Edge"}, {46, "HihatOpen"},
                           {26, "HihatOpenEdge"},  {44, "HihatPedal"},     {61, "HihatSlammed"},
                           {63, "HihatSplash"},    {48, "Tom1Head"},       {50, "Tom1Rim"},
                           {47, "Tom2Head"},       {60, "Tom2Rim"},        {45, "Tom3Head"},
                           {62, "Tom3Rim"},        {43, "FTomHead"},       {58, "FTomRim"},
                           {41, "FTom2Head"},      {65, "FTom2Rim"},       {49, "Crash1Bow"},
                           {55, "Crash1Edge"},     {57, "Crash2Bow"},      {52, "Crash2Edge"},
                           {56, "Crash3Bow"},      {54, "Crash3Edge"},     {51, "RideBow"},
                           {53, "RideBell"},       {59, "RideEdge"},       {28, "Ride2Bow"},
                           {29, "Ride2Bell"},      {60, "Ride2Edge"},      {31, "Cowbell"},
                           {32, "CowbellMuff"},    {33, "CowbellHi"}}},
    };

    for (const auto &b : builtIns)
    {
        MapPreset preset;
        preset.name = b.name;
        preset.isBuiltIn = true;

        for (const auto &[note, instrument] : b.notes)
        {
            if (note <= 0 || instrument == nullptr)
                continue;

            preset.noteToInstrumentName[note] = instrument;
        }

        presets.push_back(std::move(preset));
    }

    // The user's own presets, which are just XML in the same directory.
    auto scanDirectory = [&](const juce::File &dir)
    {
        if (!dir.isDirectory())
            return;

        for (const auto &entry : juce::RangedDirectoryIterator(dir, false, "*.xml"))
        {
            MapPreset preset;
            preset.fileName = entry.getFile().getFullPathName();
            preset.name = entry.getFile().getFileNameWithoutExtension();
            preset.isBuiltIn = false;

            if (auto doc = juce::parseXML(entry.getFile()))
                preset.name = doc->getStringAttribute("name", preset.name);

            presets.push_back(std::move(preset));
        }
    };

    const size_t builtInCount = presets.size();
    scanDirectory(userPresetDirectory());
    scanDirectory(bundledPresetDirectory());

    // Keep built-ins at the top, put "Custom" first among user presets, then sort alphabetically.
    if (presets.size() > builtInCount + 1)
    {
        std::sort(presets.begin() + static_cast<std::ptrdiff_t>(builtInCount),
                  presets.end(),
                  [](const MapPreset &a, const MapPreset &b)
                  {
                      const bool aCustom = a.name.equalsIgnoreCase("Custom");
                      const bool bCustom = b.name.equalsIgnoreCase("Custom");
                      if (aCustom != bCustom)
                          return aCustom;
                      return a.name.compareIgnoreCase(b.name) < 0;
                  });
    }

    return presets;
}

bool MidiMap::loadPresetFile(const juce::File &file, MapPreset &out, juce::String &error)
{
    auto root = juce::parseXML(file);

    if (root == nullptr || root->getTagName() != "glorpMapPreset")
    {
        error = file.getFileName() + " is not a Suite Drums Glorp MIDI map preset";
        return false;
    }

    out = MapPreset {};
    out.name = root->getStringAttribute("name", file.getFileNameWithoutExtension());
    out.fileName = file.getFullPathName();
    out.isBuiltIn = false;

    if (auto *notes = root->getChildByName("notes"))
    {
        for (auto *m : notes->getChildIterator())
        {
            const auto note = m->getIntAttribute("note", -1);

            if (note < 0 || note > 127)
                continue;

            if (auto name = m->getStringAttribute("name"); name.isNotEmpty())
                out.noteToInstrumentName[note] = name;

            const auto instrument = m->getIntAttribute("instrument", -1);

            if (instrument >= 0)
                out.noteToInstrument[note] = instrument;
        }
    }

    if (auto *learn = root->getChildByName("learn"))
    {
        for (auto *m : learn->getChildIterator())
        {
            MidiBinding binding;
            binding.controlId = m->getStringAttribute("control").toStdString();
            binding.cc = m->getIntAttribute("cc", -1);
            binding.channel = m->getIntAttribute("channel", 0);
            binding.useNote = m->getBoolAttribute("useNote", false);

            if (binding.isValid() && binding.controlId.rfind("note", 0) != 0)
                out.bindings.push_back(std::move(binding));
        }
    }

    return true;
}

bool MidiMap::savePresetFile(const juce::File &file,
                             const juce::String &name,
                             const std::vector<juce::String> &instrumentNames) const
{
    juce::XmlElement root("glorpMapPreset");
    root.setAttribute("name", name);
    root.setAttribute("version", 1);

    auto *notes = root.createNewChildElement("notes");

    for (const auto &[note, instrumentIndex] : noteToInstrument)
    {
        auto *m = notes->createNewChildElement("map");
        m->setAttribute("note", note);
        m->setAttribute("instrument", instrumentIndex);

        // Name as well as index, so the preset survives being applied to a
        // different kit with a different instrument order.
        if (instrumentIndex >= 0 && instrumentIndex < static_cast<int>(instrumentNames.size()))
            m->setAttribute("name", instrumentNames[static_cast<size_t>(instrumentIndex)]);
    }

    auto *learn = root.createNewChildElement("learn");

    for (const auto &b : bindings_)
    {
        if (b.controlId.rfind("note", 0) == 0)
            continue;

        auto *m = learn->createNewChildElement("bind");
        m->setAttribute("control", b.controlId);
        m->setAttribute("cc", b.cc);
        m->setAttribute("channel", b.channel);
        m->setAttribute("useNote", b.useNote);
    }

    file.getParentDirectory().createDirectory();
    return root.writeTo(file);
}

bool MidiMap::saveUserPreset(const juce::String &name,
                             const std::vector<juce::String> &instrumentNames,
                             juce::File *outFile) const
{
    auto trimmed = name.trim();
    if (trimmed.isEmpty())
        trimmed = "Custom";

    const bool isCustom = trimmed.equalsIgnoreCase("Custom");
    const juce::String canonicalName = isCustom ? juce::String("Custom") : trimmed;
    const juce::File targetFile =
        isCustom ? customPresetFile()
                 : userPresetDirectory().getChildFile(sanitizePresetFileName(canonicalName) + ".xml");

    const bool wrote = savePresetFile(targetFile, canonicalName, instrumentNames);

    if (outFile != nullptr)
        *outFile = targetFile;

    return wrote && targetFile.existsAsFile();
}

void MidiMap::applyPreset(const MapPreset &preset, const std::vector<juce::String> &kitInstrumentNames)
{
    clearAssignments();
    clearBindings();
    bindings_ = preset.bindings;

    // Smart instrument name matching so presets resolve correctly against any kit
    const auto stripName = [](const juce::String &s) { return s.toLowerCase().removeCharacters(" .-_/\\"); };
    std::vector<juce::String> strippedNames;
    strippedNames.reserve(kitInstrumentNames.size());
    for (const auto &name : kitInstrumentNames)
        strippedNames.push_back(stripName(name));

    auto isHiHatName = [](const juce::String &ns) { return ns.contains("hat") || ns.contains("hh"); };

    auto matchInstrument = [&](const juce::String &target) -> int
    {
        const auto lower = target.toLowerCase();
        const auto targetStripped = stripName(target);

        // 1. Exact match (case-insensitive or normalized-stripped)
        for (size_t i = 0; i < kitInstrumentNames.size(); ++i)
            if (kitInstrumentNames[i].equalsIgnoreCase(target) || strippedNames[i] == targetStripped)
                return static_cast<int>(i);

        // 2. Multi-zone snare pieces
        if (targetStripped == "snarehead" || targetStripped == "snare")
        {
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if (ns == "snarehead" || ns == "snare"
                    || (ns.contains("snare") && !ns.contains("rim") && !ns.contains("click") && !ns.contains("cross")
                        && !ns.contains("stick") && !ns.contains("rest")))
                    return static_cast<int>(i);
            }
        }
        else if (targetStripped == "rimshot" || targetStripped == "snarerimshot")
        {
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if (ns.contains("rimshot")
                    || (ns.contains("snare") && ns.contains("rim") && !ns.contains("click") && !ns.contains("cross")
                        && !ns.contains("stick")))
                    return static_cast<int>(i);
            }
        }
        else if (targetStripped == "crossstick" || targetStripped == "rimclick" || targetStripped == "sidestick")
        {
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if (ns.contains("click") || ns.contains("crossstick") || ns.contains("sidestick")
                    || ns.contains("cross"))
                    return static_cast<int>(i);
            }
        }

        // 3. Kick
        else if (targetStripped == "kick" || targetStripped == "kick2")
        {
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if (ns.contains("kick") || ns.contains("kdrum") || ns.contains("bdrum") || ns == "bd")
                    return static_cast<int>(i);
            }
        }

        // 4. Hi-Hat
        else if (targetStripped.contains("closed") || targetStripped.contains("close"))
        {
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if (isHiHatName(ns) && (ns.contains("closed") || ns.contains("close")))
                    return static_cast<int>(i);
            }
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if (isHiHatName(ns) && !ns.contains("open") && !ns.contains("pedal") && !ns.contains("foot"))
                    return static_cast<int>(i);
            }
        }
        else if (targetStripped.contains("semi"))
        {
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if (isHiHatName(ns) && ns.contains("semi"))
                    return static_cast<int>(i);
            }
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if (isHiHatName(ns) && ns.contains("open"))
                    return static_cast<int>(i);
            }
        }
        else if (targetStripped.contains("open"))
        {
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if (isHiHatName(ns) && ns.contains("open") && !ns.contains("semi"))
                    return static_cast<int>(i);
            }
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if (isHiHatName(ns) && !ns.contains("closed") && !ns.contains("close") && !ns.contains("pedal")
                    && !ns.contains("foot"))
                    return static_cast<int>(i);
            }
        }
        else if (targetStripped.contains("pedal") || targetStripped.contains("foot"))
        {
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if (isHiHatName(ns) && (ns.contains("pedal") || ns.contains("foot") || ns.contains("chick")))
                    return static_cast<int>(i);
            }
        }

        // 5. Toms
        else if (targetStripped == "tom1head" || targetStripped == "tom1" || targetStripped == "hightom")
        {
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if ((ns.contains("tom1") || ns.contains("rack1") || ns.contains("racktom1") || ns.contains("hightom"))
                    && !ns.contains("rim"))
                    return static_cast<int>(i);
            }
        }
        else if (targetStripped == "tom1rim")
        {
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if ((ns.contains("tom1") || ns.contains("rack1") || ns.contains("racktom1")) && ns.contains("rim"))
                    return static_cast<int>(i);
            }
        }
        else if (targetStripped == "tom2head" || targetStripped == "tom2" || targetStripped == "midtom")
        {
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if ((ns.contains("tom2") || ns.contains("rack2") || ns.contains("racktom2") || ns.contains("midtom"))
                    && !ns.contains("rim"))
                    return static_cast<int>(i);
            }
        }
        else if (targetStripped == "tom2rim")
        {
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if ((ns.contains("tom2") || ns.contains("rack2") || ns.contains("racktom2")) && ns.contains("rim"))
                    return static_cast<int>(i);
            }
        }
        else if (targetStripped.contains("ftomhead") || targetStripped.contains("floortom")
                 || targetStripped == "lowfloortom" || targetStripped == "ftom" || targetStripped == "tom3")
        {
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if ((ns.contains("floor") || ns.contains("ftom") || ns.contains("lowtom") || ns.contains("tom3")
                     || ns.contains("tom4"))
                    && !ns.contains("rim"))
                    return static_cast<int>(i);
            }
        }
        else if (targetStripped.contains("ftomrim") || targetStripped.contains("floortomrim"))
        {
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if ((ns.contains("floor") || ns.contains("ftom") || ns.contains("tom3")) && ns.contains("rim"))
                    return static_cast<int>(i);
            }
        }

        // 6. Cymbals
        else if (targetStripped == "ridebell")
        {
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if (ns.contains("ride") && ns.contains("bell"))
                    return static_cast<int>(i);
            }
        }
        else if (targetStripped == "rideedge")
        {
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if (ns.contains("ride") && ns.contains("edge"))
                    return static_cast<int>(i);
            }
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if (ns.contains("ride") && !ns.contains("bell") && !ns.contains("choke"))
                    return static_cast<int>(i);
            }
        }
        else if (targetStripped == "ridebow" || targetStripped == "ride")
        {
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if (ns.contains("ride") && !ns.contains("bell") && !ns.contains("edge") && !ns.contains("choke"))
                    return static_cast<int>(i);
            }
            for (size_t i = 0; i < strippedNames.size(); ++i)
                if (strippedNames[i].contains("ride"))
                    return static_cast<int>(i);
        }
        else if (targetStripped.startsWith("crash1") || targetStripped == "crash")
        {
            const bool wantsEdge = targetStripped.contains("edge");
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if (ns.contains("crash1") || ns.contains("crashl") || ns.contains("leftcrash")
                    || (ns.contains("crash") && !ns.contains("2") && !ns.contains("crashr") && !ns.contains("right")))
                {
                    if (wantsEdge && ns.contains("edge"))
                        return static_cast<int>(i);
                    if (!wantsEdge && (ns.contains("bow") || (!ns.contains("edge") && !ns.contains("choke"))))
                        return static_cast<int>(i);
                }
            }
            for (size_t i = 0; i < strippedNames.size(); ++i)
                if (strippedNames[i].contains("crash") && !strippedNames[i].contains("2"))
                    return static_cast<int>(i);
        }
        else if (targetStripped.startsWith("crash2"))
        {
            const bool wantsEdge = targetStripped.contains("edge");
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                const auto &ns = strippedNames[i];
                if (ns.contains("crash2") || ns.contains("crashr") || ns.contains("rightcrash") || ns.contains("china")
                    || ns.contains("splash"))
                {
                    if (wantsEdge && ns.contains("edge"))
                        return static_cast<int>(i);
                    if (!wantsEdge && (ns.contains("bow") || (!ns.contains("edge") && !ns.contains("choke"))))
                        return static_cast<int>(i);
                }
            }
            // Fallback to the second crash-like instrument if multiple unlabeled crashes exist
            int crashSeen = 0;
            for (size_t i = 0; i < strippedNames.size(); ++i)
            {
                if (strippedNames[i].contains("crash"))
                {
                    ++crashSeen;
                    if (crashSeen >= 2)
                        return static_cast<int>(i);
                }
            }
        }

        // Substring fallback (normalized then raw)
        for (size_t i = 0; i < strippedNames.size(); ++i)
            if (strippedNames[i].contains(targetStripped))
                return static_cast<int>(i);

        for (size_t i = 0; i < kitInstrumentNames.size(); ++i)
            if (kitInstrumentNames[i].toLowerCase().contains(lower))
                return static_cast<int>(i);

        return -1;
    };

    for (const auto &[note, instrumentName] : preset.noteToInstrumentName)
    {
        const int instIdx = matchInstrument(instrumentName);
        if (instIdx >= 0)
            noteToInstrument[note] = instIdx;
    }

    for (const auto &[note, instrumentIndex] : preset.noteToInstrument)
    {
        if (noteToInstrument.count(note) > 0)
            continue;

        if (instrumentIndex >= 0 && instrumentIndex < static_cast<int>(kitInstrumentNames.size()))
            noteToInstrument[note] = instrumentIndex;
    }

    // Keep the reverse index consistent with whatever survived.
    rebuildReverseIndex();

    if (onChanged)
        onChanged();
}

juce::XmlElement *MidiMap::exportXml() const
{
    auto *root = new juce::XmlElement("MidiMap");

    auto *notes = root->createNewChildElement("notes");

    for (const auto &[note, instrument] : noteToInstrument)
    {
        auto *m = notes->createNewChildElement("map");
        m->setAttribute("note", note);
        m->setAttribute("instrument", instrument);
    }

    auto *learn = root->createNewChildElement("learn");

    for (const auto &b : bindings_)
    {
        auto *m = learn->createNewChildElement("bind");
        m->setAttribute("control", b.controlId);
        m->setAttribute("cc", b.cc);
        m->setAttribute("channel", b.channel);
        m->setAttribute("useNote", b.useNote);
    }

    return root;
}

void MidiMap::importXml(const juce::XmlElement &xml)
{
    clearAssignments();
    clearBindings();

    if (auto *notes = xml.getChildByName("notes"))
    {
        for (auto *m : notes->getChildIterator())
        {
            const auto note = m->getIntAttribute("note", -1);
            const auto instrument = m->getIntAttribute("instrument", -1);

            if (note >= 0 && note < 128 && instrument >= 0)
                noteToInstrument[note] = instrument;
        }
    }

    if (auto *learn = xml.getChildByName("learn"))
    {
        for (auto *m : learn->getChildIterator())
        {
            MidiBinding binding;
            binding.controlId = m->getStringAttribute("control").toStdString();
            binding.cc = m->getIntAttribute("cc", -1);
            binding.channel = m->getIntAttribute("channel", 0);
            binding.useNote = m->getBoolAttribute("useNote", false);

            if (binding.isValid() && binding.controlId.rfind("note", 0) != 0)
                bindings_.push_back(std::move(binding));
        }
    }

    rebuildReverseIndex();

    if (onChanged)
        onChanged();
}

} // namespace glorp
