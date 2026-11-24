/*
    convert_kit.cpp - headless DrumGizmo XML / SFZ -> .glorpkit converter.

    Usage:
        SuiteKitConverter input.xml|input.sfz output.glorpkit
*/

#include "../src/editor_app/GlorpKitProject.h"

#include <cctype>
#include <iostream>
#include <string>

static bool hasExtensionCI (const std::string& path, const std::string& ext)
{
    std::string lower = path;
    for (auto& c : lower)
        c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
    return lower.size() >= ext.size() && lower.compare (lower.size() - ext.size(), ext.size(), ext) == 0;
}

int main (int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "Usage: " << argv[0] << " <input.xml|input.sfz> <output.glorpkit>" << std::endl;
        return 2;
    }

    const std::string inputPath = argv[1];
    const std::string outputPath = argv[2];

    glorp::GlorpKitProject project;
    std::string error;

    bool ok = false;
    if (hasExtensionCI (inputPath, ".sfz"))
        ok = project.importSfzKit (inputPath, error);
    else
        ok = project.importDrumGizmoXml (inputPath, error);

    if (! ok)
    {
        std::cerr << "Import failed: " << error << std::endl;
        return 1;
    }

    if (! project.exportToGlorpKit (outputPath, error))
    {
        std::cerr << "Export failed: " << error << std::endl;
        return 1;
    }

    std::size_t sampleCount = 0;
    for (const auto& piece : project.pieces)
        for (const auto& zone : piece.zones)
            for (const auto& hit : zone.hits)
                sampleCount += hit.micSamples.size();

    std::cout << "Converted " << inputPath << " -> " << outputPath << std::endl;
    std::cout << "  Title: " << project.title << std::endl;
    std::cout << "  Pieces: " << project.pieces.size() << ", mics: " << project.mics.size()
              << ", samples: " << sampleCount << ", rate: " << project.sampleRate << " Hz" << std::endl;
    return 0;
}
