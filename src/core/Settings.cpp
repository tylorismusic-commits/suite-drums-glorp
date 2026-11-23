#include "Settings.h"
#include "Config.h"
#include "KitScanner.h"

namespace glorp
{

namespace
{
constexpr int kVersionTag = 1;
}

namespace
{
juce::File applicationDataDirectory()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
}
} // namespace

Settings::Settings()
{
    migrateLegacyConfig();
    load();
}

void Settings::migrateLegacyConfig()
{
    const auto current = getConfigDirectory();
    const auto legacy = applicationDataDirectory().getChildFile(config::LEGACY_CONFIG_DIR_NAME);

    if (current.exists() || !legacy.isDirectory())
        return;

    // First run after the rename: carry the old settings, presets and layout
    // across so existing users keep their kit list and MIDI map edits.
    if (!legacy.copyDirectoryTo(current))
        current.createDirectory();
}

juce::File Settings::getConfigDirectory() const
{
    return applicationDataDirectory().getChildFile(config::CONFIG_DIR_NAME);
}

juce::File Settings::getConfigFile() const
{
    return getConfigDirectory().getChildFile("settings.txt");
}

juce::File Settings::getPresetDirectory() const
{
    return getConfigDirectory().getChildFile("presets");
}

void Settings::load()
{
    const auto file = getConfigFile();

    if (!file.existsAsFile())
        return;

    juce::MemoryBlock block;

    if (!file.loadFileAsData(block))
        return;

    readState(block);
}

void Settings::saveIfChanged()
{
    if (!dirty)
        return;

    const auto file = getConfigFile();
    file.getParentDirectory().createDirectory();

    juce::MemoryBlock block;
    writeState(block);

    if (file.replaceWithData(block.getData(), block.getSize()))
        dirty = false;
}

void Settings::writeState(juce::MemoryBlock &destination) const
{
    juce::XmlElement root("SuiteDrumsGlorpSettings");
    root.setAttribute("version", kVersionTag);

    auto *kits = root.createNewChildElement("KitPaths");

    for (const auto &p : extraKitPaths)
        kits->createNewChildElement("path")->setAttribute("value", p);

    auto *recent = root.createNewChildElement("RecentKits");

    for (const auto &p : recentKits)
        recent->createNewChildElement("kit")->setAttribute("value", p);

    root.setAttribute("lastKit", lastKitPath);
    root.setAttribute("lastMapPreset", lastMapPreset);
    root.setAttribute("midiLibraryPath", midiLibraryPath);
    root.setAttribute("editorWidth", editorWidth);
    root.setAttribute("editorHeight", editorHeight);
    root.setAttribute("editorMaximised", editorMaximised);
    root.setAttribute("uiTextScale", static_cast<double>(uiTextScale));
    root.setAttribute("openGlEnabled", openGlEnabled);

    juce::MemoryOutputStream stream(destination, false);
    root.writeToStream(stream, {});
}

void Settings::readState(const juce::MemoryBlock &source)
{
    auto xmlString =
        juce::String::fromUTF8(static_cast<const char *>(source.getData()), static_cast<int>(source.getSize()));
    auto root = juce::parseXML(xmlString);

    if (root == nullptr
        || (root->getTagName() != "SuiteDrumsGlorpSettings" && root->getTagName() != "GlorpDrumsSettings"))
        return;

    if (auto *kits = root->getChildByName("KitPaths"))
        for (auto *p : kits->getChildIterator())
            if (auto v = p->getStringAttribute("value"); v.isNotEmpty())
                extraKitPaths.add(v);

    if (auto *recent = root->getChildByName("RecentKits"))
        for (auto *k : recent->getChildIterator())
            if (auto v = k->getStringAttribute("value"); v.isNotEmpty())
                recentKits.add(v);

    lastKitPath = root->getStringAttribute("lastKit", lastKitPath);
    lastMapPreset = root->getStringAttribute("lastMapPreset", lastMapPreset);
    midiLibraryPath = root->getStringAttribute("midiLibraryPath", midiLibraryPath);
    editorWidth = root->getIntAttribute("editorWidth", editorWidth);
    editorHeight = root->getIntAttribute("editorHeight", editorHeight);
    editorMaximised = root->getBoolAttribute("editorMaximised", editorMaximised);
    uiTextScale = juce::jlimit(0.85f, 1.35f, static_cast<float>(root->getDoubleAttribute("uiTextScale", uiTextScale)));
    openGlEnabled = root->getBoolAttribute("openGlEnabled", openGlEnabled);
}

void Settings::addKitPath(const juce::String &path)
{
    if (path.isEmpty())
        return;

    const auto resolved = KitScanner::resolvePath(path.toStdString());

    if (resolved.empty())
        return;

    const auto s = juce::String(resolved);

    if (!extraKitPaths.contains(s))
    {
        extraKitPaths.add(s);
        dirty = true;
    }
}

void Settings::removeKitPath(const juce::String &path)
{
    extraKitPaths.removeString(path);
    dirty = true;
}

std::vector<std::string> Settings::allSearchPaths() const
{
    auto paths = KitScanner::defaultSearchPaths();

    for (const auto &p : extraKitPaths)
    {
        const auto resolved = KitScanner::resolvePath(p.toStdString());

        if (!resolved.empty())
            paths.push_back(resolved);
    }

    return paths;
}

void Settings::pushRecentKit(const juce::String &path)
{
    if (path.isEmpty())
        return;

    recentKits.removeString(path);
    recentKits.insert(0, path);

    while (recentKits.size() > maxRecentKits())
        recentKits.remove(recentKits.size() - 1);

    lastKitPath = path;
    dirty = true;
}

} // namespace glorp
