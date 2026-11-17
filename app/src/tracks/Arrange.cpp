#include "Arrange.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace aicore::tracks
{
int newId()
{
    static int next = 1;
    return next++;
}

double Session::length() const
{
    double l = 0;
    for (auto& t : tracks)
        for (auto& c : t.clips)
            l = std::max (l, c.end());
    return l;
}

Clip* Session::find (int clipId, int* trackIndex)
{
    for (size_t t = 0; t < tracks.size(); ++t)
        for (auto& c : tracks[t].clips)
            if (c.id == clipId)
            {
                if (trackIndex != nullptr)
                    *trackIndex = (int) t;
                return &c;
            }
    return nullptr;
}

Track& addTrack (Session& s, const juce::String& name)
{
    Track t;
    t.name = name.isNotEmpty() ? name : juce::String ((int) s.tracks.size() + 1);
    t.id = newId();
    s.tracks.push_back (t);
    return s.tracks.back();
}

Clip* addClip (Session& s, int track, const juce::File& file, double start)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (file));
    if (r == nullptr || r->sampleRate <= 0)
        return nullptr;
    const double seconds = (double) r->lengthInSamples / r->sampleRate;
    while ((int) s.tracks.size() <= track)
        addTrack (s, (int) s.tracks.size() == track ? file.getFileNameWithoutExtension() : juce::String());
    if (s.rate == 0)
        s.rate = (int) r->sampleRate;
    Clip c;
    c.path = file.getFullPathName();
    c.start = std::max (0.0, start);
    c.length = c.sourceLength = seconds;
    c.id = newId();
    s.tracks[(size_t) track].clips.push_back (c);
    return &s.tracks[(size_t) track].clips.back();
}

Clip* split (Session& s, int clipId, double at)
{
    int ti = -1;
    auto* c = s.find (clipId, &ti);
    if (c == nullptr || ! (c->start + kMinClip <= at && at <= c->end() - kMinClip))
        return nullptr;
    const double left = at - c->start;
    Clip right = *c;
    right.id = newId();
    right.start = at;
    right.offset = c->offset + left;
    right.length = c->length - left;
    right.fadeIn = 0;
    right.fadeOut = std::min (right.fadeOut, right.length);
    c->length = left;
    c->fadeOut = 0;
    c->fadeIn = std::min (c->fadeIn, c->length);
    auto& clips = s.tracks[(size_t) ti].clips;
    auto it = std::find_if (clips.begin(), clips.end(), [&] (const Clip& x) { return x.id == clipId; });
    const int newIdValue = right.id;
    clips.insert (it + 1, right);
    return s.find (newIdValue);
}

std::vector<int> splitAt (Session& s, double at, const std::vector<int>& clipIds, int track)
{
    std::vector<int> ids = clipIds;
    if (ids.empty())
        for (size_t t = 0; t < s.tracks.size(); ++t)
            if (track < 0 || (int) t == track)
                for (auto& c : s.tracks[t].clips)
                    if (c.start < at && at < c.end())
                        ids.push_back (c.id);
    std::vector<int> made;
    for (int id : ids)
        if (auto* r = split (s, id, at))
            made.push_back (r->id);
    return made;
}

void trimStart (Session& s, int clipId, double newStart)
{
    auto* c = s.find (clipId);
    if (c == nullptr)
        return;
    newStart = std::max ({ c->start - c->offset, 0.0, std::min (newStart, c->end() - kMinClip) });
    const double d = newStart - c->start;
    c->start = newStart;
    c->offset += d;
    c->length -= d;
    c->fadeIn = std::min (c->fadeIn, c->length);
    c->fadeOut = std::min (c->fadeOut, c->length);
}

void trimEnd (Session& s, int clipId, double newEnd)
{
    auto* c = s.find (clipId);
    if (c == nullptr)
        return;
    const double limit = c->start + (c->sourceLength > 0 ? c->sourceLength - c->offset : c->length);
    c->length = std::max (kMinClip, std::min (newEnd, limit) - c->start);
    c->fadeIn = std::min (c->fadeIn, c->length);
    c->fadeOut = std::min (c->fadeOut, c->length);
}

void move (Session& s, int clipId, double start, int track)
{
    int ti = -1;
    auto* c = s.find (clipId, &ti);
    if (c == nullptr)
        return;
    c->start = std::max (0.0, start);
    if (track >= 0 && track < (int) s.tracks.size() && track != ti)
    {
        Clip copy = *c;
        auto& from = s.tracks[(size_t) ti].clips;
        from.erase (std::remove_if (from.begin(), from.end(), [&] (const Clip& x) { return x.id == clipId; }), from.end());
        s.tracks[(size_t) track].clips.push_back (copy);
    }
}

void remove (Session& s, const std::vector<int>& ids)
{
    std::set<int> set (ids.begin(), ids.end());
    for (auto& t : s.tracks)
        t.clips.erase (std::remove_if (t.clips.begin(), t.clips.end(), [&] (const Clip& c) { return set.count (c.id) > 0; }),
                       t.clips.end());
}

std::vector<int> duplicate (Session& s, const std::vector<int>& ids)
{
    std::vector<std::pair<int, Clip>> found;
    for (int id : ids)
    {
        int ti = -1;
        if (auto* c = s.find (id, &ti))
            found.push_back ({ ti, *c });
    }
    if (found.empty())
        return {};
    double lo = 1e18, hi = 0;
    for (auto& [t, c] : found)
    {
        lo = std::min (lo, c.start);
        hi = std::max (hi, c.end());
    }
    std::vector<int> made;
    for (auto& [t, c] : found)
    {
        Clip n = c;
        n.id = newId();
        n.start = c.start + (hi - lo);
        s.tracks[(size_t) t].clips.push_back (n);
        made.push_back (n.id);
    }
    return made;
}

double gridStep (double bpm, double pps)
{
    double step = 60.0 / std::max (1.0, bpm);
    while (step * pps < 24.0)
        step *= 2.0;
    return step;
}

std::vector<double> snapPoints (const Session& s, const std::vector<int>& exclude, double playhead)
{
    std::set<double> pts { 0.0, playhead };
    for (auto& t : s.tracks)
        for (auto& c : t.clips)
            if (std::find (exclude.begin(), exclude.end(), c.id) == exclude.end())
            {
                pts.insert (c.start);
                pts.insert (c.end());
            }
    return { pts.begin(), pts.end() };
}

double snap (double value, const std::vector<double>& points, double tolerance, double step)
{
    double best = value, bestD = tolerance;
    for (double p : points)
        if (std::abs (p - value) <= bestD)
        {
            best = p;
            bestD = std::abs (p - value);
        }
    if (best == value && step > 0)
    {
        const double g = std::round (value / step) * step;
        if (std::abs (g - value) <= tolerance)
            return g;
    }
    return best;
}

double snapMove (double start, double length, const std::vector<double>& points, double tolerance, double step)
{
    const double a = snap (start, points, tolerance, step);
    const double b = snap (start + length, points, tolerance, step) - length;
    if (a != start && (b == start || std::abs (a - start) <= std::abs (b - start)))
        return a;
    if (b != start)
        return b;
    return start;
}

//==============================================================================
Mixer::Mixer (juce::AudioFormatManager& fm) : formats (fm) {}

void Mixer::setSession (const Session& s)
{
    const juce::ScopedLock sl (lock);
    const bool sameShape = s.tracks.size() == session.tracks.size() && trackChains.size() == s.tracks.size();
    session = s;
    rate_ = s.rate > 0 ? s.rate : 44100;
    peaks.assign (session.tracks.size(), 0.0f);
    trackLufs.resize (session.tracks.size(), 0.0);
    if (! sameShape || masterChain == nullptr)
        resetChains();
    else
    {
        for (size_t i = 0; i < session.tracks.size(); ++i)
            trackChains[i]->update (session.tracks[i].fx); // settings change between blocks, ramped
        masterChain->update (session.master);
    }
}

juce::int64 Mixer::frames() const
{
    return (juce::int64) std::ceil (session.length() * rate_);
}

void Mixer::resetChains()
{
    trackChains.clear();
    for (size_t i = 0; i < session.tracks.size(); ++i)
    {
        trackChains.push_back (std::make_unique<Chain> (rate_, session.tracks[i].fx));
        trackChains.back()->setLufsDb (i < trackLufs.size() ? trackLufs[i] : 0.0);
    }
    masterChain = std::make_unique<Chain> (rate_, session.master);
    masterChain->setLufsDb (masterLufs);
}

const juce::AudioBuffer<float>* Mixer::audioFor (const juce::String& path)
{
    auto it = cache.find (path);
    if (it != cache.end())
        return it->second.get();
    std::unique_ptr<juce::AudioBuffer<float>> buf;
    std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (juce::File (path)));
    if (r != nullptr && r->lengthInSamples > 0)
    {
        juce::AudioBuffer<float> raw (2, (int) r->lengthInSamples);
        r->read (&raw, 0, (int) r->lengthInSamples, 0, true, true);
        if (r->numChannels == 1)
            raw.copyFrom (1, 0, raw, 0, 0, raw.getNumSamples());
        if ((int) r->sampleRate == rate_)
            buf = std::make_unique<juce::AudioBuffer<float>> (std::move (raw));
        else
        {
            const double ratio = r->sampleRate / rate_;
            const int outN = (int) std::ceil (raw.getNumSamples() / ratio);
            buf = std::make_unique<juce::AudioBuffer<float>> (2, outN);
            for (int ch = 0; ch < 2; ++ch)
            {
                juce::LagrangeInterpolator li;
                li.process (ratio, raw.getReadPointer (ch), buf->getWritePointer (ch), outN, raw.getNumSamples(), 0);
            }
        }
    }
    auto* p = buf.get();
    cache[path] = std::move (buf); // a missing or broken file plays as silence
    return p;
}

void Mixer::mixTrack (juce::AudioBuffer<float>& out, juce::int64 start, int n, int ti)
{
    out.clear (0, n);
    bool soloed = false;
    for (auto& tr : session.tracks)
        soloed = soloed || tr.solo;
    auto& t = session.tracks[(size_t) ti];
    float trackPeak = 0;
    if (t.mute || (soloed && ! t.solo))
    {
        peaks[(size_t) ti] = 0;
        return;
    }
    const float tg = juce::Decibels::decibelsToGain ((float) t.gainDb, -100.0f);
    for (auto& c : t.clips)
    {
        const juce::int64 cs = (juce::int64) std::llround (c.start * rate_);
        const juce::int64 total = (juce::int64) std::llround (c.length * rate_);
        const juce::int64 a = std::max (start, cs), b = std::min (start + n, cs + total);
        if (a >= b)
            continue;
        const auto* src = audioFor (c.path);
        if (src == nullptr)
            continue;
        const juce::int64 off = (juce::int64) std::llround (c.offset * rate_);
        const float cg = tg * juce::Decibels::decibelsToGain ((float) c.gainDb, -100.0f);
        const juce::int64 nIn = (juce::int64) std::llround (c.fadeIn * rate_), nOut = (juce::int64) std::llround (c.fadeOut * rate_);
        for (juce::int64 f = a; f < b; ++f)
        {
            const juce::int64 idx = f - cs;
            const juce::int64 si = off + idx;
            if (si < 0 || si >= src->getNumSamples())
                continue;
            float gain = cg;
            if (nIn > 0)
                gain *= (float) juce::jlimit<double> (0.0, 1.0, (double) idx / (double) nIn);
            if (nOut > 0)
                gain *= (float) juce::jlimit<double> (0.0, 1.0, (double) (total - idx) / (double) nOut);
            for (int ch = 0; ch < 2; ++ch)
            {
                const float v = src->getSample (ch, (int) si) * gain;
                out.addSample (ch, (int) (f - start), v);
                trackPeak = std::max (trackPeak, std::abs (v));
            }
        }
    }
    peaks[(size_t) ti] = trackPeak;
}

void Mixer::mix (juce::AudioBuffer<float>& out, juce::int64 start, int n)
{
    const juce::ScopedLock sl (lock);
    out.clear (0, n);
    juce::AudioBuffer<float> tmp (2, n);
    for (int ti = 0; ti < (int) session.tracks.size(); ++ti)
    {
        mixTrack (tmp, start, n, ti);
        for (int ch = 0; ch < 2; ++ch)
            out.addFrom (ch, 0, tmp, ch, 0, n);
    }
}

void Mixer::processBlock (juce::AudioBuffer<float>& out, juce::int64 start, int n)
{
    out.clear (0, n);
    juce::AudioBuffer<float> tmp (2, n);
    for (int ti = 0; ti < (int) session.tracks.size() && ti < (int) trackChains.size(); ++ti)
    {
        mixTrack (tmp, start, n, ti);
        trackChains[(size_t) ti]->process (tmp, n);
        for (int ch = 0; ch < 2; ++ch)
            out.addFrom (ch, 0, tmp, ch, 0, n);
    }
    if (masterChain != nullptr)
        masterChain->process (out, n);
}

void Mixer::setNextReadPosition (juce::int64 p)
{
    const juce::ScopedLock sl (lock);
    resetChains();
    // the two chains (track + master) are 2 x latency late: run that much ahead once and drop it
    const int pre = 2 * chainLatency (rate_);
    juce::AudioBuffer<float> skip (2, pre);
    processBlock (skip, p, pre);
    renderPos = p + pre;
    pos = p;
}

void Mixer::measureLufs()
{
    const juce::ScopedLock sl (lock);
    const juce::int64 total = frames();
    if (total <= 0)
        return;
    bool any = chainValue (session.master, "targetLufs") != 0;
    for (auto& t : session.tracks)
        any = any || chainValue (t.fx, "targetLufs") != 0;
    if (! any)
        return;
    juce::AudioBuffer<float> sum (2, (int) total);
    sum.clear();
    trackLufs.assign (session.tracks.size(), 0.0);
    for (int ti = 0; ti < (int) session.tracks.size(); ++ti)
    {
        juce::AudioBuffer<float> raw (2, (int) total);
        mixTrack (raw, 0, (int) total, ti);
        trackLufs[(size_t) ti] = lufsGainDb (raw, rate_, session.tracks[(size_t) ti].fx);
        if (chainActive (session.tracks[(size_t) ti].fx))
            renderChain (raw, rate_, session.tracks[(size_t) ti].fx, false);
        for (int ch = 0; ch < 2; ++ch)
            sum.addFrom (ch, 0, raw, ch, 0, (int) total);
    }
    masterLufs = lufsGainDb (sum, rate_, session.master);
    for (size_t i = 0; i < trackChains.size(); ++i)
        trackChains[i]->setLufsDb (trackLufs[i]);
    if (masterChain != nullptr)
        masterChain->setLufsDb (masterLufs);
}

juce::AudioBuffer<float> Mixer::renderAll()
{
    const juce::ScopedLock sl (lock);
    const juce::int64 total = frames();
    juce::AudioBuffer<float> sum (2, (int) juce::jmax<juce::int64> (1, total));
    sum.clear();
    for (int ti = 0; ti < (int) session.tracks.size(); ++ti)
    {
        juce::AudioBuffer<float> raw (2, (int) total);
        mixTrack (raw, 0, (int) total, ti);
        if (chainActive (session.tracks[(size_t) ti].fx))
            renderChain (raw, rate_, session.tracks[(size_t) ti].fx, false);
        for (int ch = 0; ch < 2; ++ch)
            sum.addFrom (ch, 0, raw, ch, 0, (int) total);
    }
    renderChain (sum, rate_, session.master, true);
    return sum;
}

void Mixer::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    const juce::ScopedLock sl (lock);
    juce::AudioBuffer<float> tmp (2, info.numSamples);
    processBlock (tmp, renderPos, info.numSamples);
    for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
        info.buffer->copyFrom (ch, info.startSample, tmp, juce::jmin (ch, 1), 0, info.numSamples);
    renderPos += info.numSamples;
    pos += info.numSamples;
}
} // namespace aicore::tracks
