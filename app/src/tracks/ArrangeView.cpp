#include "ArrangeView.h"
#include "../ui/I18n.h"
#include "../ui/AicoreTheme.h"

#include <cmath>

namespace aicore::tracks
{
using namespace aicore::ui;
namespace
{
constexpr double gainLow = -60.0, gainHigh = 12.0;

juce::Colour mixc (juce::Colour a, juce::Colour b, float f)
{
    return juce::Colour ((juce::uint8) (a.getRed() + (b.getRed() - a.getRed()) * f),
                         (juce::uint8) (a.getGreen() + (b.getGreen() - a.getGreen()) * f),
                         (juce::uint8) (a.getBlue() + (b.getBlue() - a.getBlue()) * f));
}
juce::Colour lighter (juce::Colour c, int factor) { return c.withMultipliedBrightness ((float) factor / 100.0f); }
juce::Colour darker (juce::Colour c, int factor) { return c.withMultipliedBrightness (100.0f / (float) factor); }
} // namespace

juce::Colour trackColour (const juce::String& name, int index)
{
    static const std::pair<const char*, juce::uint32> roles[] = {
        { "kick", 0xffe39d96 }, { "bass sub 303 acid", 0xff6a9cf0 }, { "hat hats ride cymbal", 0xffe0c070 },
        { "clap snare", 0xffd9845a }, { "perc tom conga shaker drum", 0xffddb07c }, { "lead synth arp pluck other", 0xffa98bd0 },
        { "pad atmo drone ambient", 0xff6fb08f }, { "fx riser impact crash sweep noise", 0xffc47fa8 },
        { "vocal vocals voice chop", 0xffc98bb9 } };
    static const juce::uint32 others[] = { 0xff7fa9c6, 0xff8a8fc0, 0xffb9a56a, 0xff6f8fb0, 0xffd88080, 0xff7a9ab8 };
    const auto key = name.toLowerCase();
    for (auto& [words, colour] : roles)
        for (auto& w : juce::StringArray::fromTokens (words, " ", ""))
            if (key.contains (w))
                return juce::Colour (colour);
    return juce::Colour (others[(size_t) index % 6]);
}

ArrangeView::ArrangeView() : Thread ("aicore-peaks")
{
    setWantsKeyboardFocus (true);
    startThread (juce::Thread::Priority::low);
}

ArrangeView::~ArrangeView() { stopThread (5000); }

int ArrangeView::trackAt (float y) const
{
    if (y < rulerH)
        return -1;
    return juce::jmin ((int) session.tracks.size(), (int) ((y - rulerH + scrollY) / rowH));
}

juce::Rectangle<float> ArrangeView::clipRect (int ti, const Clip& c) const
{
    return { (float) xOf (c.start), (float) trackTop (ti) + 4.0f, (float) juce::jmax (2.0, c.length * pps), (float) rowH - 8.0f };
}

Clip* ArrangeView::clipAt (juce::Point<float> p, int& ti)
{
    ti = trackAt (p.y);
    if (ti < 0 || ti >= (int) session.tracks.size() || p.x < headW)
        return nullptr;
    auto& clips = session.tracks[(size_t) ti].clips;
    for (auto it = clips.rbegin(); it != clips.rend(); ++it)
        if (clipRect (ti, *it).contains (p))
            return &*it;
    return nullptr;
}

void ArrangeView::setView (std::optional<double> scroll, std::optional<double> newPps)
{
    if (newPps)
        pps = juce::jlimit (0.5, 4000.0, *newPps);
    if (scroll)
        scrollX = juce::jmax (0.0, *scroll);
    const int n = juce::jmax (1, (int) session.tracks.size());
    rowH = (int) juce::jlimit ((double) rowMin, (double) rowMax, (getHeight() - rulerH - 60) / (double) n);
    scrollY = juce::jmax (0, juce::jmin (scrollY, contentHeight() - getHeight()));
    repaint();
    if (onViewChanged)
        onViewChanged();
}

void ArrangeView::zoom (double factor, std::optional<float> atX)
{
    const float ax = atX ? *atX : headW + (getWidth() - headW) / 2.0f;
    const double anchor = secondsAt (ax);
    const double np = juce::jlimit (0.5, 4000.0, pps * factor);
    setView (anchor - (ax - headW) / np, np);
}

void ArrangeView::zoomAll()
{
    setView (0.0, (getWidth() - headW - 20) / juce::jmax (session.length(), 10.0));
}

void ArrangeView::setSession (const Session& s)
{
    session = s;
    std::set<int> ids;
    for (auto& t : session.tracks)
        for (auto& c : t.clips)
        {
            ids.insert (c.id);
            wantPeaks (c.path);
        }
    for (auto it = selected.begin(); it != selected.end();)
        it = ids.count (*it) ? std::next (it) : selected.erase (it);
    currentTrack = juce::jlimit (0, juce::jmax (0, (int) session.tracks.size() - 1), currentTrack);
    setView();
    if (onSelectionChanged)
        onSelectionChanged();
}

void ArrangeView::setPlayhead (double s, bool follow)
{
    playhead = juce::jmax (0.0, s);
    if (follow)
    {
        const double span = visibleSeconds();
        if (! (scrollX <= playhead && playhead <= scrollX + span * 0.95))
            setView (playhead - span * 0.05);
    }
    repaint();
}

//==============================================================================
void ArrangeView::wantPeaks (const juce::String& path)
{
    const juce::ScopedLock sl (peakLock);
    if (peaks.count (path))
        return;
    peaks[path] = {};
    peakQueue.add (path);
    notify();
}

void ArrangeView::run()
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    while (! threadShouldExit())
    {
        juce::String path;
        {
            const juce::ScopedLock sl (peakLock);
            if (! peakQueue.isEmpty())
            {
                path = peakQueue[0];
                peakQueue.remove (0);
            }
        }
        if (path.isEmpty())
        {
            wait (200);
            continue;
        }
        Peaks pk;
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (juce::File (path)));
        if (r != nullptr && r->lengthInSamples > 0)
        {
            const int cols = 8192;
            const juce::int64 len = r->lengthInSamples;
            pk.seconds = (double) len / r->sampleRate;
            pk.lo.resize (cols);
            pk.hi.resize (cols);
            pk.rms.resize (cols);
            juce::AudioBuffer<float> buf ((int) r->numChannels, (int) juce::jmin<juce::int64> (len, 1 << 16));
            for (int c = 0; c < cols && ! threadShouldExit(); ++c)
            {
                const juce::int64 a = len * c / cols, b = juce::jmax (a + 1, len * (c + 1) / cols);
                const int n = (int) juce::jmin<juce::int64> (b - a, buf.getNumSamples());
                r->read (&buf, 0, n, a, true, true);
                float lo = 0, hi = 0;
                double sq = 0;
                for (int ch = 0; ch < buf.getNumChannels(); ++ch)
                {
                    auto range = juce::FloatVectorOperations::findMinAndMax (buf.getReadPointer (ch), n);
                    lo = juce::jmin (lo, range.getStart());
                    hi = juce::jmax (hi, range.getEnd());
                    const float rm = buf.getRMSLevel (ch, 0, n);
                    sq = juce::jmax (sq, (double) rm);
                }
                pk.lo[(size_t) c] = lo;
                pk.hi[(size_t) c] = hi;
                pk.rms[(size_t) c] = (float) sq;
            }
        }
        pk.ready = true;
        {
            const juce::ScopedLock sl (peakLock);
            peaks[path] = std::move (pk);
        }
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<ArrangeView> (this)] {
            if (safe != nullptr)
                safe->repaint();
        });
    }
}

//==============================================================================
void ArrangeView::paint (juce::Graphics& g)
{
    const int w = getWidth(), h = getHeight();
    g.fillAll (col::panel);
    const double beat = 60.0 / juce::jmax (1.0, session.bpm), bar = beat * 4, barPx = bar * pps;
    const int every = barPx >= 28 ? 1 : barPx >= 14 ? 2 : barPx >= 7 ? 4 : 16;
    const int first = (int) (scrollX / bar), last = (int) ((scrollX + visibleSeconds()) / bar) + 1;
    bool soloed = false;
    for (auto& t : session.tracks)
        soloed = soloed || t.solo;
    const double endX = xOf (session.length());
    for (int ti = 0; ti < (int) session.tracks.size(); ++ti)
    {
        const int top = trackTop (ti);
        if (top > h || top + rowH < rulerH)
            continue;
        const juce::Rectangle<float> lane ((float) headW, (float) top + 3, (float) (w - headW), (float) rowH - 6);
        g.setColour (col::field);
        g.fillRect (lane);
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (lane.toNearestInt());
            if (beat * pps >= 12)
            {
                g.setColour (col::grid);
                for (int i = first * 4; i <= last * 4; ++i)
                    if (i % 4)
                        g.drawVerticalLine ((int) xOf (i * beat), lane.getY(), lane.getBottom());
            }
            g.setColour (col::outline);
            for (int b = first; b <= last; ++b)
                if (b % every == 0)
                    g.drawVerticalLine ((int) xOf (b * bar), lane.getY(), lane.getBottom());
            if (endX < w)
            {
                g.setColour (col::background.withAlpha (0.35f));
                g.fillRect (juce::Rectangle<float> ((float) juce::jmax ((double) headW, endX), lane.getY(), (float) w, lane.getHeight()));
            }
        }
        auto& tr = session.tracks[(size_t) ti];
        const bool silent = tr.mute || (soloed && ! tr.solo);
        for (auto& c : tr.clips)
            paintClip (g, ti, c, trackColour (tr.name, ti), silent);
        paintHead (g, ti, tr);
    }
    paintZone (g);
    paintRuler (g, beat, bar, every, first, last);
    const double x = xOf (playhead);
    if (x >= headW)
    {
        g.setColour (col::text);
        g.drawLine ((float) x, (float) rulerH - 14, (float) x, (float) h, 1.5f);
    }
    if (drag && drag->guide)
    {
        const float gx = (float) xOf (*drag->guide);
        const float dashes[] = { 4.0f, 3.0f };
        g.setColour (col::accent);
        g.drawDashedLine ({ gx, (float) rulerH, gx, (float) h }, dashes, 2, 1.0f);
    }
}

void ArrangeView::paintZone (juce::Graphics& g)
{
    if (! zone)
        return;
    const double x0 = juce::jmax ((double) headW, xOf (zone->t0)), x1 = xOf (zone->t1);
    if (x1 <= headW)
        return;
    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (headW, rulerH, getWidth() - headW, getHeight() - rulerH);
    for (int ti = zone->lo; ti <= juce::jmin (zone->hi, (int) session.tracks.size() - 1); ++ti)
    {
        const int top = trackTop (ti);
        const auto colour = trackColour (session.tracks[(size_t) ti].name, ti);
        juce::Rectangle<float> area ((float) x0, (float) top + 3, (float) (x1 - x0), (float) rowH - 6);
        g.setColour (colour.withAlpha (0.28f));
        g.fillRect (area);
        g.setColour (darker (colour, 140));
        g.drawLine (area.getX(), area.getY(), area.getX(), area.getBottom(), 1.5f);
        g.drawLine (area.getRight(), area.getY(), area.getRight(), area.getBottom(), 1.5f);
    }
}

void ArrangeView::paintRuler (juce::Graphics& g, double beat, double bar, int every, int first, int last)
{
    const int w = getWidth();
    g.setColour (col::card);
    g.fillRoundedRectangle (0.0f, 0.0f, (float) w, (float) rulerH - 4, 2.0f);
    g.setColour (col::muted);
    g.setFont (uiFont (14.5f));
    g.drawText (aicore::ui::tr ("Bars"), 12, 4, headW - 24, 20, juce::Justification::centredLeft);
    g.setFont (uiFont (13.0f));
    g.drawText (juce::String (session.bpm, session.bpm == std::floor (session.bpm) ? 0 : 1) + " BPM", 12, 24, headW - 24, 16,
                juce::Justification::centredLeft);
    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (headW, 0, w - headW, rulerH);
    if (beat * pps >= 12)
    {
        g.setColour (col::outline);
        for (int i = first * 4; i <= last * 4; ++i)
            if (i % 4)
                g.drawVerticalLine ((int) xOf (i * beat), (float) rulerH - 13, (float) rulerH - 6);
    }
    g.setFont (uiFont (14.5f));
    for (int b = first; b <= last; ++b)
    {
        if (b % every)
            continue;
        const int x = (int) xOf (b * bar);
        g.setColour (col::outline);
        g.drawVerticalLine (x, 22.0f, (float) rulerH - 6);
        g.setColour (col::muted);
        g.drawText (juce::String (b + 1), x + 4, 4, 60, 18, juce::Justification::centredLeft);
    }
    const double endX = xOf (session.length());
    if (session.length() > 0 && headW <= endX && endX < w)
    {
        g.setColour (col::muted);
        g.fillRect ((float) endX, 0.0f, 1.0f, (float) rulerH - 4);
        g.setFont (uiFont (13.0f));
        g.drawText (aicore::ui::tr ("End"), (int) endX + 4, 22, 60, 16, juce::Justification::centredLeft);
    }
}

ArrangeView::HeadRects ArrangeView::headRects (int ti) const
{
    const float top = (float) trackTop (ti);
    const float y = top + rowH / 2.0f - 16.0f;
    const float x = 16.0f + 74.0f;
    HeadRects r;
    r.fx = { x, y, 40, 26 };
    r.m = { x + 44, y, 26, 26 };
    r.s = { x + 74, y, 26, 26 };
    r.vol = { x + 106, y, headW - 10 - (x + 106) - 8, 26 };
    r.meter = { r.vol.getX(), r.vol.getBottom() + 3, r.vol.getWidth(), 6 };
    return r;
}

void ArrangeView::paintHead (juce::Graphics& g, int ti, const Track& tr)
{
    const float top = (float) trackTop (ti);
    const auto colour = trackColour (tr.name, ti);
    juce::Rectangle<float> card (0.0f, top + 3, headW - 10.0f, rowH - 6.0f);
    const bool current = ti == currentTrack;
    g.setColour (col::card);
    g.fillRoundedRectangle (current ? card.reduced (1) : card, 2.0f);
    if (current)
    {
        g.setColour (colour);
        g.drawRoundedRectangle (card.reduced (1), 2.0f, 2.0f);
    }
    g.setColour (colour);
    g.fillRoundedRectangle (4.0f, card.getY() + 4, 4.0f, card.getHeight() - 8, 1.5f);
    auto r = headRects (ti);
    g.setFont (uiFont (15.5f, true));
    g.setColour (tr.mute ? col::muted : col::text);
    g.drawText (tr.name, juce::Rectangle<float> (16.0f, r.fx.getY(), r.fx.getX() - 20.0f, r.fx.getHeight()),
                juce::Justification::centredLeft, true);
    struct B { juce::Rectangle<float> rect; bool on; const char* text; juce::Colour onColour; };
    for (auto& b : { B { r.fx, false, "FX", col::hover }, B { r.m, tr.mute, "M", col::muteOn }, B { r.s, tr.solo, "S", col::hover } })
    {
        g.setColour (b.on ? b.onColour : col::control);
        g.fillRoundedRectangle (b.rect, 2.0f);
        if (! b.on)
        {
            g.setColour (col::edge);
            g.drawRoundedRectangle (b.rect, 2.0f, 1.0f);
        }
        g.setColour (b.on ? col::onText : col::selectedText);
        g.setFont (uiFont (13.0f));
        g.drawText (b.text, b.rect, juce::Justification::centred);
    }
    g.setColour (col::field);
    g.fillRoundedRectangle (r.vol, 2.0f);
    g.setColour (col::outline);
    g.drawRoundedRectangle (r.vol, 2.0f, 1.0f);
    const float frac = (float) juce::jlimit (0.0, 1.0, (tr.gainDb - gainLow) / (gainHigh - gainLow));
    g.setColour (col::hover);
    g.fillRect (r.vol.getX() + 1, r.vol.getY() + 1, (r.vol.getWidth() - 2) * frac, r.vol.getHeight() - 2);
    g.setColour (col::text);
    g.setFont (uiFont (13.0f));
    g.drawText (juce::String (tr.gainDb, 1) + " dB", r.vol, juce::Justification::centred);
    paintMeter (g, r.meter, ti < (int) levels.size() ? levels[(size_t) ti] : 0.0f);
}

void ArrangeView::paintMeter (juce::Graphics& g, juce::Rectangle<float> area, float peak)
{
    const float db = peak > 1e-6f ? 20.0f * std::log10 (peak) : -90.0f;
    const float frac = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
    const float seg = 3, gap = 1;
    const int count = juce::jmax (1, (int) ((area.getWidth() + gap) / (seg + gap)));
    const int lit = (int) std::ceil (frac * count);
    const float hh = (area.getHeight() - 2) / 2;
    for (int row = 0; row < 2; ++row)
        for (int i = 0; i < count; ++i)
        {
            const float f = (i + 1) / (float) count;
            juce::Colour c = col::grid;
            if (i < lit)
                c = (peak >= 1.0f && i == count - 1) ? col::error : f > 0.9f ? col::muteOn : col::level;
            g.setColour (c);
            g.fillRect (area.getX() + i * (seg + gap), area.getY() + row * (hh + 2), seg, hh);
        }
}

std::pair<double, double> ArrangeView::fadeXs (juce::Rectangle<float> r, const Clip& c) const
{
    return { r.getX() + c.fadeIn * pps + fadeBox / 2.0 + 1, r.getRight() - c.fadeOut * pps - fadeBox / 2.0 - 1 };
}

void ArrangeView::paintClip (juce::Graphics& g, int ti, const Clip& c, juce::Colour colour, bool silent)
{
    auto r = clipRect (ti, c);
    if (r.getRight() < headW || r.getX() > getWidth())
        return;
    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (headW, rulerH, getWidth() - headW, getHeight() - rulerH);
    const bool sel = selected.count (c.id) > 0;
    auto fill = mixc (colour, juce::Colour (0xff4c5568), 0.30f);
    if (sel)
        fill = lighter (fill, 110);
    const float alpha = silent ? 0.5f : 1.0f;
    auto middle = lighter (fill, 116).withAlpha (0.88f * alpha);
    juce::ColourGradient grad (fill.withAlpha (alpha), r.getX(), r.getY(), fill.withAlpha (alpha), r.getX(), r.getBottom(), false);
    grad.addColour (0.5, middle);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (r.reduced (0.5f), 2.0f);
    g.setColour (darker (fill, 165).withAlpha (alpha));
    g.drawRoundedRectangle (r.reduced (0.5f), 2.0f, 1.2f);

    // the wave: the peaks a light veil, the RMS body over them (Core's drawWaveColumn)
    auto box = r.withTrimmedTop ((float) nameH + 1).withTrimmedBottom (3);
    Peaks pk;
    {
        const juce::ScopedLock sl (peakLock);
        auto it = peaks.find (c.path);
        if (it != peaks.end() && it->second.ready)
            pk = it->second;
    }
    if (pk.ready && pk.seconds > 0 && ! pk.hi.empty())
    {
        const int x0 = (int) juce::jmax ((double) r.getX(), (double) headW), x1 = (int) juce::jmin ((double) r.getRight(), (double) getWidth());
        const int n = (int) pk.hi.size();
        const float gain = juce::Decibels::decibelsToGain ((float) c.gainDb);
        const float mid = box.getCentreY(), half = box.getHeight() / 2 - 2;
        juce::Path veil, body;
        std::vector<float> his, bodies;
        for (int px = x0; px <= x1; ++px)
        {
            const double t = c.offset + (px - r.getX()) / pps;
            const int a = juce::jlimit (0, n - 1, (int) (t / pk.seconds * n));
            const int b = juce::jmax (a + 1, juce::jlimit (1, n, (int) ((t + 1.0 / pps) / pk.seconds * n)));
            float hv = 0, rv = 0;
            for (int i = a; i < b && i < n; ++i)
            {
                hv = juce::jmax (hv, juce::jmax (pk.hi[(size_t) i], -pk.lo[(size_t) i]));
                rv = juce::jmax (rv, pk.rms[(size_t) i]);
            }
            hv = juce::jmin (1.0f, hv * gain) * half;
            his.push_back (hv);
            bodies.push_back (juce::jmin (hv, juce::jmin (1.0f, rv * 1.41f * gain) * half));
        }
        auto shape = [&] (const std::vector<float>& hs) {
            juce::Path p;
            p.startNewSubPath ((float) x0, mid - juce::jmax (0.5f, hs[0]));
            for (size_t i = 1; i < hs.size(); ++i)
                p.lineTo ((float) (x0 + (int) i), mid - juce::jmax (0.5f, hs[i]));
            for (size_t i = hs.size(); i-- > 0;)
                p.lineTo ((float) (x0 + (int) i), mid + juce::jmax (0.5f, hs[i]));
            p.closeSubPath();
            return p;
        };
        if (! his.empty())
        {
            g.setColour (juce::Colours::white.withAlpha (0.42f * (silent ? 0.45f : 1.0f)));
            g.fillPath (shape (his));
            g.setColour (juce::Colour (0xfff4f7fa).withAlpha (0.95f * (silent ? 0.45f : 1.0f)));
            g.fillPath (shape (bodies));
        }
    }
    g.setColour (col::text.withAlpha (0.7f));
    if (c.fadeIn > 0)
        g.drawLine (r.getX(), r.getBottom(), r.getX() + (float) (c.fadeIn * pps), r.getY(), 1.5f);
    if (c.fadeOut > 0)
        g.drawLine (r.getRight() - (float) (c.fadeOut * pps), r.getY(), r.getRight(), r.getBottom(), 1.5f);
    g.setColour (colour.withAlpha (silent ? 0.35f : 0.95f));
    g.fillRect (r.getX() + 2, r.getY(), r.getWidth() - 4, 2.0f);
    g.setColour (mixc (colour, juce::Colour (0xff2e3440), 0.45f).withAlpha (silent ? 0.30f : 0.55f));
    g.fillRect (r.getX() + 1, r.getY() + 2, r.getWidth() - 2, (float) nameH - 2);
    if ((sel || c.id == hoverClip) && r.getWidth() > 3 * fadeBox)
        for (double fx : { fadeXs (r, c).first, fadeXs (r, c).second })
        {
            juce::Rectangle<float> box2 ((float) fx - fadeBox / 2.0f, r.getY() + 3, (float) fadeBox, (float) fadeBox);
            g.setColour (col::field);
            g.fillRect (box2);
            g.setColour (col::text.withAlpha (0.8f));
            g.drawRect (box2, 1.0f);
        }
    g.setColour (juce::Colours::white);
    g.setFont (uiFont (13.5f, true));
    auto text = c.label() + (std::abs (c.gainDb) > 0.05 ? "  " + juce::String (c.gainDb >= 0 ? "+" : "") + juce::String (c.gainDb, 1) + " dB" : juce::String());
    g.drawText (text, r.withTrimmedLeft (4.0f + (sel ? fadeBox + 4.0f : 0.0f)).withTrimmedTop (2).withTrimmedRight (4).withHeight ((float) nameH - 2),
                juce::Justification::topLeft, true);
    if (sel)
    {
        g.setColour (darker (colour, 135));
        g.drawRoundedRectangle (r.reduced (1.0f), 2.0f, 2.0f);
    }
}

//==============================================================================
juce::String ArrangeView::hit (int ti, const Clip& c, juce::Point<float> p) const
{
    auto r = clipRect (ti, c);
    auto [fin, fout] = fadeXs (r, c);
    if (r.getWidth() > 3 * fadeBox && p.y <= r.getY() + 6 + fadeBox)
    {
        if (std::abs (p.x - fin) <= fadeBox)
            return "fade_in";
        if (std::abs (p.x - fout) <= fadeBox)
            return "fade_out";
    }
    const float edge = juce::jmin ((float) edgePx, r.getWidth() / 4);
    if (p.x - r.getX() <= edge)
        return "trim_l";
    if (r.getRight() - p.x <= edge)
        return "trim_r";
    return "move";
}

bool ArrangeView::snapOn (const juce::ModifierKeys& m) const
{
    return snapping && ! m.isAltDown() && ! (drag && drag->free);
}

double ArrangeView::snapTime (double s, const juce::ModifierKeys& m) const
{
    if (! snapOn (m))
        return s;
    return snap (s, snapPoints (session, {}, playhead), snapPx / pps, gridStep (session.bpm, pps));
}

void ArrangeView::startZone (int ti, juce::Point<float> p, const juce::ModifierKeys& m)
{
    const double at = snapTime (secondsAt (p.x), m);
    currentTrack = ti;
    zone.reset();
    playhead = at;
    if (onSeek)
        onSeek (at);
    Drag d;
    d.kind = "zone";
    d.x = p.x;
    d.track = ti;
    d.at = at;
    drag = d;
    setMouseCursor (juce::MouseCursor::IBeamCursor);
    if (onSelectionChanged)
        onSelectionChanged();
    repaint();
}

void ArrangeView::mouseMove (const juce::MouseEvent& e)
{
    const auto p = e.position;
    int ti = -1;
    auto* c = clipAt (p, ti);
    const int hov = c != nullptr ? c->id : 0;
    if (hov != hoverClip)
    {
        hoverClip = hov;
        repaint();
    }
    juce::String kind = c != nullptr ? hit (ti, *c, p) : juce::String();
    if (kind == "move" && p.y > clipRect (ti, *c).getY() + nameH && ! e.mods.isCtrlDown())
        kind = "zone";
    setMouseCursor (kind == "zone" ? juce::MouseCursor::IBeamCursor
                    : kind.startsWith ("trim") || kind.startsWith ("fade") ? juce::MouseCursor::LeftRightResizeCursor
                    : kind == "move" ? juce::MouseCursor::DraggingHandCursor
                                     : juce::MouseCursor::NormalCursor);
}

void ArrangeView::setVolume (int ti, float x)
{
    auto vol = headRects (ti).vol;
    const double frac = juce::jlimit (0.0, 1.0, (double) ((x - vol.getX()) / vol.getWidth()));
    session.tracks[(size_t) ti].gainDb = std::round ((gainLow + frac * (gainHigh - gainLow)) * 10.0) / 10.0;
    repaint();
}

void ArrangeView::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    const auto p = e.position;
    if (e.mods.isPopupMenu())
    {
        int ti = -1;
        auto* c = clipAt (p, ti);
        if (c != nullptr && selected.count (c->id) == 0)
        {
            selected = { c->id };
            if (onSelectionChanged)
                onSelectionChanged();
        }
        if (onContextMenu)
            onContextMenu (e, ti, c);
        return;
    }
    if (p.y < rulerH)
    {
        Drag d;
        d.kind = "seek";
        drag = d;
        playhead = snapTime (secondsAt (p.x), e.mods);
        if (onSeek)
            onSeek (playhead);
        repaint();
        return;
    }
    int ti = -1;
    auto* c = clipAt (p, ti);
    if (p.x < headW)
    {
        if (ti >= 0 && ti < (int) session.tracks.size())
        {
            currentTrack = ti;
            auto r = headRects (ti);
            if (r.fx.contains (p))
            {
                if (onFx)
                    onFx (ti);
            }
            else if (r.m.contains (p) || r.s.contains (p))
            {
                const Session before = session;
                auto& tr = session.tracks[(size_t) ti];
                if (r.m.contains (p))
                    tr.mute = ! tr.mute;
                else
                    tr.solo = ! tr.solo;
                if (onEdited)
                    onEdited (before);
            }
            else if (r.vol.contains (p))
            {
                Drag d;
                d.kind = "volume";
                d.track = ti;
                d.base = session;
                drag = d;
                setVolume (ti, p.x);
            }
            if (onSelectionChanged)
                onSelectionChanged();
            repaint();
        }
        return;
    }
    if (c == nullptr)
    {
        if (! e.mods.isShiftDown())
            selected.clear();
        if (ti >= 0 && ti < (int) session.tracks.size())
        {
            currentTrack = ti;
            startZone (ti, p, e.mods);
            return;
        }
        zone.reset();
        Drag d;
        d.kind = "seek";
        drag = d;
        playhead = snapTime (secondsAt (p.x), e.mods);
        if (onSeek)
            onSeek (playhead);
        if (onSelectionChanged)
            onSelectionChanged();
        repaint();
        return;
    }
    currentTrack = ti;
    if (e.mods.isShiftDown())
    {
        if (! selected.erase (c->id))
            selected.insert (c->id);
        if (onSelectionChanged)
            onSelectionChanged();
        repaint();
        return;
    }
    const auto kind = hit (ti, *c, p);
    if (kind == "move" && ! e.mods.isCtrlDown() && p.y > clipRect (ti, *c).getY() + nameH)
    {
        selected = { c->id };
        startZone (ti, p, e.mods);
        return;
    }
    zone.reset();
    if (selected.count (c->id) == 0)
        selected = { c->id };
    Drag d;
    d.kind = kind;
    d.x = p.x;
    d.grab = c->id;
    d.grabTrack = ti;
    d.ids = kind == "move" ? std::vector<int> (selected.begin(), selected.end()) : std::vector<int> { c->id };
    d.base = session;
    d.free = e.mods.isAltDown();
    drag = d;
    setMouseCursor (kind == "move" ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::LeftRightResizeCursor);
    if (onSelectionChanged)
        onSelectionChanged();
    repaint();
}

void ArrangeView::mouseDrag (const juce::MouseEvent& e)
{
    if (! drag)
        return;
    const auto p = e.position;
    auto& d = *drag;
    if (d.kind == "zone")
    {
        if (std::abs (p.x - d.x) < 4 && ! zone)
            return;
        const int n = (int) session.tracks.size();
        const int ti = juce::jlimit (0, n - 1, p.y >= rulerH ? trackAt (p.y) : 0);
        const double end = snapTime (juce::jmax (0.0, secondsAt (p.x)), e.mods);
        zone = Zone { juce::jmin (d.track, ti), juce::jmax (d.track, ti), juce::jmin (d.at, end), juce::jmax (d.at, end) };
        repaint();
        return;
    }
    if (d.kind == "seek")
    {
        playhead = snapTime (secondsAt (p.x), e.mods);
        if (onSeek)
            onSeek (playhead);
        repaint();
        return;
    }
    if (d.kind == "volume")
    {
        setVolume (d.track, p.x);
        return;
    }
    const double dt = (p.x - d.x) / pps;
    Session s = d.base;
    std::optional<double> guide;
    if (d.kind == "move")
    {
        auto* gc = s.find (d.grab);
        double newStart = gc->start + dt;
        if (snapOn (e.mods))
        {
            auto pts = snapPoints (s, d.ids, playhead);
            const double snapped = snapMove (newStart, gc->length, pts, snapPx / pps, gridStep (s.bpm, pps));
            if (snapped != newStart)
            {
                bool startHit = false, endHit = false;
                for (double q : pts)
                {
                    startHit = startHit || std::abs (snapped - q) < 1e-9;
                    endHit = endHit || std::abs (snapped + gc->length - q) < 1e-9;
                }
                guide = endHit && ! startHit ? snapped + gc->length : snapped;
            }
            newStart = snapped;
        }
        double shift = newStart - gc->start;
        const int target = trackAt (p.y);
        int dtrack = target < 0 ? 0 : juce::jmin (target, (int) s.tracks.size() - 1) - d.grabTrack;
        std::vector<int> tracksOf;
        double minStart = 1e18;
        for (int id : d.ids)
        {
            int t = 0;
            auto* c = s.find (id, &t);
            tracksOf.push_back (t);
            minStart = juce::jmin (minStart, c->start);
        }
        const int lo = *std::min_element (tracksOf.begin(), tracksOf.end()), hi = *std::max_element (tracksOf.begin(), tracksOf.end());
        dtrack = juce::jlimit (-lo, (int) s.tracks.size() - 1 - hi, dtrack);
        shift = juce::jmax (shift, -minStart);
        std::vector<double> starts;
        for (int id : d.ids)
            starts.push_back (s.find (id)->start);
        for (size_t i = 0; i < d.ids.size(); ++i)
            move (s, d.ids[i], starts[i] + shift, tracksOf[i] + dtrack);
    }
    else if (d.kind == "trim_l" || d.kind == "trim_r")
    {
        auto* c = s.find (d.grab);
        double edge = (d.kind == "trim_l" ? c->start : c->end()) + dt;
        if (snapOn (e.mods))
        {
            edge = snap (edge, snapPoints (s, { c->id }, playhead), snapPx / pps, gridStep (s.bpm, pps));
            guide = edge;
        }
        if (d.kind == "trim_l")
            trimStart (s, c->id, edge);
        else
            trimEnd (s, c->id, edge);
    }
    else if (d.kind == "fade_in" || d.kind == "fade_out")
    {
        auto* c = s.find (d.grab);
        if (d.kind == "fade_in")
            c->fadeIn = juce::jmax (0.0, juce::jmin (c->length - c->fadeOut, c->fadeIn + dt));
        else
            c->fadeOut = juce::jmax (0.0, juce::jmin (c->length - c->fadeIn, c->fadeOut - dt));
    }
    d.guide = guide;
    d.moved = true;
    session = s;
    repaint();
}

void ArrangeView::mouseUp (const juce::MouseEvent&)
{
    if (! drag)
        return;
    auto d = *drag;
    drag.reset();
    if (d.kind == "zone")
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
        if (zone && zone->t1 - zone->t0 < 1e-3)
            zone.reset();
        if (zone)
        {
            playhead = zone->t0;
            if (onSeek)
                onSeek (playhead);
        }
        if (onSelectionChanged)
            onSelectionChanged();
        repaint();
        return;
    }
    if (d.kind == "volume" || (d.kind != "seek" && d.moved))
        if (onEdited)
            onEdited (d.base);
    setMouseCursor (juce::MouseCursor::NormalCursor);
    if (onSelectionChanged)
        onSelectionChanged();
    repaint();
}

void ArrangeView::renameTrack (int ti)
{
    auto* w = new juce::AlertWindow ("Rename track", "Name", juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor ("name", session.tracks[(size_t) ti].name);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([this, ti, w] (int r) {
        const auto name = w->getTextEditorContents ("name").trim();
        if (r == 1 && name.isNotEmpty() && ti < (int) session.tracks.size())
        {
            const Session before = session;
            session.tracks[(size_t) ti].name = name;
            if (onEdited)
                onEdited (before);
            repaint();
        }
    }), true);
}

void ArrangeView::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto p = e.position;
    int ti = -1;
    auto* c = clipAt (p, ti);
    if (c != nullptr)
        return; // Open in SAMPLE: the SAMPLE page is not built yet
    if (p.x < headW && ti >= 0 && ti < (int) session.tracks.size())
    {
        auto r = headRects (ti);
        if (r.vol.contains (p))
        {
            const Session before = session;
            session.tracks[(size_t) ti].gainDb = 0;
            if (onEdited)
                onEdited (before);
        }
        else if (! (r.m.contains (p) || r.s.contains (p)))
            renameTrack (ti);
    }
}

void ArrangeView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const float dy = w.deltaY != 0 ? w.deltaY : w.deltaX;
    if (e.mods.isCtrlDown())
        zoom (dy > 0 ? 1.25 : 0.8, e.position.x);
    else if (e.mods.isShiftDown() || contentHeight() <= getHeight())
        setView (scrollX - dy * 4.0 * visibleSeconds() * 0.1);
    else
    {
        scrollY = juce::jlimit (0, juce::jmax (0, contentHeight() - getHeight()), scrollY - (int) (dy * 120));
        repaint();
    }
}

bool ArrangeView::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (juce::File (f).hasFileExtension ("wav;flac;mp3;ogg;aif;aiff"))
            return true;
    return false;
}

void ArrangeView::filesDropped (const juce::StringArray& files, int x, int y)
{
    juce::StringArray audio;
    for (auto& f : files)
        if (juce::File (f).hasFileExtension ("wav;flac;mp3;ogg;aif;aiff"))
            audio.add (f);
    if (audio.isEmpty() || ! onFilesDropped)
        return;
    const int ti = trackAt ((float) y);
    const double at = x < headW ? 0.0 : snapTime (secondsAt (x), {});
    onFilesDropped (audio, juce::jmax (0, ti), at);
}
} // namespace aicore::tracks
