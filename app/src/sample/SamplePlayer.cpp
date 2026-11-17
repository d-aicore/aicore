#include "SamplePlayer.h"

#include "../ui/AudioOut.h"

namespace aicore::sample
{
SamplePlayer::SamplePlayer() = default;

SamplePlayer::~SamplePlayer()
{
    transport.stop();
    ui::AudioOut::get().detach (&transport);
    transport.setSource (nullptr);
}

void SamplePlayer::play (std::shared_ptr<const Audio> a, double r, int f, int l, int from, bool lp, const tracks::ChainSettings& s)
{
    stop();
    {
        const juce::ScopedLock sl (lock);
        const bool same = a == audio && f == first && l == last && r == rate;
        audio = std::move (a);
        rate = r;
        first = f;
        last = juce::jmax (f + 1, l);
        loop = lp;
        settings = s;
        active = tracks::chainActive (s);
        if (! same || ! active)
            lufsDb = 0;
        if (active && (! same || lufsDb == 0) && tracks::chainValue (s, "targetLufs") != 0)
        {
            auto part = range (*audio, first, last); // the LUFS target is measured on the part, as the export
            lufsDb = tracks::lufsGainDb (part, (int) rate, s);
        }
    }
    ui::AudioOut::get().attach (&transport);
    transport.setSource (this, 0, nullptr, rate);
    transport.setPosition ((from - first) / rate);
    transport.start();
}

void SamplePlayer::updateSettings (const tracks::ChainSettings& s)
{
    const juce::ScopedLock sl (lock);
    const bool lufsChanged = tracks::chainValue (s, "targetLufs") != tracks::chainValue (settings, "targetLufs");
    settings = s;
    const bool nowActive = tracks::chainActive (s);
    if (lufsChanged && audio != nullptr && nowActive)
        lufsDb = tracks::chainValue (s, "targetLufs") != 0 ? tracks::lufsGainDb (range (*audio, first, last), (int) rate, s) : 0;
    if (nowActive != active || chain == nullptr)
    {
        active = nowActive;
        restartChain (pos);
    }
    else
    {
        chain->update (s);
        chain->setLufsDb (lufsDb);
    }
}

void SamplePlayer::stop()
{
    transport.stop();
}

int SamplePlayer::position() const
{
    return transport.isPlaying() ? first + (int) (transport.getCurrentPosition() * rate) % juce::jmax (1, last - first) : -1;
}

void SamplePlayer::restartChain (juce::int64 at)
{
    pos = at;
    renderPos = at;
    chain.reset();
    if (! active || audio == nullptr)
        return;
    chain = std::make_unique<tracks::Chain> ((int) rate, settings);
    chain->setLufsDb (lufsDb);
    // feed the look-ahead from `at`: the output then starts exactly at `at`
    const int look = chain->latency();
    juce::AudioBuffer<float> pre (2, look);
    pre.clear();
    for (int i = 0; i < look; ++i)
        if (at + i < last - first)
            for (int ch = 0; ch < 2; ++ch)
                pre.setSample (ch, i, audio->getSample (ch, first + (int) at + i));
    chain->process (pre, look);
    renderPos = at + look;
}

void SamplePlayer::setNextReadPosition (juce::int64 p)
{
    const juce::ScopedLock sl (lock);
    restartChain (juce::jlimit<juce::int64> (0, last - first, p));
}

void SamplePlayer::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    const juce::ScopedLock sl (lock);
    auto& out = *info.buffer;
    const int n = info.numSamples, len = last - first;
    if (audio == nullptr || len <= 0)
    {
        out.clear (info.startSample, n);
        return;
    }
    const double fin = tracks::chainValue (settings, "fadeInS") * rate, fout = tracks::chainValue (settings, "fadeOutS") * rate;
    for (int done = 0; done < n;)
    {
        if (pos >= len)
        {
            if (! loop)
            {
                out.clear (info.startSample + done, n - done);
                return;
            }
            restartChain (0);
        }
        const int m = (int) juce::jmin<juce::int64> (n - done, len - pos);
        juce::AudioBuffer<float> blk (2, m);
        if (active && chain != nullptr)
        {
            blk.clear();
            for (int ch = 0; ch < 2; ++ch)
            {
                const int avail = (int) juce::jlimit<juce::int64> (0, m, len - renderPos);
                if (avail > 0)
                    blk.copyFrom (ch, 0, *audio, ch, first + (int) renderPos, avail);
            }
            chain->process (blk, m);
            renderPos += m;
            if (fin > 0 || fout > 0) // the master's fades, linear, on the part (master_chain.py)
                for (int i = 0; i < m; ++i)
                {
                    const double k = (double) (pos + i);
                    double g = 1;
                    if (fin > 0) g = juce::jmin (g, juce::jlimit (0.0, 1.0, k / fin));
                    if (fout > 0) g = juce::jmin (g, juce::jlimit (0.0, 1.0, (len - k) / fout));
                    for (int ch = 0; ch < 2; ++ch)
                        blk.setSample (ch, i, (float) (blk.getSample (ch, i) * g));
                }
        }
        else
            for (int ch = 0; ch < 2; ++ch)
                blk.copyFrom (ch, 0, *audio, ch, first + (int) pos, m);
        for (int ch = 0; ch < out.getNumChannels(); ++ch)
            out.copyFrom (ch, info.startSample + done, blk, juce::jmin (ch, 1), 0, m);
        pos += m;
        renderPos = juce::jmax (renderPos, pos);
        done += m;
    }
}
} // namespace aicore::sample
