#include "MasterChain.h"

#include <cmath>
#include <limits>
#include <vector>

namespace aicore::tracks
{
namespace
{
const struct { const char* key; double frequency; const char* kind; } kEq[3] = {
    { "lowDb", 100, "low" }, { "midDb", 1000, "peak" }, { "highDb", 8000, "high" } };
const ChainSettings kDefaults { { "gainDb", 0 }, { "lowDb", 0 }, { "midDb", 0 }, { "highDb", 0 }, { "highpassHz", 0 },
                                { "thresholdDb", -18 }, { "ratio", 2 }, { "targetLufs", 0 }, { "ceilingDb", -1 },
                                { "fadeInS", 0 }, { "fadeOutS", 0 }, { "compress", 0 }, { "limit", 0 } };
} // namespace

double chainValue (const ChainSettings& s, const std::string& key)
{
    auto it = s.find (key);
    if (it != s.end())
        return it->second;
    auto d = kDefaults.find (key);
    return d != kDefaults.end() ? d->second : 0.0;
}

bool chainActive (const ChainSettings& s)
{
    for (auto k : { "gainDb", "lowDb", "midDb", "highDb", "highpassHz", "targetLufs", "fadeInS", "fadeOutS" })
        if (std::abs (chainValue (s, k)) > 1e-9)
            return true;
    return chainValue (s, "compress") != 0 || chainValue (s, "limit") != 0;
}

int chainLatency (int rate) { return std::max (1, (int) std::lround (rate * 0.005)); }

Chain::Biquad Chain::rbj (const char* kind, double frequency, double gainDb, int rate)
{
    Biquad q;
    const double w = 2 * juce::MathConstants<double>::pi * std::min (frequency, rate * 0.45) / rate;
    const double c = std::cos (w), sn = std::sin (w), a = std::pow (10.0, gainDb / 40.0);
    const double alpha = sn / (2 * std::sqrt (0.5));
    double b[3], d[3];
    if (std::string (kind) == "peak")
    {
        b[0] = 1 + alpha * a; b[1] = -2 * c; b[2] = 1 - alpha * a;
        d[0] = 1 + alpha / a; d[1] = -2 * c; d[2] = 1 - alpha / a;
    }
    else if (std::string (kind) == "low")
    {
        const double z = 2 * std::sqrt (a) * alpha;
        b[0] = a * ((a + 1) - (a - 1) * c + z); b[1] = 2 * a * ((a - 1) - (a + 1) * c); b[2] = a * ((a + 1) - (a - 1) * c - z);
        d[0] = (a + 1) + (a - 1) * c + z; d[1] = -2 * ((a - 1) + (a + 1) * c); d[2] = (a + 1) + (a - 1) * c - z;
    }
    else
    {
        const double z = 2 * std::sqrt (a) * alpha;
        b[0] = a * ((a + 1) + (a - 1) * c + z); b[1] = -2 * a * ((a - 1) + (a + 1) * c); b[2] = a * ((a + 1) + (a - 1) * c - z);
        d[0] = (a + 1) - (a - 1) * c + z; d[1] = 2 * ((a - 1) - (a + 1) * c); d[2] = (a + 1) - (a - 1) * c - z;
    }
    q.b0 = b[0] / d[0]; q.b1 = b[1] / d[0]; q.b2 = b[2] / d[0]; q.a1 = d[1] / d[0]; q.a2 = d[2] / d[0];
    q.on = true;
    return q;
}

Chain::Biquad Chain::butterHighpass (double f, int rate)
{
    // scipy butter(2, f, 'highpass', fs) == bilinear 2nd-order Butterworth (Q = 1/sqrt2) with prewarping
    Biquad q;
    const double k = std::tan (juce::MathConstants<double>::pi * f / rate), r2 = std::sqrt (2.0);
    const double norm = 1 / (1 + r2 * k + k * k);
    q.b0 = norm; q.b1 = -2 * norm; q.b2 = norm;
    q.a1 = 2 * (k * k - 1) * norm; q.a2 = (1 - r2 * k + k * k) * norm;
    q.on = true;
    return q;
}

Chain::Chain (int r, const ChainSettings& st) : rate (r), look (chainLatency (r)), tail (2, chainLatency (r))
{
    tail.clear();
    update (st);
}

void Chain::update (const ChainSettings& st)
{
    s = st;
    targetGain = std::pow (10.0, chainValue (s, "gainDb") / 20.0);
    for (int i = 0; i < 3; ++i)
    {
        const double v = chainValue (s, kEq[i].key);
        if (v != 0)
        {
            auto z1 = eq[i].z1, z2 = eq[i].z2;
            const bool was = eq[i].on;
            eq[i] = rbj (kEq[i].kind, kEq[i].frequency, v, rate);
            if (was)
                for (int ch = 0; ch < 2; ++ch) { eq[i].z1[ch] = z1[ch]; eq[i].z2[ch] = z2[ch]; }
        }
        else
            eq[i] = {};
    }
    const double h = chainValue (s, "highpassHz");
    hp = h > 0 ? butterHighpass (h, rate) : Biquad {};
    compress = chainValue (s, "compress") != 0;
    threshold = chainValue (s, "thresholdDb");
    ratio = std::max (1.0, chainValue (s, "ratio"));
    targetLufs = chainValue (s, "targetLufs");
    limit = chainValue (s, "limit") != 0;
    ceiling = std::pow (10.0, chainValue (s, "ceilingDb") / 20.0);
}

void Chain::reset()
{
    for (auto& q : eq)
        q.clear();
    hp.clear();
    comp = compAtt = lim = 0;
    gain = lufsLin = -1;
    tail.clear();
}

void Chain::pre (juce::AudioBuffer<float>& b, int n)
{
    if (n <= 0)
        return;
    const double from = gain < 0 ? targetGain : gain;
    const double release = std::exp (-1.0 / (rate * 0.1)), attack = std::exp (-1.0 / (rate * 0.01));
    auto* l = b.getWritePointer (0);
    auto* r = b.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        // gain ramps over the block (no clicks): linspace(current, target, n, endpoint=False) + step
        const double g = std::abs (from - targetGain) < 1e-9 ? targetGain : from + (targetGain - from) * (i + 1) / n;
        double x[2] = { l[i] * g, r[i] * g };
        for (auto& q : eq)
            if (q.on)
                for (int ch = 0; ch < 2; ++ch)
                    x[ch] = q.run (ch, x[ch]);
        if (hp.on)
            for (int ch = 0; ch < 2; ++ch)
                x[ch] = hp.run (ch, x[ch]);
        if (compress)
        {
            const double peak = std::max (std::abs (x[0]), std::abs (x[1]));
            const double over = std::max (0.0, 20 * std::log10 (std::max (peak, 1e-12)) - threshold);
            const double reduction = over * (1 - 1 / ratio);
            comp = std::max (reduction, comp * release);                 // instant rise, exponential fall
            compAtt = (1 - attack) * comp + attack * compAtt;            // attack smoother
            const double cg = std::pow (10.0, -compAtt / 20.0);
            x[0] *= cg;
            x[1] *= cg;
        }
        l[i] = (float) x[0];
        r[i] = (float) x[1];
    }
    if (! compress)
        comp = compAtt = 0;
    gain = targetGain;
}

void Chain::post (juce::AudioBuffer<float>& b, int n)
{
    if (n <= 0)
        return;
    const double want = targetLufs != 0 ? std::pow (10.0, lufsDb / 20.0) : 1.0;
    const double from = lufsLin < 0 ? want : lufsLin;
    const double release = std::exp (-1.0 / (rate * 0.08));
    // ext = tail + x ; out = ext[:n] ; tail = ext[n:]
    juce::AudioBuffer<float> ext (2, look + n);
    for (int ch = 0; ch < 2; ++ch)
    {
        ext.copyFrom (ch, 0, tail, ch, 0, look);
        for (int i = 0; i < n; ++i)
        {
            const double g = std::abs (from - want) < 1e-9 ? want : from + (want - from) * (i + 1) / n;
            ext.setSample (ch, look + i, (float) (b.getSample (ch, i) * g));
        }
    }
    lufsLin = want;
    for (int ch = 0; ch < 2; ++ch)
        tail.copyFrom (ch, 0, ext, ch, n, look);
    for (int i = 0; i < n; ++i)
    {
        double o[2] = { ext.getSample (0, i), ext.getSample (1, i) };
        if (limit)
        {
            double ahead = 0; // max of peaks[k .. k + look]
            for (int j = i; j <= i + look && j < look + n; ++j)
                ahead = std::max ({ ahead, (double) std::abs (ext.getSample (0, j)), (double) std::abs (ext.getSample (1, j)) });
            const double reduction = std::max (0.0, 20 * std::log10 (std::max (ahead, 1e-12) / ceiling));
            lim = std::max (reduction, lim * release);
            const double lg = std::pow (10.0, -lim / 20.0);
            for (auto& v : o)
                v = juce::jlimit (-ceiling, ceiling, v * lg);
        }
        for (int ch = 0; ch < 2; ++ch)
        {
            const double v = o[ch];
            b.setSample (ch, i, std::isfinite (v) ? (float) v : 0.0f);
        }
    }
    if (! limit)
        lim = 0;
}

//==============================================================================
double integratedLufs (const juce::AudioBuffer<float>& audio, int rate)
{
    const int n = audio.getNumSamples();
    const int block = (int) std::lround (0.4 * rate), step = (int) std::lround (0.1 * rate);
    if (n < block || block < 1)
        return std::numeric_limits<double>::quiet_NaN();
    // K-weighting as libebur128 designs it for this rate
    struct Q { double b[3], a[3]; };
    double f0 = 1681.974450955533, gain = 3.999843853973347, q = 0.7071752369554196;
    double k = std::tan (juce::MathConstants<double>::pi * f0 / rate), vh = std::pow (10.0, gain / 20), vb = std::pow (vh, 0.4996667741545416);
    double a0 = 1 + k / q + k * k;
    Q shelf { { (vh + vb * k / q + k * k) / a0, 2 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0 }, { 1, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0 } };
    f0 = 38.13547087602444; q = 0.5003270373238773;
    k = std::tan (juce::MathConstants<double>::pi * f0 / rate); a0 = 1 + k / q + k * k;
    Q high { { 1, -2, 1 }, { 1, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0 } };
    std::vector<double> power ((size_t) n + 1, 0.0);
    std::vector<std::vector<double>> sq (2, std::vector<double> ((size_t) n));
    for (int ch = 0; ch < std::min (2, audio.getNumChannels()); ++ch)
    {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0, u1 = 0, u2 = 0, w1 = 0, w2 = 0;
        for (int i = 0; i < n; ++i)
        {
            const double x = audio.getSample (ch, i);
            const double y = shelf.b[0] * x + shelf.b[1] * x1 + shelf.b[2] * x2 - shelf.a[1] * y1 - shelf.a[2] * y2;
            x2 = x1; x1 = x; y2 = y1; y1 = y;
            const double w = high.b[0] * y + high.b[1] * u1 + high.b[2] * u2 - high.a[1] * w1 - high.a[2] * w2;
            u2 = u1; u1 = y; w2 = w1; w1 = w;
            sq[(size_t) ch][(size_t) i] = w * w;
        }
    }
    for (int i = 0; i < n; ++i)
        power[(size_t) i + 1] = power[(size_t) i] + sq[0][(size_t) i] + sq[1][(size_t) i];
    std::vector<double> z;
    for (int s = 0; s + block <= n; s += step)
        z.push_back ((power[(size_t) s + (size_t) block] - power[(size_t) s]) / block);
    double sum = 0;
    int cnt = 0;
    for (double v : z)
        if (-0.691 + 10 * std::log10 (std::max (v, 1e-30)) > -70) { sum += v; ++cnt; }
    if (cnt == 0)
        return std::numeric_limits<double>::quiet_NaN();
    const double relative = -0.691 + 10 * std::log10 (sum / cnt) - 10;
    double sum2 = 0;
    int cnt2 = 0;
    for (double v : z)
    {
        const double loud = -0.691 + 10 * std::log10 (std::max (v, 1e-30));
        if (loud > -70 && loud > relative) { sum2 += v; ++cnt2; }
    }
    return cnt2 ? -0.691 + 10 * std::log10 (sum2 / cnt2) : std::numeric_limits<double>::quiet_NaN();
}

double lufsGainDb (const juce::AudioBuffer<float>& audio, int rate, const ChainSettings& s)
{
    const double target = chainValue (s, "targetLufs");
    if (target == 0)
        return 0.0;
    juce::AudioBuffer<float> x (audio);
    Chain c (rate, s);
    for (int a = 0; a < x.getNumSamples(); a += 8192)
    {
        const int n = std::min (8192, x.getNumSamples() - a);
        juce::AudioBuffer<float> blk (x.getArrayOfWritePointers(), 2, a, n);
        c.pre (blk, n);
    }
    const double measured = integratedLufs (x, rate);
    return std::isnan (measured) ? 0.0 : juce::jlimit (-24.0, 24.0, target - measured);
}

void renderChain (juce::AudioBuffer<float>& audio, int rate, const ChainSettings& s, bool fades)
{
    Chain c (rate, s);
    c.setLufsDb (lufsGainDb (audio, rate, s));
    const int n = audio.getNumSamples(), look = c.latency();
    juce::AudioBuffer<float> padded (2, n + look);
    padded.clear();
    for (int ch = 0; ch < 2; ++ch)
        padded.copyFrom (ch, 0, audio, juce::jmin (ch, audio.getNumChannels() - 1), 0, n);
    for (int a = 0; a < n + look; a += 8192)
    {
        const int m = std::min (8192, n + look - a);
        juce::AudioBuffer<float> blk (padded.getArrayOfWritePointers(), 2, a, m);
        c.process (blk, m);
    }
    for (int ch = 0; ch < audio.getNumChannels() && ch < 2; ++ch)
        audio.copyFrom (ch, 0, padded, ch, look, n);
    if (fades)
    {
        const int nIn = (int) std::lround (chainValue (s, "fadeInS") * rate), nOut = (int) std::lround (chainValue (s, "fadeOutS") * rate);
        if (nIn > 0 || nOut > 0)
            for (int i = 0; i < n; ++i)
            {
                double g = 1;
                if (nIn > 0) g = std::min (g, juce::jlimit (0.0, 1.0, (double) i / nIn));
                if (nOut > 0) g = std::min (g, juce::jlimit (0.0, 1.0, (double) (n - i) / nOut));
                for (int ch = 0; ch < audio.getNumChannels(); ++ch)
                    audio.setSample (ch, i, (float) (audio.getSample (ch, i) * g));
            }
    }
}
} // namespace aicore::tracks
