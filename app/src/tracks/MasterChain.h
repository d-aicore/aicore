// AICore-C — the old AICORE's mastering chain (master_chain.py + stem_audio.py), block by block:
// gain -> EQ (low shelf 100 Hz, peak 1 kHz, high shelf 8 kHz, RBJ) -> high-pass (2nd-order Butterworth) ->
// compressor (peak over threshold, 10 ms attack, 100 ms release) -> LUFS target (BS.1770-4 integrated, measured
// on the pre-chain output) -> sample-peak limiter (5 ms look-ahead, 80 ms release, ceiling) ; fades on the master.
// Every chain delays its output by latency(rate) frames, also when the limiter is off (stems stay aligned).
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <map>
#include <string>

namespace aicore::tracks
{
using ChainSettings = std::map<std::string, double>; // keys of the old app: gainDb lowDb midDb highDb highpassHz
                                                       // compress thresholdDb ratio targetLufs limit ceilingDb fadeInS fadeOutS

double chainValue (const ChainSettings&, const std::string& key);
bool chainActive (const ChainSettings&); // anything that changes the sound
int chainLatency (int rate);
/** Integrated loudness (ITU-R BS.1770-4, 400 ms blocks every 100 ms, -70 / -10 LU gates); NaN for silence. */
double integratedLufs (const juce::AudioBuffer<float>&, int rate);

class Chain
{
public:
    Chain (int rate, const ChainSettings& = {});
    void update (const ChainSettings&);
    void setLufsDb (double db) { lufsDb = db; }
    void reset();
    /** gain, EQ, high-pass, compressor (no delay), in place */
    void pre (juce::AudioBuffer<float>&, int n);
    /** LUFS gain and limiter; the output is latency frames late, in place */
    void post (juce::AudioBuffer<float>&, int n);
    void process (juce::AudioBuffer<float>& b, int n) { pre (b, n); post (b, n); }
    int latency() const { return look; }
    const ChainSettings& settings() const { return s; }

private:
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1[2] {}, z2[2] {};
        bool on = false;
        double run (int ch, double x)
        {
            const double y = b0 * x + z1[ch];
            z1[ch] = b1 * x - a1 * y + z2[ch];
            z2[ch] = b2 * x - a2 * y;
            return y;
        }
        void clear() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
    };
    static Biquad rbj (const char* kind, double frequency, double gainDb, int rate);
    static Biquad butterHighpass (double frequency, int rate);

    int rate, look;
    ChainSettings s;
    double targetGain = 1, gain = -1, lufsLin = -1, lufsDb = 0;
    Biquad eq[3], hp;
    bool compress = false, limit = false;
    double threshold = -18, ratio = 2, targetLufs = 0, ceiling = 1;
    double comp = 0, compAtt = 0, lim = 0;
    juce::AudioBuffer<float> tail; // the look-ahead: input not yet sent out
};

/** The gain the LUFS target asks for: measured after gain, EQ, high-pass and compressor; 0 when off. */
double lufsGainDb (const juce::AudioBuffer<float>& audio, int rate, const ChainSettings&);
/** The whole sound through the chain, aligned (look-ahead taken off), with its fades if fades = true. */
void renderChain (juce::AudioBuffer<float>& audio, int rate, const ChainSettings&, bool fades = true);
} // namespace aicore::tracks
