#include "WaveView.h"
#include "../ui/I18n.h"

#include <cmath>

namespace aicore::sample
{
using namespace aicore::ui;

namespace
{
juce::String rulerLabel (double sec, double step)
{
    sec = std::round (sec * 1000) / 1000;
    const int m = (int) (sec / 60);
    const double rest = sec - m * 60;
    if (step >= 1)
        return juce::String (m) + ":" + juce::String ((int) std::round (rest)).paddedLeft ('0', 2);
    const int digits = step >= 0.1 ? 1 : step >= 0.01 ? 2 : 3;
    return juce::String (m) + ":" + juce::String (rest, digits).paddedLeft ('0', 3 + digits);
}
} // namespace

WaveView::WaveView()
{
    setWantsKeyboardFocus (true);
    setMouseCursor (juce::MouseCursor::IBeamCursor);
}

void WaveView::computeStats()
{
    for (int ch = 0; ch < 2; ++ch)
    {
        lows[ch].clear();
        highs[ch].clear();
        sums[ch].clear();
    }
    if (audio_ == nullptr)
        return;
    const int count = audio_->getNumSamples() / block;
    for (int ch = 0; ch < 2; ++ch)
    {
        lows[ch].resize ((size_t) count);
        highs[ch].resize ((size_t) count);
        sums[ch].resize ((size_t) count);
        const float* d = audio_->getReadPointer (juce::jmin (ch, audio_->getNumChannels() - 1));
        for (int b = 0; b < count; ++b)
        {
            float lo = d[b * block], hi = lo;
            double s = 0;
            for (int i = b * block; i < (b + 1) * block; ++i)
            {
                lo = juce::jmin (lo, d[i]);
                hi = juce::jmax (hi, d[i]);
                s += (double) d[i] * d[i];
            }
            lows[ch][(size_t) b] = lo;
            highs[ch][(size_t) b] = hi;
            sums[ch][(size_t) b] = s;
        }
    }
}

void WaveView::setAudio (std::shared_ptr<const Audio> a, double rate, bool keepView)
{
    audio_ = std::move (a);
    rate_ = rate;
    computeStats();
    twoLanes = false;
    if (audio_ != nullptr && audio_->getNumChannels() > 1)
    {
        const int n = audio_->getNumSamples(), step = juce::jmax (1, n / 20000);
        for (int i = 0; i < n && ! twoLanes; i += step)
            twoLanes = audio_->getSample (0, i) != audio_->getSample (1, i);
    }
    ++serial;
    const int n = frames();
    if (! keepView || viewStart + viewSpan > n)
    {
        viewStart = 0;
        viewSpan = juce::jmax (1, n);
    }
    sel = keepView && sel.second > sel.first ? std::pair<int, int> { juce::jmin (sel.first, n), juce::jmin (sel.second, n) } : std::pair<int, int> { 0, 0 };
    repaint();
    if (onSelectionChanged) onSelectionChanged();
    if (onViewChanged) onViewChanged();
}

std::pair<int, int> WaveView::selection() const
{
    return hasSelection() ? sel : std::pair<int, int> { 0, frames() };
}

void WaveView::setSelection (int a, int b)
{
    const int n = frames();
    a = juce::jlimit (0, n, a);
    b = juce::jlimit (0, n, b);
    sel = { juce::jmin (a, b), juce::jmax (a, b) };
    repaint();
    if (onSelectionChanged) onSelectionChanged();
}

std::pair<int, int> WaveView::fadeFrames() const
{
    auto [a, b] = selection();
    return fadeLengths (b - a, (int) std::round (fadeIn * rate_), (int) std::round (fadeOut * rate_));
}

void WaveView::setFades (std::optional<double> in, std::optional<double> out, std::optional<juce::String> c)
{
    if (in) fadeIn = juce::jmax (0.0, *in);
    if (out) fadeOut = juce::jmax (0.0, *out);
    if (c) curve = *c;
    repaint();
    if (onFadesChanged) onFadesChanged();
}

Audio WaveView::rendered (int& first) const
{
    auto [a, b] = selection();
    auto [fi, fo] = fadeFrames();
    first = a;
    return applyFades (range (*audio_, a, b), fi, fo, curve);
}

std::pair<float, float> WaveView::handles() const
{
    auto [a, b] = selection();
    auto [fi, fo] = fadeFrames();
    return { xOf (a + fi), xOf (b - fo) };
}

juce::String WaveView::nearHandle (juce::Point<float> p) const
{
    if (audio_ == nullptr || p.y > ruler + handle + 8)
        return {};
    auto [hin, hout] = handles();
    const float din = std::abs (p.x - hin), dout = std::abs (p.x - hout);
    if (juce::jmin (din, dout) > handle + 2)
        return {};
    return din <= dout ? "in" : "out";
}

void WaveView::setView (double start, double span)
{
    const int n = frames();
    span = juce::jmax ((double) juce::jmin (64, n), juce::jmin ((double) n, span));
    start = juce::jmax (0.0, juce::jmin (start, n - span));
    viewStart = start;
    viewSpan = juce::jmax (1.0, span);
    repaint();
    if (onViewChanged) onViewChanged();
}

void WaveView::zoom (double factor, std::optional<float> atX)
{
    if (audio_ == nullptr)
        return;
    double at;
    if (! atX)
        at = viewStart <= playhead && playhead <= viewStart + viewSpan ? playhead : viewStart + viewSpan / 2;
    else
        at = frameOf (*atX);
    const double span = juce::jmax (64.0, juce::jmin ((double) frames(), viewSpan * factor));
    setView (at - (at - viewStart) * span / viewSpan, span);
}

void WaveView::zoomToSelection()
{
    auto [a, b] = selection();
    const double pad = (b - a) * 0.05;
    setView (a - pad, b - a + 2 * pad);
}

void WaveView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.fillAll (col::card);
    g.setColour (col::panel);
    g.fillRect (0.0f, 0.0f, r.getWidth(), (float) ruler);
    auto wave = juce::Rectangle<float> (0, (float) ruler, r.getWidth(), r.getHeight() - ruler);
    g.setColour (juce::Colour (0xffeef1f6));
    g.fillRect (wave);
    if (audio_ == nullptr || frames() == 0)
    {
        g.setColour (col::muted);
        g.setFont (uiFont (15.0f));
        g.drawText (aicore::ui::tr ("Open a song (the folder button above, or drop a file here)."), wave, juce::Justification::centred);
        g.setColour (col::outline);
        g.drawRect (r, 1.0f);
        return;
    }
    auto [a, b] = sel;
    if (b > a)
    {
        g.setColour (col::accent.withAlpha (0.16f));
        g.fillRect (juce::Rectangle<float> (xOf (a), (float) ruler, xOf (b) - xOf (a), wave.getHeight()));
        g.setColour (col::accent.withAlpha (0.30f));
        g.fillRect (juce::Rectangle<float> (xOf (a), 0.0f, xOf (b) - xOf (a), (float) ruler));
    }
    paintRuler (g);
    if (twoLanes)
    {
        const float h = wave.getHeight() / 2;
        for (int ch = 0; ch < 2; ++ch)
            paintLane (g, ch, { 0.0f, ruler + ch * h + 3, wave.getWidth(), h - 6 });
        g.setColour (col::outline);
        g.drawHorizontalLine ((int) (ruler + h), 0.0f, wave.getWidth());
    }
    else
        paintLane (g, 0, wave.reduced (0, 4));
    paintFades (g, wave);
    if (b > a)
    {
        g.setColour (col::accent);
        for (int f : { a, b })
            g.drawLine (std::round (xOf (f)) + 0.5f, (float) ruler, std::round (xOf (f)) + 0.5f, r.getHeight(), 1.5f);
    }
    if (playhead >= 0)
    {
        const float x = std::round (xOf (playhead)) + 0.5f;
        g.setColour (col::error);
        g.drawLine (x, (float) ruler, x, r.getHeight(), 1.5f);
        juce::Path tri;
        tri.addTriangle (x - 5, (float) ruler - 8, x + 5, (float) ruler - 8, x, (float) ruler);
        g.fillPath (tri);
    }
    g.setColour (col::outline);
    g.drawRect (r, 1.0f);
}

void WaveView::paintRuler (juce::Graphics& g)
{
    const double seconds = viewSpan / rate_;
    const int width = juce::jmax (1, getWidth());
    const double steps[] = { 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300 };
    double step = 600;
    for (double s : steps)
        if (width / (seconds / s) >= 80)
        {
            step = s;
            break;
        }
    g.setFont (uiFont (10.0f));
    g.setColour (col::outline);
    g.drawHorizontalLine (ruler - 1, 0.0f, (float) width);
    const int first = (int) (viewStart / rate_ / step);
    for (int i = first; i < first + (int) (seconds / step) + 2; ++i)
    {
        const double sec = i * step;
        const float x = std::round (xOf (sec * rate_)) + 0.5f;
        if (x > width)
            break;
        g.setColour (col::outline);
        g.drawLine (x, (float) ruler - 8, x, (float) ruler, 1.0f);
        for (int k = 1; k < 4; ++k)
        {
            const float xm = std::round (xOf ((sec + step * k / 4) * rate_)) + 0.5f;
            g.drawLine (xm, (float) ruler - 4, xm, (float) ruler, 1.0f);
        }
        if (x >= 0)
        {
            g.setColour (col::muted);
            g.drawText (rulerLabel (sec, step), (int) x + 4, 0, 80, ruler - 9, juce::Justification::bottomLeft, false);
        }
    }
}

void WaveView::paintLane (juce::Graphics& g, int ch, juce::Rectangle<float> box)
{
    const int cols = juce::jmax (1, (int) box.getWidth());
    const float mid = box.getCentreY(), half = box.getHeight() / 2;
    const double perCol = viewSpan / cols;
    g.setColour (juce::Colour::fromRGBA (31, 38, 51, 22));
    for (float frac : { 0.25f, 0.75f })
    {
        const float y = box.getY() + box.getHeight() * frac;
        for (float x = box.getX(); x < box.getRight(); x += 4)
            g.drawLine (x, y, juce::jmin (x + 1.0f, box.getRight()), y, 1.0f);
    }
    g.setColour (col::outline);
    g.drawLine (box.getX(), mid, box.getRight(), mid, 1.0f);
    const auto& a = *audio_;
    const float* d = a.getReadPointer (juce::jmin (ch, a.getNumChannels() - 1));
    if (perCol < 4)
    {
        const int s = (int) juce::jmax (0.0, viewStart), e = (int) juce::jmin ((double) a.getNumSamples(), viewStart + viewSpan + 2);
        juce::Path path;
        for (int k = s; k < e; ++k)
        {
            juce::Point<float> pt (box.getX() + (float) ((k - viewStart) / perCol), mid - d[k] * half);
            k == s ? path.startNewSubPath (pt) : path.lineTo (pt);
        }
        g.setColour (juce::Colour (0xff2f4f86));
        g.strokePath (path, juce::PathStrokeType (1.4f));
        if (perCol < 0.25)
            for (int k = s; k < e; ++k)
                g.fillEllipse (box.getX() + (float) ((k - viewStart) / perCol) - 2.2f, mid - d[k] * half - 2.2f, 4.4f, 4.4f);
        return;
    }
    std::vector<float> lo ((size_t) cols), hi ((size_t) cols), rms ((size_t) cols);
    const bool useBlocks = perCol >= block * 2 && ! lows[ch].empty();
    for (int c = 0; c < cols; ++c)
    {
        const double e0 = viewStart + c * perCol, e1 = viewStart + (c + 1) * perCol;
        if (useBlocks)
        {
            const int nb = (int) lows[ch].size();
            const int i0 = juce::jlimit (0, nb - 1, (int) (e0 / block)), i1 = juce::jlimit (i0 + 1, nb, (int) (e1 / block));
            float l = lows[ch][(size_t) i0], h = highs[ch][(size_t) i0];
            double s = 0;
            for (int i = i0; i < i1; ++i)
            {
                l = juce::jmin (l, lows[ch][(size_t) i]);
                h = juce::jmax (h, highs[ch][(size_t) i]);
                s += sums[ch][(size_t) i];
            }
            lo[(size_t) c] = l;
            hi[(size_t) c] = h;
            rms[(size_t) c] = (float) std::sqrt (s / ((i1 - i0) * block));
        }
        else
        {
            const int n = a.getNumSamples();
            const int i0 = juce::jlimit (0, n - 1, (int) e0), i1 = juce::jlimit (i0 + 1, n, (int) e1);
            float l = d[i0], h = d[i0];
            double s = 0;
            for (int i = i0; i < i1; ++i)
            {
                l = juce::jmin (l, d[i]);
                h = juce::jmax (h, d[i]);
                s += (double) d[i] * d[i];
            }
            lo[(size_t) c] = l;
            hi[(size_t) c] = h;
            rms[(size_t) c] = (float) std::sqrt (s / (i1 - i0));
        }
    }
    auto shape = [&] (auto upper, auto lower) {
        juce::Path p;
        p.startNewSubPath (box.getX(), upper (0));
        for (int i = 1; i < cols; ++i)
            p.lineTo (box.getX() + i, upper (i));
        for (int i = cols - 1; i >= 0; --i)
            p.lineTo (box.getX() + i, lower (i));
        p.closeSubPath();
        return p;
    };
    auto outline = shape ([&] (int i) { return mid - juce::jmax (0.5f, hi[(size_t) i] * half); },
                          [&] (int i) { return mid - juce::jmin (-0.5f, lo[(size_t) i] * half); });
    const juce::Colour peak (0xff7aa3e6), body (0xff3d6fc4);
    g.setColour (peak);
    g.fillPath (outline);
    g.strokePath (outline, juce::PathStrokeType (1.0f));
    if (perCol >= 32)
    {
        g.setColour (body);
        g.fillPath (shape ([&] (int i) { return mid - rms[(size_t) i] * half; }, [&] (int i) { return mid + rms[(size_t) i] * half; }));
    }
}

void WaveView::paintFades (juce::Graphics& g, juce::Rectangle<float> box)
{
    auto [a, b] = selection();
    auto [fi, fo] = fadeFrames();
    const float top = box.getY(), bottom = box.getBottom();
    for (auto [start, n, rising] : { std::tuple<int, int, bool> { a, fi, true }, { b - fo, fo, false } })
    {
        if (n <= 0)
            continue;
        const float x0 = xOf (start), x1 = xOf (start + n);
        if (x1 < 0 || x0 > box.getWidth())
            continue;
        const int steps = juce::jmax (2, juce::jmin (200, (int) (std::abs (x1 - x0) / 2)));
        auto gains = fadeCurve (steps, curve);
        if (! rising)
            std::reverse (gains.begin(), gains.end());
        juce::Path line, shade;
        shade.startNewSubPath (x0, top);
        for (int i = 0; i < steps; ++i)
        {
            juce::Point<float> pt (x0 + (x1 - x0) * i / (steps - 1), bottom - gains[(size_t) i] * (bottom - top));
            i == 0 ? line.startNewSubPath (pt) : line.lineTo (pt);
            shade.lineTo (pt);
        }
        shade.lineTo (x1, top);
        shade.closeSubPath();
        g.setColour (juce::Colour::fromRGBA (20, 28, 45, 40));
        g.fillPath (shade);
        g.setColour (col::accent);
        g.strokePath (line, juce::PathStrokeType (1.6f));
    }
    auto [hin, hout] = handles();
    for (float x : { hin, hout })
    {
        juce::Rectangle<float> h (x - handle / 2.0f, top, (float) handle, (float) handle);
        g.setColour (col::card);
        g.fillRect (h);
        g.setColour (col::accent);
        g.drawRect (h, 1.2f);
    }
}

void WaveView::mouseDown (const juce::MouseEvent& e)
{
    if (audio_ == nullptr || ! e.mods.isLeftButtonDown())
        return;
    grabKeyboardFocus();
    const float x = e.position.x;
    auto [a, b] = sel;
    dragHandle = nearHandle (e.position);
    if (dragHandle.isNotEmpty())
        drag = Drag::fade;
    else if (b > a && std::abs (x - xOf (a)) < 6)
    {
        drag = Drag::edge;
        anchor = b;
    }
    else if (b > a && std::abs (x - xOf (b)) < 6)
    {
        drag = Drag::edge;
        anchor = a;
    }
    else
    {
        drag = Drag::fresh;
        anchor = frameOf (x);
        sel = { anchor, anchor };
    }
    repaint();
}

void WaveView::mouseMove (const juce::MouseEvent& e)
{
    const float x = e.position.x;
    auto [a, b] = sel;
    const bool nearEdge = b > a && (std::abs (x - xOf (a)) < 6 || std::abs (x - xOf (b)) < 6);
    setMouseCursor (nearEdge || nearHandle (e.position).isNotEmpty() ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::IBeamCursor);
}

void WaveView::mouseDrag (const juce::MouseEvent& e)
{
    const float x = e.position.x;
    if (drag == Drag::fade)
    {
        auto [lo, hi] = selection();
        const int f = juce::jlimit (lo, hi, frameOf (x));
        if (dragHandle == "in")
            setFades ((f - lo) / rate_, {});
        else
            setFades ({}, (hi - f) / rate_);
        return;
    }
    if (drag == Drag::none)
        return;
    const int f = juce::jlimit (0, frames(), frameOf (x));
    sel = { juce::jmin (anchor, f), juce::jmax (anchor, f) };
    repaint();
}

void WaveView::mouseUp (const juce::MouseEvent&)
{
    if (drag == Drag::none)
        return;
    if (drag == Drag::fade)
    {
        drag = Drag::none;
        if (onFadesChanged) onFadesChanged();
        return;
    }
    drag = Drag::none;
    auto [a, b] = sel;
    if (b - a < juce::jmax (2.0, viewSpan / juce::jmax (1, getWidth()) * 3)) // a click: the playhead moves there
    {
        playhead = a;
        sel = { 0, 0 };
    }
    else
        sel = { snapped (a), snapped (b) };
    repaint();
    if (onSelectionChanged) onSelectionChanged();
}

void WaveView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (audio_ == nullptr)
        return;
    const double steps = (w.deltaY != 0 ? w.deltaY : w.deltaX) / 0.125 * 0.5; // JUCE: 1 notch ~ 0.125..0.25
    const double notches = steps > 0 ? juce::jmax (1.0, std::round (steps)) : juce::jmin (-1.0, std::round (steps));
    if (e.mods.isCtrlDown())
        zoom (std::pow (0.75, notches), e.position.x);
    else
        setView (viewStart - notches * viewSpan * 0.12, viewSpan);
}

//==============================================================================
Overview::Overview (WaveView& w) : wave (w)
{
    setRepaintsOnMouseActivity (false);
}

void Overview::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (col::panel);
    g.fillRect (r);
    if (wave.frames() > 0)
    {
        const int cols = juce::jmax (1, getWidth());
        if (key != std::pair<int, int> { wave.serial, cols })
        {
            key = { wave.serial, cols };
            cache.assign ((size_t) cols, 0.0f);
            const int nb = (int) wave.lows[0].size();
            if (nb > 0)
                for (int c = 0; c < cols; ++c)
                {
                    const int i0 = juce::jlimit (0, nb - 1, (int) ((double) c / cols * nb));
                    const int i1 = juce::jlimit (i0 + 1, nb, (int) ((double) (c + 1) / cols * nb));
                    float v = 0;
                    for (int i = i0; i < i1; ++i)
                        for (int ch = 0; ch < 2; ++ch)
                            v = juce::jmax (v, -wave.lows[ch][(size_t) i], wave.highs[ch][(size_t) i]);
                    cache[(size_t) c] = v;
                }
        }
        const float mid = r.getCentreY(), half = r.getHeight() / 2 - 3;
        g.setColour (juce::Colour (0xff8fa3c4));
        for (int x = 0; x < (int) cache.size(); ++x)
            g.drawLine (x + 0.5f, mid - cache[(size_t) x] * half, x + 0.5f, mid + juce::jmax (cache[(size_t) x] * half, 0.5f), 1.0f);
        auto [a, b] = wave.sel;
        if (b > a)
        {
            g.setColour (col::accent.withAlpha (0.18f));
            g.fillRect (juce::Rectangle<float> (xOf (a), r.getY(), xOf (b) - xOf (a), r.getHeight()));
        }
        juce::Rectangle<float> box (xOf (wave.viewStart), r.getY() + 1, juce::jmax (4.0f, xOf (wave.viewSpan)), r.getHeight() - 2);
        g.setColour (col::accent.withAlpha (0.10f));
        g.fillRoundedRectangle (box, 2.0f);
        g.setColour (col::accent);
        g.drawRoundedRectangle (box, 2.0f, 1.5f);
        if (wave.playhead >= 0)
        {
            g.setColour (col::error);
            g.drawLine (xOf (wave.playhead), r.getY(), xOf (wave.playhead), r.getBottom(), 1.0f);
        }
    }
    g.setColour (col::outline);
    g.drawRoundedRectangle (r, 2.0f, 1.0f);
}

juce::String Overview::edge (float x) const
{
    if (std::abs (x - xOf (wave.viewStart)) < 5) return "left";
    if (std::abs (x - xOf (wave.viewStart + wave.viewSpan)) < 5) return "right";
    return {};
}

void Overview::mouseDown (const juce::MouseEvent& e)
{
    if (wave.frames() == 0)
        return;
    const float x = e.position.x;
    drag = edge (x);
    if (drag.isNotEmpty())
        return;
    if (xOf (wave.viewStart) <= x && x <= xOf (wave.viewStart + wave.viewSpan))
        offset = frameOf (x) - wave.viewStart;
    else
    {
        wave.setView (frameOf (x) - wave.viewSpan / 2, wave.viewSpan);
        offset = wave.viewSpan / 2;
    }
    drag = "move";
}

void Overview::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (edge (e.position.x).isNotEmpty() ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::PointingHandCursor);
}

void Overview::mouseDrag (const juce::MouseEvent& e)
{
    const double f = frameOf (e.position.x), start = wave.viewStart, span = wave.viewSpan;
    if (drag == "move")
        wave.setView (f - offset, span);
    else if (drag == "left")
    {
        const double end = start + span, s = juce::jmin (f, end - 64);
        wave.setView (s, end - s);
    }
    else if (drag == "right")
        wave.setView (start, juce::jmax (64.0, f - start));
}
} // namespace aicore::sample
