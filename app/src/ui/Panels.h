// AICore-C — the old AICORE's panel pieces: the centre / library card (QWidget#centrePanel, #sourceLibrary,
// #inspectorPanel), the panel heading (QLabel#panelHeading, small capitals, 1.5 px spacing) with its thin lines,
// and the bar spin box (widgets.py core_spin style "bar": the value as a bar, drag sideways, double-click to type).
#pragma once

#include "AicoreTheme.h"
#include "I18n.h"

#include <cmath>

namespace aicore::ui
{
inline void drawPanel (juce::Graphics& g, juce::Rectangle<int> box)
{
    auto r = box.toFloat();
    g.setGradientFill (juce::ColourGradient (col::centreTop, 0, r.getY(), col::centreBottom, 0, r.getBottom(), false));
    g.fillRoundedRectangle (r, 2.0f);
    g.setColour (col::outline);
    g.drawRoundedRectangle (r.reduced (0.5f), 2.0f, 1.0f);
    g.setColour (col::cardEdge);
    g.fillRect (r.getX() + 1, r.getBottom() - 2, r.getWidth() - 2, 2.0f);
}

/** A section heading at (x, y) (its text box 26 px tall) and the thin line under it (y + 30); returns the y
    under the line. lineBefore: the line above too (a heading that is not the first in its panel). */
inline int drawHeading (juce::Graphics& g, const juce::String& text, int x, int y, int w, bool lineBefore = false)
{
    if (lineBefore)
    {
        g.setColour (col::grid);
        g.fillRect (x, y - 8, w, 1);
    }
    g.setColour (col::title);
    g.setFont (uiFont (15.0f, true).withExtraKerningFactor (0.1f));
    g.drawText (aicore::ui::upper (tr (text)), x + 4, y, w - 4, 26, juce::Justification::centredLeft);
    g.setColour (col::grid);
    g.fillRect (x, y + 32, w, 1);
    return y + 40;
}

class BarSpin : public juce::Component, public juce::SettableTooltipClient
{
public:
    BarSpin (double lo_, double hi_, double value, int decimals_, juce::String suffix_ = {}, juce::String special_ = {})
        : lo (lo_), hi (hi_), v (value), decimals (decimals_), suffix (std::move (suffix_)), special (std::move (special_))
    {
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    }
    double value() const { return v; }
    void setValue (double nv, bool notify = true)
    {
        const double step = std::pow (10.0, -decimals);
        nv = juce::jlimit (lo, hi, std::round (nv / step) * step);
        if (nv == v)
            return;
        v = nv;
        repaint();
        if (notify && onChange)
            onChange (v);
    }
    std::function<void (double)> onChange;
    juce::String prefix; // a title inside the bar (widgets.py core_spin title=, e.g. "Epochs  10")
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (isEnabled() ? col::field : col::grid);
        g.fillRoundedRectangle (r, 2.0f);
        const float frac = (float) juce::jlimit (0.0, 1.0, (v - lo) / (hi - lo));
        if (frac > 0)
        {
            g.setColour (col::hover);
            g.fillRoundedRectangle (r.withWidth (r.getWidth() * frac), 2.0f);
        }
        g.setColour (col::outline);
        g.drawRoundedRectangle (r, 2.0f, 1.0f);
        g.setColour (isEnabled() ? col::text : col::dim);
        g.setFont (uiFont (14.0f));
        const auto text = (prefix.isNotEmpty() ? tr (prefix.trim()) + "  " : juce::String()) + ((special.isNotEmpty() && v <= lo) ? tr (special) : juce::String (v, decimals) + suffix);
        g.drawText (text, r, juce::Justification::centred);
    }
    void mouseDown (const juce::MouseEvent&) override { start = v; }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        setValue (start + e.getDistanceFromDragStartX() / juce::jmax (40.0f, (float) getWidth()) * (hi - lo) * (e.mods.isShiftDown() ? 0.25 : 1.0));
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        setValue (v + (w.deltaY > 0 ? 1 : -1) * juce::jmax (std::pow (10.0, -decimals), (hi - lo) / 100.0));
    }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        auto* ed = new juce::TextEditor();
        ed->setJustification (juce::Justification::centred);
        ed->setFont (uiFont (14.0f));
        ed->setText (juce::String (v, decimals), false);
        ed->setBounds (getLocalBounds());
        addAndMakeVisible (ed);
        ed->selectAll();
        ed->grabKeyboardFocus();
        editor.reset (ed);
        auto done = [this] {
            const auto text = editor != nullptr ? editor->getText() : juce::String();
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<BarSpin> (this), text] {
                if (safe == nullptr || safe->editor == nullptr)
                    return;
                safe->editor.reset();
                safe->setValue (text.getDoubleValue());
                safe->repaint();
            });
        };
        ed->onReturnKey = done;
        ed->onFocusLost = done;
    }

private:
    double lo, hi, v, start = 0;
    int decimals;
    juce::String suffix, special;
    std::unique_ptr<juce::TextEditor> editor;
};
} // namespace aicore::ui
