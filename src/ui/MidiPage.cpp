#include "MidiPage.h"

#include <algorithm>
#include <cmath>

namespace glorp
{

namespace
{
std::array<float, 8> presetCurve(VelocityCurve curve)
{
    std::array<float, 8> points {};

    for (size_t i = 0; i < points.size(); ++i)
    {
        const float x = static_cast<float>(i) / static_cast<float>(points.size() - 1);

        switch (curve)
        {
            case VelocityCurve::logarithmic:
                points[i] = std::sqrt(x);
                break;
            case VelocityCurve::exponential:
                points[i] = x * x;
                break;
            case VelocityCurve::sCurve:
                points[i] = x * x * (3.0f - 2.0f * x);
                break;
            case VelocityCurve::linear:
            default:
                points[i] = x;
                break;
        }
    }

    return points;
}

std::array<float, 8> curveForParams(const InstrumentParams &params)
{
    return params.useCustomVelocityCurve ? params.customVelocityCurve : presetCurve(params.velocityCurve);
}

juce::String noteName(int note)
{
    if (note < 0 || note > 127)
        return "--";

    return juce::MidiMessage::getMidiNoteName(note, true, true, 3);
}

float evaluateCurve(const std::array<float, 8> &points, float x)
{
    x = juce::jlimit(0.0f, 1.0f, x);
    const auto scaled = x * static_cast<float>(points.size() - 1);
    const auto index = static_cast<int>(scaled);
    const auto frac = scaled - static_cast<float>(index);
    const auto a = points[static_cast<size_t>(juce::jlimit(0, 7, index))];
    const auto b = points[static_cast<size_t>(juce::jlimit(0, 7, index + 1))];
    return juce::jlimit(0.0f, 1.0f, a + (b - a) * frac);
}

class NoteSlot : public juce::Component
{
  public:
    NoteSlot(MidiPage &ownerIn, int instrumentIn, int slotIn) : owner(ownerIn), instrument(instrumentIn), slot(slotIn)
    {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    void setTheme(const Theme &t)
    {
        theme = t;
        repaint();
    }

    void setNote(int newNote)
    {
        note = newNote;
        repaint();
    }

    void setLearning(bool value)
    {
        learning = value;
        repaint();
    }

    void paint(juce::Graphics &g) override
    {
        auto b = getLocalBounds().toFloat().reduced(1.0f);
        const auto learnWidth = juce::jmin(30.0f, b.getWidth() * 0.30f);
        auto learnArea = b.removeFromRight(learnWidth);

        g.setColour(learning ? theme.accent.withAlpha(0.20f) : theme.trough);
        g.fillRoundedRectangle(b, 5.0f);
        g.setColour(learning ? theme.accent : theme.panelEdge);
        g.drawRoundedRectangle(b.reduced(0.5f), 5.0f, learning ? 1.8f : 1.0f);

        g.setColour(note >= 0 ? theme.ink : theme.inkSoft.withAlpha(0.65f));
        g.setFont(displayFont(15.0f));
        g.drawFittedText(note >= 0 ? noteName(note) : "--", b.reduced(7.0f, 1.0f).toNearestInt(),
                         juce::Justification::centredLeft, 1, 0.8f);

        g.setColour(learning ? theme.accent : theme.panel);
        g.fillRoundedRectangle(learnArea.reduced(1.0f), 4.0f);
        g.setColour(learning ? theme.background : theme.inkSoft);
        g.setFont(uiFont(8.5f).boldened());
        g.drawFittedText(learning ? "..." : "LRN", learnArea.toNearestInt(), juce::Justification::centred, 1, 0.7f);
    }

    void mouseDown(const juce::MouseEvent &e) override
    {
        if (e.mods.isPopupMenu())
        {
            owner.noteSlotChanged(instrument, slot, -1);
            return;
        }

        const auto learnWidth = juce::jmin(30.0f, static_cast<float>(getWidth()) * 0.30f);
        if (static_cast<float>(e.position.x) >= static_cast<float>(getWidth()) - learnWidth)
        {
            owner.armNoteLearn(instrument, slot);
            return;
        }

        juce::PopupMenu menu;
        menu.addItem(1, "Clear slot", note >= 0);

        for (int octave = 0; octave < 11; ++octave)
        {
            juce::PopupMenu octaveMenu;

            for (int n = octave * 12; n <= juce::jmin(127, octave * 12 + 11); ++n)
                octaveMenu.addItem(n + 1000, noteName(n) + "  (" + juce::String(n) + ")", true, note == n);

            menu.addSubMenu("Octave " + juce::String(octave - 1), octaveMenu);
        }

        auto safeThis = juce::Component::SafePointer<NoteSlot>(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                           [safeThis](int result)
                           {
                               if (safeThis == nullptr || result == 0)
                                   return;

                               safeThis->owner.noteSlotChanged(safeThis->instrument,
                                                               safeThis->slot,
                                                               result == 1 ? -1 : result - 1000);
                           });
    }

  private:
    MidiPage &owner;
    int instrument = 0;
    int slot = 0;
    int note = -1;
    bool learning = false;
    Theme theme = Theme::suite();
};

class VelocityCurveEditor : public juce::Component
{
  public:
    explicit VelocityCurveEditor(MidiPage &ownerIn) : owner(ownerIn)
    {
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
    }

    void setTheme(const Theme &t)
    {
        theme = t;
        repaint();
    }

    void setPoints(const std::array<float, 8> &newPoints, int instrumentIn)
    {
        points = newPoints;
        instrument = instrumentIn;
        repaint();
    }

    void paint(juce::Graphics &g) override
    {
        auto b = getLocalBounds().toFloat().reduced(1.0f);
        g.setColour(theme.trough);
        g.fillRoundedRectangle(b, 5.0f);
        g.setColour(theme.panelEdge);
        g.drawRoundedRectangle(b.reduced(0.5f), 5.0f, 1.0f);

        auto plot = b.reduced(7.0f, 5.0f);

        g.setColour(theme.panelEdge.withAlpha(0.35f));
        for (int i = 1; i < 4; ++i)
        {
            const float x = plot.getX() + plot.getWidth() * static_cast<float>(i) / 4.0f;
            const float y = plot.getY() + plot.getHeight() * static_cast<float>(i) / 4.0f;
            g.drawVerticalLine(juce::roundToInt(x), plot.getY(), plot.getBottom());
            g.drawHorizontalLine(juce::roundToInt(y), plot.getX(), plot.getRight());
        }

        juce::Path curve;
        constexpr int segments = 64;
        for (int i = 0; i <= segments; ++i)
        {
            const float x = static_cast<float>(i) / static_cast<float>(segments);
            const float y = evaluateCurve(points, x);
            const auto px = plot.getX() + x * plot.getWidth();
            const auto py = plot.getBottom() - y * plot.getHeight();
            if (i == 0)
                curve.startNewSubPath(px, py);
            else
                curve.lineTo(px, py);
        }

        g.setColour(theme.accent);
        g.strokePath(curve, juce::PathStrokeType(2.0f));

        for (size_t i = 0; i < points.size(); ++i)
        {
            const float x = static_cast<float>(i) / static_cast<float>(points.size() - 1);
            const auto px = plot.getX() + x * plot.getWidth();
            const auto py = plot.getBottom() - points[i] * plot.getHeight();
            g.setColour(theme.background);
            g.fillEllipse(px - 3.0f, py - 3.0f, 6.0f, 6.0f);
            g.setColour(theme.accent);
            g.drawEllipse(px - 3.0f, py - 3.0f, 6.0f, 6.0f, 1.2f);
        }
    }

    void mouseDown(const juce::MouseEvent &e) override
    {
        if (e.mods.isPopupMenu())
        {
            showPresetMenu();
            return;
        }

        if (e.getNumberOfClicks() >= 2)
        {
            setPoints(presetCurve(VelocityCurve::linear), instrument);
            owner.curveChanged(instrument, points);
            return;
        }

        dragPoint(e);
    }

    void mouseDrag(const juce::MouseEvent &e) override { dragPoint(e); }

  private:
    void dragPoint(const juce::MouseEvent &e)
    {
        auto plot = getLocalBounds().toFloat().reduced(8.0f, 6.0f);
        if (plot.getWidth() <= 1.0f)
            return;

        const auto xNorm = juce::jlimit(0.0f, 1.0f, (e.position.x - plot.getX()) / plot.getWidth());
        const auto index = juce::jlimit(0, 7, juce::roundToInt(xNorm * 7.0f));
        const auto yNorm = juce::jlimit(0.0f, 1.0f, 1.0f - (e.position.y - plot.getY()) / plot.getHeight());

        points[static_cast<size_t>(index)] = yNorm;
        repaint();
        owner.curveChanged(instrument, points);
    }

    void showPresetMenu()
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Linear");
        menu.addItem(2, "Logarithmic");
        menu.addItem(3, "Exponential");
        menu.addItem(4, "S-Curve");

        auto safeThis = juce::Component::SafePointer<VelocityCurveEditor>(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                           [safeThis](int result)
                           {
                               if (safeThis == nullptr || result == 0)
                                   return;

                               const auto curve = result == 2   ? VelocityCurve::logarithmic
                                                  : result == 3 ? VelocityCurve::exponential
                                                  : result == 4 ? VelocityCurve::sCurve
                                                                : VelocityCurve::linear;
                               safeThis->setPoints(presetCurve(curve), safeThis->instrument);
                               safeThis->owner.curveChanged(safeThis->instrument, safeThis->points);
                           });
    }

    MidiPage &owner;
    int instrument = 0;
    std::array<float, 8> points {};
    Theme theme = Theme::suite();
};

} // namespace

//==============================================================================
class MidiPage::HitRow : public juce::Component
{
  public:
    HitRow(MidiPage &ownerIn, int instrumentIn) : owner(ownerIn), instrument(instrumentIn)
    {
        for (int i = 0; i < 3; ++i)
        {
            slots[static_cast<size_t>(i)] = std::make_unique<NoteSlot>(owner, instrument, i);
            addAndMakeVisible(*slots[static_cast<size_t>(i)]);
        }

        curve = std::make_unique<VelocityCurveEditor>(owner);
        addAndMakeVisible(*curve);
        setTheme(owner.theme);
        refresh();
    }

    void setTheme(const Theme &t)
    {
        theme = t;
        for (auto &slot : slots)
            slot->setTheme(t);
        curve->setTheme(t);
        repaint();
    }

    void refresh()
    {
        const auto mapSlots = owner.processor.getMidiMap().noteSlotsForInstrument(instrument);

        for (int i = 0; i < 3; ++i)
            slots[static_cast<size_t>(i)]->setNote(mapSlots[static_cast<size_t>(i)]);

        const auto params = owner.processor.getSampler().getInstrumentParams(instrument);
        curve->setPoints(curveForParams(params), instrument);
        curve->setVisible(instrument < owner.processor.getSampler().numInstruments());

        for (int i = 0; i < 3; ++i)
            slots[static_cast<size_t>(i)]->setLearning(owner.learningInstrument == instrument
                                                       && owner.learningSlot == i);
    }

    void paint(juce::Graphics &g) override
    {
        auto b = getLocalBounds().toFloat().reduced(2.0f);
        g.setColour(instrument % 2 == 0 ? theme.panel.withAlpha(0.72f) : theme.panel.withAlpha(0.50f));
        g.fillRoundedRectangle(b, 7.0f);
        g.setColour(theme.panelEdge.withAlpha(0.65f));
        g.drawRoundedRectangle(b.reduced(0.5f), 7.0f, 1.0f);

        g.setColour(theme.inkSoft);
        g.setFont(uiFont(9.0f).boldened());
        g.drawText(juce::String(instrument + 1).paddedLeft('0', 2),
                   juce::Rectangle<int>(16, 10, 28, 16),
                   juce::Justification::centredLeft,
                   false);

        g.setColour(theme.ink);
        g.setFont(displayFont(17.0f));
        g.drawFittedText(owner.processor.getMidiMapInstrumentName(instrument),
                         juce::Rectangle<int>(46, 8, 170, 22),
                         juce::Justification::centredLeft,
                         1,
                         0.75f);

        g.setColour(theme.inkSoft);
        g.setFont(uiFont(9.0f));
        g.drawText("MIDI NOTES", juce::Rectangle<int>(16, 40, 220, 12), juce::Justification::centredLeft, false);
        g.drawText("VELOCITY RESPONSE", juce::Rectangle<int>(getWidth() - 300, 8, 280, 12),
                   juce::Justification::centredRight,
                   false);

        for (int i = 0; i < 3; ++i)
        {
            const auto labelArea = juce::Rectangle<int>(16 + i * 100, 54, 92, 12);
            g.setColour(theme.inkSoft.withAlpha(0.8f));
            g.setFont(uiFont(8.5f));
            g.drawText("NOTE " + juce::String(i + 1), labelArea, juce::Justification::centredLeft, false);
        }
    }

    void resized() override
    {
        const int left = 16;
        const int top = 54;
        const int slotWidth = 92;
        const int slotHeight = 26;

        for (int i = 0; i < 3; ++i)
            slots[static_cast<size_t>(i)]->setBounds(left + i * (slotWidth + 8), top, slotWidth, slotHeight);

        const int curveX = std::max(left + 3 * (slotWidth + 8) + 20, getWidth() - 300);
        curve->setBounds(curveX, 24, std::max(160, getWidth() - curveX - 16), 56);
    }

  private:
    MidiPage &owner;
    int instrument = 0;
    std::array<std::unique_ptr<NoteSlot>, 3> slots;
    std::unique_ptr<VelocityCurveEditor> curve;
    Theme theme = Theme::suite();
};

//==============================================================================
MidiPage::MidiPage(PluginProcessor &processorIn) : processor(processorIn)
{
    setOpaque(true);
    addAndMakeVisible(viewport);
    viewport.setViewedComponent(&content, false);
    viewport.setScrollBarsShown(true, false);

    addAndMakeVisible(closeButton);
    closeButton.onClick = [this]
    {
        cancelNoteLearn();
        if (auto *editor = findParentComponentOfClass<juce::Component>())
            juce::ignoreUnused(editor);
        if (onCloseRequested)
            onCloseRequested();
    };
    closeButton.setColour(juce::TextButton::textColourOffId, theme.ink);
    closeButton.setColour(juce::TextButton::buttonColourId, theme.panel);
    closeButton.setColour(juce::TextButton::buttonOnColourId, theme.accent);

    refresh();
    startTimerHz(8);
}

MidiPage::~MidiPage() = default;

void MidiPage::setTheme(const Theme &newTheme)
{
    theme = newTheme;
    closeButton.setColour(juce::TextButton::textColourOffId, theme.ink);
    closeButton.setColour(juce::TextButton::buttonColourId, theme.panel);
    closeButton.setColour(juce::TextButton::buttonOnColourId, theme.accent);

    for (auto &row : rows)
        row->setTheme(theme);

    repaint();
}

void MidiPage::refresh()
{
    rebuildRows();
    refreshRows();
}

void MidiPage::rebuildRows()
{
    const int count = processor.getMidiMapInstrumentCount();

    if (static_cast<int>(rows.size()) == count)
        return;

    rows.clear();
    content.removeAllChildren();

    for (int i = 0; i < count; ++i)
    {
        auto row = std::make_unique<HitRow>(*this, i);
        content.addAndMakeVisible(*row);
        rows.push_back(std::move(row));
    }

    syncScrollContent();
    repaint();
}

void MidiPage::refreshRows()
{
    for (auto &row : rows)
        row->refresh();

    learningInstrument = -1;
    learningSlot = -1;

    if (const auto *id = processor.getMidiMap().learningControlId())
    {
        const juce::String control(id->c_str());

        if (control.startsWith("note"))
        {
            const auto payload = control.substring(4);
            const auto colon = payload.indexOfChar(':');
            if (colon >= 0)
            {
                learningInstrument = payload.substring(0, colon).getIntValue();
                learningSlot = payload.substring(colon + 1).getIntValue();
            }
        }
    }

    for (auto &row : rows)
        row->refresh();
}

void MidiPage::syncScrollContent()
{
    constexpr int rowHeight = 94;
    const int height = juce::jmax(viewport.getHeight(), static_cast<int>(rows.size()) * rowHeight + 8);
    content.setSize(juce::jmax(200, viewport.getWidth() - 12), height);

    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        rows[static_cast<size_t>(i)]->setBounds(0, i * rowHeight, content.getWidth(), rowHeight);
}

void MidiPage::noteSlotChanged(int instrumentIndex, int slot, int note)
{
    if (processor.getMidiMap().setNoteSlot(instrumentIndex, slot, note))
    {
        processor.saveCustomMapPreset("Custom");
        refreshRows();
    }
}

void MidiPage::armNoteLearn(int instrumentIndex, int slot)
{
    learningInstrument = instrumentIndex;
    learningSlot = slot;
    processor.getMidiMap().beginLearn("note" + std::to_string(instrumentIndex) + ":" + std::to_string(slot), true);
    refreshRows();
}

void MidiPage::cancelNoteLearn()
{
    processor.getMidiMap().cancelLearn();
    learningInstrument = -1;
    learningSlot = -1;
}

void MidiPage::curveChanged(int instrumentIndex, const std::array<float, 8> &points)
{
    if (instrumentIndex < 0 || instrumentIndex >= processor.getSampler().numInstruments())
        return;

    auto params = processor.getSampler().getInstrumentParams(instrumentIndex);
    params.useCustomVelocityCurve = true;
    params.customVelocityCurve = points;
    processor.getSampler().setInstrumentParams(instrumentIndex, params);
    processor.getSettings().markDirty();
}

void MidiPage::timerCallback()
{
    const auto count = static_cast<int>(processor.getMidiMap().noteAssignments().size());
    const bool learning = processor.getMidiMap().isLearning();
    const int instrumentCount = processor.getMidiMapInstrumentCount();

    if (count != lastAssignmentCount || learning != lastLearningState
        || instrumentCount != lastInstrumentCount)
    {
        lastAssignmentCount = count;
        lastLearningState = learning;
        lastInstrumentCount = instrumentCount;
        refresh();
    }
}

void MidiPage::paint(juce::Graphics &g)
{
    g.fillAll(theme.background);

    g.setColour(theme.ink);
    g.setFont(displayFont(30.0f));
    g.drawText("MIDI MAPPING", juce::Rectangle<int>(20, 10, 320, 34), juce::Justification::centredLeft, false);

    g.setColour(theme.inkSoft);
    g.setFont(uiFont(11.5f));
    g.drawText("Up to three notes per hit. Click a note, drag the points, or use LRN.",
               juce::Rectangle<int>(20, 42, 760, 16),
               juce::Justification::centredLeft,
               false);

    g.setColour(theme.panelEdge);
    g.drawHorizontalLine(56, 20.0f, static_cast<float>(getWidth() - 20));
}

void MidiPage::resized()
{
    auto b = getLocalBounds();
    b.removeFromTop(66);

    closeButton.setBounds(getWidth() - 150, 14, 132, 30);
    viewport.setBounds(b.reduced(12, 4));
    syncScrollContent();
}

} // namespace glorp
