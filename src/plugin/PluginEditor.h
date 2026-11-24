/*
    PluginEditor.h - the thin AudioProcessorEditor wrapper.

    Keeps JUCE's plumbing out of the Suite UI so the Editor stays testable
    without dragging a host wrapper into it.
*/

#pragma once

#include "Editor.h"

namespace glorp
{

class PluginEditor : public juce::AudioProcessorEditor
{
  public:
    explicit PluginEditor(PluginProcessor &p) : juce::AudioProcessorEditor(&p), proc(p), editor(p)
    {
        addAndMakeVisible(editor);

#if defined(__linux__)
        // Let the X11/Wayland window manager handle resize handles cleanly without an extra JUCE corner resizer.
        setResizable(true, false);
#else
        setResizable(true, true);
#endif
        setResizeLimits(900, 620, 2400, 1800);

        auto &settings = p.getSettings();
        setSize(juce::jlimit(900, 2400, settings.editorWidth), juce::jlimit(620, 1800, settings.editorHeight));
    }

    void paint(juce::Graphics &g) override { g.fillAll(Theme::suite().background); }

    void resized() override
    {
        auto bounds = getLocalBounds();
        auto &settings = proc.getSettings();

        if (settings.uiScale > 1.001f)
        {
            // Scale the whole logical 900x620 editor into the window. This keeps
            // the existing layout intact while making everything larger.
            const float scale = juce::jlimit(
                1.0f, 2.5f, std::min(bounds.getWidth() / 900.0f, bounds.getHeight() / 620.0f));
            const int scaledW = juce::roundToInt(900.0f * scale);
            const int scaledH = juce::roundToInt(620.0f * scale);
            editor.setBounds((bounds.getWidth() - scaledW) / 2,
                             (bounds.getHeight() - scaledH) / 2,
                             900,
                             620);
            editor.setTransform(juce::AffineTransform::scale(scale));
        }
        else
        {
            editor.setTransform({});
            editor.setBounds(bounds);
        }

        if (bounds.getWidth() >= 900 && bounds.getHeight() >= 620)
        {
            if (settings.editorWidth != bounds.getWidth() || settings.editorHeight != bounds.getHeight())
            {
                settings.editorWidth = bounds.getWidth();
                settings.editorHeight = bounds.getHeight();
                settings.markDirty();
            }
        }
    }

  private:
    PluginProcessor &proc;
    Editor editor;
};

} // namespace glorp
