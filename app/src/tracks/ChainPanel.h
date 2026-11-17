// AICore-C — EDIT > TRACKS > CHAIN: the old AICORE's ChainPanel (multitrack.py) and Core's knob (widgets.py
// CoreValue "knob"): one card per insert (VOLUME, EQ, HIGH-PASS, COMPRESSOR, LUFS, LIMITER, FADE), a band with
// the title, ▾ to fold, × to take the effect out; SIGNAL PATH at the left (Master, Chosen track, Add effect, Reset).
#pragma once

#include "MasterChain.h"
#include "../ui/AicoreTheme.h"

namespace aicore::tracks
{
class Knob : public juce::Component, public juce::SettableTooltipClient
{
public:
    Knob (juce::String title, juce::String unit, double lo, double hi, int decimals, double def);
    double value() const { return v; }
    void setValue (double nv, bool notify = true);
    std::function<void (double)> onChange;
    juce::Colour arcColour = ui::col::accent;
    bool showUnit = true; // EDIT > STEMS' inserts show some values bare (0.0, as the old trackview)

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseEnter (const juce::MouseEvent&) override { over = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { over = false; repaint(); }

    const juce::String title, unit;
    const double lo, hi, def;
    const int decimals;

private:
    juce::Rectangle<float> knobRect() const;
    juce::Rectangle<float> valueRect() const;
    double v;
    double dragStart = 0;
    bool dragging = false, over = false;
    std::unique_ptr<juce::TextEditor> editor;
};

class ChainPanel : public juce::Component
{
public:
    ChainPanel();
    /** master: Volume and Fade cards too (MASTER_ONLY). title goes to the dock bar. */
    void showSettings (const ChainSettings&, bool master, juce::Colour tint = {});
    ChainSettings settings() const;
    std::function<void (const ChainSettings&)> onChanged;
    std::function<void (bool master)> onPathChosen; // Master / Chosen track
    void setPath (bool master);
    void addEffect (const juce::String& key);
    /** The chain as a column (EDIT > SAMPLE's Mastering): a heading, the cards one under the other, Add effect and
        Reset under them; no signal path column. */
    void setVertical (const juce::String& heading);
    bool isActive() const { return chainActive (settings()); }
    /** EDIT > STEMS' MIX: a strip's inserts (trackview.py InsertPage): every card always there (EQ, HIGH-PASS,
        COMPRESSOR, LUFS, LIMITER, FADE), no X, no Add effect; the compressor is its circle only, the limiter has its
        Ceiling on the master only. The volume is the strip's fader (not here). */
    void setInsertsMode (const juce::String& heading, bool master);
    void setHeading (const juce::String& h) { heading = h; repaint(); }
    int preferredHeight() const { return contentHeight; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    struct Card
    {
        juce::String key, title;
        bool removable = false, toggle = false; // toggle: compressor / limiter (the dot on the band)
        bool shown = false, folded = false, on = false;
        juce::OwnedArray<Knob> knobs;
        juce::Rectangle<int> bounds, band, dot, fold, close;
    };
    bool active (const Card&) const;
    void emit();
    void removeEffect (const juce::String& key);
    void reset();
    Card* card (const juce::String& key);

    juce::OwnedArray<Card> cards;
    ui::AButton masterButton { "Master" }, trackButton { "Chosen track" }, addButton { "Add effect" }, resetButton { "Reset" };
    bool master = true, loading = false, vertical = false, insertsMode = false;
    int contentHeight = 0;
    juce::String heading;
    juce::Colour tint;
};
} // namespace aicore::tracks
