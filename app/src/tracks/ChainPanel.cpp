#include "ChainPanel.h"
#include "../ui/I18n.h"

#include <cmath>

namespace aicore::tracks
{
using namespace aicore::ui;

namespace
{
// key, card, title, unit, low, high, decimals, default  (multitrack.py CHAIN_KNOBS)
struct KnobDef { const char* key; const char* card; const char* title; const char* unit; double lo, hi; int dec; double def; };
const KnobDef kKnobs[] = {
    { "gainDb", "gain", "Volume", "dB", -24, 12, 1, 0.0 },   { "lowDb", "eq", "Low", "dB", -12, 12, 1, 0.0 },
    { "midDb", "eq", "Mid", "dB", -12, 12, 1, 0.0 },         { "highDb", "eq", "High", "dB", -12, 12, 1, 0.0 },
    { "highpassHz", "hp", "HP", "Hz", 0, 1000, 0, 0.0 },     { "thresholdDb", "comp", "Threshold", "dB", -60, 0, 1, -18.0 },
    { "ratio", "comp", "Ratio", ":1", 1, 10, 1, 2.0 },       { "targetLufs", "lufs", "Target", "LUFS", -40, 0, 1, 0.0 },
    { "ceilingDb", "limit", "Ceiling", "dB", -12, 0, 1, -1.0 }, { "fadeInS", "fade", "Fade in", "s", 0, 60, 1, 0.0 },
    { "fadeOutS", "fade", "Fade out", "s", 0, 60, 1, 0.0 } };
const std::pair<const char*, const char*> kCards[] = { { "gain", "Volume" }, { "eq", "EQ" }, { "hp", "High-pass" },
                                                       { "comp", "Compressor" }, { "lufs", "LUFS" }, { "limit", "Limiter" },
                                                       { "fade", "Fade" } };
constexpr int knobW = 78, knobH = 84, bandH = 30;

juce::String fmt (double v, int dec, const juce::String& unit)
{
    return juce::String (v, dec) + (unit.trim().isNotEmpty() ? " " + unit.trim() : juce::String());
}

juce::Colour mixc (juce::Colour a, juce::Colour b, float f)
{
    return juce::Colour ((juce::uint8) (a.getRed() + (b.getRed() - a.getRed()) * f), (juce::uint8) (a.getGreen() + (b.getGreen() - a.getGreen()) * f),
                         (juce::uint8) (a.getBlue() + (b.getBlue() - a.getBlue()) * f));
}
} // namespace

//==============================================================================
Knob::Knob (juce::String t, juce::String u, double l, double h, int d, double df)
    : title (std::move (t)), unit (std::move (u)), lo (l), hi (h), def (df), decimals (d), v (df)
{
    setSize (knobW, knobH);
    setRepaintsOnMouseActivity (true);
}

void Knob::setValue (double nv, bool notify)
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

juce::Rectangle<float> Knob::knobRect() const
{
    const float top = title.isNotEmpty() ? 22.0f : 2.0f;
    const float side = juce::jmax (20.0f, juce::jmin ((float) getWidth() - 4.0f, (float) getHeight() - top - 22.0f));
    return { ((float) getWidth() - side) / 2.0f, top, side, side };
}

juce::Rectangle<float> Knob::valueRect() const
{
    auto k = knobRect();
    return { 0.0f, k.getBottom() + 2.0f, (float) getWidth(), 20.0f };
}

void Knob::paint (juce::Graphics& g)
{
    const float alpha = isEnabled() ? 1.0f : 0.45f;
    const bool live = isEnabled() && (over || dragging);
    if (title.isNotEmpty())
    {
        g.setColour (col::muted.withMultipliedAlpha (alpha));
        g.setFont (uiFont (14.0f));
        g.drawText (aicore::ui::tr (title), 0, 0, getWidth(), 20, juce::Justification::centred);
    }
    auto r = knobRect();
    const float radius = r.getWidth() / 2, ring = juce::jlimit (1.5f, 3.0f, radius * 0.11f), arcR = radius - ring / 2;
    const auto c = r.getCentre();
    const float pi = juce::MathConstants<float>::pi;
    // Qt draws from 225 deg (counter-clockwise from 3 o'clock) sweeping -270: in JUCE (clockwise from 12) that is
    // -135 deg to +135 deg
    auto arc = [&] (float from, float to) {
        juce::Path p;
        p.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, from, to, true);
        return p;
    };
    const float a0 = -0.75f * pi, span = 1.5f * pi;
    g.setColour (juce::Colour (0xffc3ccd6).withMultipliedAlpha (alpha));
    g.strokePath (arc (a0, a0 + span), juce::PathStrokeType (ring, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    const bool bipolar = lo < 0 && 0 < hi;
    const double zero = bipolar ? (0 - lo) / (hi - lo) : 0.0, pos = (v - lo) / (hi - lo);
    const double fa = juce::jmin (zero, pos), fb = juce::jmax (zero, pos);
    if (fb - fa > 0.002)
    {
        auto ac = arcColour.withMultipliedAlpha (alpha);
        g.setColour (live ? ac.darker (0.15f) : ac);
        g.strokePath (arc (a0 + (float) fa * span, a0 + (float) fb * span),
                      juce::PathStrokeType (ring, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    const float body = radius - ring - juce::jmax (2.0f, radius * 0.1f);
    juce::Rectangle<float> disc (c.x - body, c.y - body, body * 2, body * 2);
    g.setColour (juce::Colour::fromRGBA (0, 0, 0, (juce::uint8) (70 * alpha)));
    g.fillEllipse (disc.translated (0, juce::jmax (1.0f, body * 0.06f)));
    g.setGradientFill (juce::ColourGradient ((live ? juce::Colour (0xff4c5568) : juce::Colour (0xff3d4555)).withMultipliedAlpha (alpha), c.x, disc.getY(),
                                             juce::Colour (0xff22272f).withMultipliedAlpha (alpha), c.x, disc.getBottom(), false));
    g.fillEllipse (disc.reduced (0.5f));
    g.setColour (juce::Colour (0xff5d6880).withMultipliedAlpha (alpha));
    g.drawEllipse (disc.reduced (0.5f), 1.0f);
    const float ang = a0 + (float) pos * span; // JUCE: 0 = up, clockwise
    auto point = [&] (float dist) { return juce::Point<float> (c.x + std::sin (ang) * dist, c.y - std::cos (ang) * dist); };
    g.setColour (juce::Colours::white.withMultipliedAlpha (alpha));
    g.drawLine ({ point (body * 0.25f), point (body * 0.92f) }, juce::jlimit (1.8f, 2.8f, body * 0.13f));
    const float tipR = juce::jlimit (2.2f, 3.6f, ring * 0.95f);
    const auto tip = point (arcR);
    g.setColour (arcColour.withMultipliedAlpha (alpha));
    g.fillEllipse (tip.x - tipR - 1, tip.y - tipR - 1, (tipR + 1) * 2, (tipR + 1) * 2);
    g.setColour (juce::Colours::white.withMultipliedAlpha (alpha));
    g.fillEllipse (tip.x - tipR, tip.y - tipR, tipR * 2, tipR * 2);
    if (editor == nullptr)
    {
        auto box = valueRect().reduced (3.0f, 1.0f);
        g.setColour (col::field.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (box, 2.0f);
        g.setColour (col::outline.withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (box, 2.0f, 1.0f);
        g.setColour (col::text.withMultipliedAlpha (alpha));
        g.setFont (uiFont (13.0f));
        g.drawText (fmt (v, decimals, showUnit ? unit : juce::String()), box, juce::Justification::centred);
    }
}

void Knob::mouseDown (const juce::MouseEvent&)
{
    dragging = true;
    dragStart = v;
}

void Knob::mouseDrag (const juce::MouseEvent& e)
{
    const double pixels = e.mods.isShiftDown() ? 600.0 : 160.0;
    setValue (dragStart + (-e.getDistanceFromDragStartY()) / pixels * (hi - lo));
}

void Knob::mouseUp (const juce::MouseEvent&)
{
    dragging = false;
    repaint();
}

void Knob::mouseDoubleClick (const juce::MouseEvent&)
{
    editor = std::make_unique<juce::TextEditor>();
    editor->setJustification (juce::Justification::centred);
    editor->setFont (uiFont (13.0f));
    editor->setText (juce::String (v, decimals), false);
    editor->setBounds (valueRect().toNearestInt().reduced (2, 0));
    addAndMakeVisible (*editor);
    editor->selectAll();
    editor->grabKeyboardFocus();
    auto done = [this] {
        if (editor == nullptr)
            return;
        const auto text = editor->getText();
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<Knob> (this), text] {
            if (safe == nullptr)
                return;
            safe->editor.reset();
            safe->setValue (text.getDoubleValue());
            safe->repaint();
        });
    };
    editor->onReturnKey = done;
    editor->onFocusLost = done;
    editor->onEscapeKey = [this] { juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<Knob> (this)] { if (safe) { safe->editor.reset(); safe->repaint(); } }); };
}

void Knob::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    const double step = std::pow (10.0, -decimals);
    setValue (v + (w.deltaY > 0 ? 1 : -1) * juce::jmax (step, (hi - lo) / 100.0));
}

//==============================================================================
ChainPanel::ChainPanel()
{
    for (auto& [key, title] : kCards)
    {
        auto* c = cards.add (new Card());
        c->key = key;
        c->title = title;
        c->removable = juce::StringArray { "eq", "hp", "comp", "lufs", "limit" }.contains (key);
        c->toggle = c->key == "comp" || c->key == "limit";
        for (auto& k : kKnobs)
            if (c->key == k.card)
            {
                auto* kn = c->knobs.add (new Knob (k.title, k.unit, k.lo, k.hi, k.dec, k.def));
                kn->setComponentID (k.key);
                kn->onChange = [this] (double) { emit(); };
                addChildComponent (kn);
            }
    }
    for (auto* b : { &masterButton, &trackButton, &addButton, &resetButton })
        addAndMakeVisible (b);
    masterButton.setClickingTogglesState (false);
    trackButton.setClickingTogglesState (false);
    masterButton.onClick = [this] { if (onPathChosen) onPathChosen (true); };
    trackButton.onClick = [this] { if (onPathChosen) onPathChosen (false); };
    resetButton.setTooltip ("Every knob back to where it starts; the effects leave the chain");
    resetButton.onClick = [this] { reset(); };
    addButton.onClick = [this] {
        juce::PopupMenu m;
        int id = 1;
        juce::StringArray keys;
        for (auto* c : cards)
            if (c->removable && ! c->shown)
            {
                m.addItem (id++, c->title);
                keys.add (c->key);
            }
        if (keys.isEmpty())
            m.addItem (-1, "Every effect is in the chain", false);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addButton), [this, keys] (int r) {
            if (r >= 1 && r <= keys.size())
                addEffect (keys[r - 1]);
        });
    };
    setPath (true);
}

ChainPanel::Card* ChainPanel::card (const juce::String& key)
{
    for (auto* c : cards)
        if (c->key == key)
            return c;
    return nullptr;
}

void ChainPanel::setPath (bool m)
{
    masterButton.setToggleState (m, juce::dontSendNotification);
    trackButton.setToggleState (! m, juce::dontSendNotification);
}

bool ChainPanel::active (const Card& c) const
{
    if (c.key == "eq")
        return std::abs (c.knobs[0]->value()) > 1e-9 || std::abs (c.knobs[1]->value()) > 1e-9 || std::abs (c.knobs[2]->value()) > 1e-9;
    if (c.key == "hp")
        return c.knobs[0]->value() > 0;
    if (c.key == "lufs")
        return std::abs (c.knobs[0]->value()) > 1e-9;
    return c.on;
}

void ChainPanel::showSettings (const ChainSettings& s, bool m, juce::Colour t)
{
    loading = true;
    master = m;
    tint = t;
    for (auto* c : cards)
    {
        for (auto* k : c->knobs)
        {
            const std::string key = k->getComponentID().toStdString();
            k->setValue (s.count (key) ? s.at (key) : k->def, false);
            k->arcColour = t.isTransparent() ? col::accent : t.darker (0.15f);
        }
        if (c->toggle)
            c->on = chainValue (s, c->key == "comp" ? "compress" : "limit") != 0;
        c->shown = insertsMode ? c->key != "gain" : c->key == "gain" || c->key == "fade" ? master : active (*c);
    }
    loading = false;
    setPath (m);
    resized();
    repaint();
}

ChainSettings ChainPanel::settings() const
{
    ChainSettings s;
    for (auto* c : cards)
    {
        for (auto* k : c->knobs)
        {
            const auto key = k->getComponentID();
            if (insertsMode ? key != "gainDb" : (master || ! juce::StringArray { "gainDb", "fadeInS", "fadeOutS" }.contains (key)))
                s[key.toStdString()] = k->value();
        }
        if (c->toggle)
            s[c->key == "comp" ? "compress" : "limit"] = c->on ? 1.0 : 0.0;
    }
    return s;
}

void ChainPanel::emit()
{
    if (! loading && onChanged)
        onChanged (settings());
    repaint();
}

void ChainPanel::addEffect (const juce::String& key)
{
    if (auto* c = card (key))
    {
        c->shown = true;
        if (c->toggle)
            c->on = true; // a compressor or limiter is added to be used: it starts on
    }
    resized();
    emit();
}

void ChainPanel::removeEffect (const juce::String& key)
{
    if (auto* c = card (key))
    {
        loading = true;
        for (auto* k : c->knobs)
            k->setValue (k->def, false);
        c->on = false;
        c->shown = false;
        loading = false;
    }
    resized();
    emit();
}

void ChainPanel::reset()
{
    loading = true;
    for (auto* c : cards)
    {
        for (auto* k : c->knobs)
            k->setValue (k->def, false);
        c->on = false;
        c->shown = c->key == "gain" || c->key == "fade" ? master : false;
    }
    loading = false;
    resized();
    emit();
}

void ChainPanel::setVertical (const juce::String& h)
{
    vertical = true;
    heading = h;
    masterButton.setVisible (false);
    trackButton.setVisible (false);
    resized();
}

void ChainPanel::setInsertsMode (const juce::String& h, bool m)
{
    insertsMode = vertical = true;
    heading = h;
    master = m;
    for (auto* c : cards)
        for (auto* k : c->knobs) // trackview.py INSERT_OF_COLUMN: only HP (Hz) and the fades (s) carry a unit
            k->showUnit = juce::StringArray { "highpassHz", "fadeInS", "fadeOutS" }.contains (k->getComponentID());
    for (auto* b : { &masterButton, &trackButton, &addButton, &resetButton })
        b->setVisible (false);
    for (auto* c : cards)
    {
        c->removable = false;
        c->shown = c->key != "gain";
    }
    resized();
}

void ChainPanel::resized()
{
    auto r = getLocalBounds();
    if (insertsMode)
    {
        int y = 36;
        for (auto* c : cards)
        {
            auto allowed = [&] (Knob* k) {
                if (c->key == "comp" || c->key == "gain")
                    return false;
                if (c->key == "limit")
                    return master; // the Ceiling, on the master only
                juce::ignoreUnused (k);
                return true;
            };
            bool any = false;
            for (auto* k : c->knobs)
            {
                const bool vis = c->shown && ! c->folded && allowed (k);
                k->setVisible (vis);
                any = any || (c->shown && allowed (k));
            }
            if (! c->shown)
                continue;
            const int h = c->folded ? bandH + 2 : any ? bandH + 6 + knobH + 14 : bandH + 8;
            c->bounds = { 0, y, getWidth(), h };
            c->band = c->bounds.withHeight (bandH);
            c->close = {};
            c->fold = c->band.withLeft (c->band.getRight() - 28).withWidth (26).reduced (3);
            c->dot = c->toggle ? juce::Rectangle<int> (c->band.getX() + 10, c->band.getCentreY() - 6, 12, 12) : juce::Rectangle<int>();
            int kx = 10;
            for (auto* k : c->knobs)
                if (k->isVisible())
                {
                    k->setBounds (kx, c->band.getBottom() + 6, knobW, knobH);
                    kx += knobW + 4;
                }
            y += h + 6;
        }
        contentHeight = y;
        return;
    }
    if (vertical)
    {
        r.removeFromTop (36); // INSERTS · MASTER
        auto buttons = r.removeFromBottom (r.getHeight()); // placed after the cards
        int shownCount = 0;
        for (auto* c : cards)
            shownCount += c->shown ? 1 : 0;
        const int full = bandH + knobH + 36;
        const int avail = getHeight() - 36 - 44;
        // the old column: every card 206 px (its knobs at the top), Add effect / Reset right under the last one
        const int each = juce::jmax (full, juce::jmin (206, shownCount > 0 ? (avail - 8 * (shownCount - 1)) / shownCount : 206));
        int y = 36;
        for (auto* c : cards)
        {
            for (auto* k : c->knobs)
                k->setVisible (c->shown && ! c->folded);
            if (! c->shown)
                continue;
            const int h = c->folded ? bandH + 2 : each;
            c->bounds = { 0, y, getWidth(), h };
            c->band = c->bounds.withHeight (bandH);
            c->close = c->removable ? c->band.withLeft (c->band.getRight() - 28).reduced (3) : juce::Rectangle<int>();
            c->fold = c->key == "gain" ? juce::Rectangle<int>() : c->band.withLeft (c->band.getRight() - (c->removable ? 54 : 28)).withWidth (26).reduced (3);
            c->dot = c->toggle ? juce::Rectangle<int> (c->band.getX() + 10, c->band.getCentreY() - 6, 12, 12) : juce::Rectangle<int>();
            int kx = 10;
            for (auto* k : c->knobs)
            {
                k->setBounds (kx, c->band.getBottom() + 6, knobW, knobH);
                kx += knobW + 4;
            }
            y += h + 8;
        }
        const int bw = (getWidth() - 6) / 2;
        addButton.setBounds (0, y, bw, 30);
        resetButton.setBounds (bw + 6, y, getWidth() - bw - 6, 30);
        juce::ignoreUnused (buttons);
        return;
    }
    auto path = r.removeFromLeft (170).reduced (8, 0);
    path.removeFromTop (34); // SIGNAL PATH heading
    for (auto* b : { &masterButton, &trackButton, &addButton, &resetButton })
    {
        b->setBounds (path.removeFromTop (30));
        path.removeFromTop (6);
    }
    r.removeFromLeft (8);
    int x = r.getX();
    for (auto* c : cards)
    {
        const bool vis = c->shown;
        for (auto* k : c->knobs)
            k->setVisible (vis && ! c->folded);
        if (! vis)
            continue;
        // wide enough for the knobs and for the band (dot, title, ▾, ×), as the old cards
        const int titleW = juce::roundToInt (juce::GlyphArrangement::getStringWidth (uiFont (14.0f, true).withExtraKerningFactor (0.07f), c->title.toUpperCase()));
        const int bandW = 10 + (c->toggle ? 20 : 0) + titleW + 16 + (c->removable ? 56 : 30);
        const int w = juce::jmax (100, juce::jmax ((int) c->knobs.size() * knobW + 20, bandW));
        const int h = c->folded ? bandH + 2 : juce::jmax (bandH + knobH + 36, r.getHeight() - 14); // the cards fill the dock, as the old ones
        c->bounds = { x, r.getY() + 6, w, h };
        c->band = c->bounds.withHeight (bandH);
        c->close = c->removable ? c->band.withLeft (c->band.getRight() - 28).reduced (3) : juce::Rectangle<int>();
        c->fold = c->key == "gain" ? juce::Rectangle<int>() : c->band.withLeft (c->band.getRight() - (c->removable ? 54 : 28)).withWidth (26).reduced (3);
        c->dot = c->toggle ? juce::Rectangle<int> (c->band.getX() + 10, c->band.getCentreY() - 6, 12, 12) : juce::Rectangle<int>();
        int kx = x + (w - (int) c->knobs.size() * knobW) / 2;
        for (auto* k : c->knobs)
        {
            k->setBounds (kx, c->band.getBottom() + 6, knobW, knobH);
            kx += knobW;
        }
        x += w + 8;
    }
}

void ChainPanel::paint (juce::Graphics& g)
{
    g.setColour (col::title);
    g.setFont (uiFont (15.0f, true).withExtraKerningFactor (0.1f));
    if (vertical)
        g.drawText (aicore::ui::tr (heading), 4, 0, getWidth(), 30, juce::Justification::centredLeft);
    else
        g.drawText (aicore::ui::tr ("SIGNAL PATH"), 12, 0, 160, 30, juce::Justification::centredLeft);
    const auto bandColour = tint.isTransparent() ? juce::Colour (0xff4c5568) : mixc (juce::Colour (0xff4c5568), tint, 0.30f);
    const auto edgeColour = tint.isTransparent() ? col::edge : tint;
    for (auto* c : cards)
    {
        if (! c->shown)
            continue;
        auto b = c->bounds.toFloat();
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfff6f8fa), 0, b.getY(), juce::Colour (0xffe9edf1), 0, b.getBottom(), false));
        g.fillRoundedRectangle (b, 2.0f);
        g.setColour (col::outline);
        g.drawRoundedRectangle (b.reduced (0.5f), 2.0f, 1.0f);
        g.setColour (col::cardEdge);
        g.fillRect (b.getX() + 1, b.getBottom() - 2, b.getWidth() - 2, 2.0f);
        auto band = c->band.toFloat();
        g.setColour (bandColour);
        g.fillRect (band);
        g.setColour (edgeColour);
        g.fillRect (band.withHeight (3.0f));
        float tx = band.getX() + 10;
        if (c->toggle)
        {
            auto d = c->dot.toFloat();
            g.setColour (col::selectedText.withAlpha (c->on ? 1.0f : 0.35f));
            g.fillEllipse (d);
            tx = d.getRight() + 8;
        }
        g.setColour (col::selectedText);
        g.setFont (uiFont (14.0f, true).withExtraKerningFactor (0.07f));
        g.drawText (aicore::ui::upper (aicore::ui::tr (c->title)), juce::Rectangle<float> (tx, band.getY() + 2, band.getWidth() - (tx - band.getX()) - (c->removable ? 56 : 30), band.getHeight() - 2),
                    juce::Justification::centredLeft);
        // ▾ fold and × remove, light marks on the band
        g.setColour (col::selectedText);
        auto f = c->fold.toFloat().reduced (6.0f);
        if (! c->fold.isEmpty())
        {
        juce::Path chevron;
        if (c->folded)
            chevron.addTriangle (f.getX() + 2, f.getY(), f.getX() + 2, f.getBottom(), f.getRight() - 1, f.getCentreY());
        else
        {
            chevron.startNewSubPath (f.getX(), f.getY() + 2);
            chevron.lineTo (f.getCentreX(), f.getBottom() - 2);
            chevron.lineTo (f.getRight(), f.getY() + 2);
        }
        g.strokePath (chevron, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        if (c->removable)
        {
            auto x = c->close.toFloat().reduced (6.0f);
            g.drawLine (x.getX(), x.getY(), x.getRight(), x.getBottom(), 1.6f);
            g.drawLine (x.getRight(), x.getY(), x.getX(), x.getBottom(), 1.6f);
        }
    }
}

void ChainPanel::mouseUp (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    for (auto* c : cards)
    {
        if (! c->shown)
            continue;
        if (c->removable && c->close.contains (p))
        {
            removeEffect (c->key);
            return;
        }
        if (c->fold.contains (p))
        {
            c->folded = ! c->folded;
            resized();
            repaint();
            return;
        }
        if (c->toggle && c->dot.expanded (4).contains (p))
        {
            c->on = ! c->on;
            emit();
            return;
        }
    }
}
} // namespace aicore::tracks
