// AICore-C — EDIT > TRACKS session model and its edits, ported from the old AICORE's arrange.py
// (D:\AICore\app ... aicore_studio/arrange.py). Same fields, same rules; the files are never changed.
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include "MasterChain.h"

#include <map>
#include <memory>
#include <vector>

namespace aicore::tracks
{
constexpr double kMinClip = 0.01;

struct Clip
{
    juce::String path;
    double start = 0, offset = 0, length = 0, sourceLength = 0;
    double gainDb = 0, fadeIn = 0, fadeOut = 0;
    juce::String name;
    int id = 0;
    double end() const { return start + length; }
    juce::String label() const { return name.isNotEmpty() ? name : juce::File (path).getFileNameWithoutExtension(); }
};

struct Track
{
    juce::String name;
    std::vector<Clip> clips;
    double gainDb = 0;
    bool mute = false, solo = false;
    ChainSettings fx; // the track's inserts (EQ, HP, compressor, LUFS, limiter), as the old app's track.fx
    int id = 0;
};

struct Session
{
    std::vector<Track> tracks;
    double bpm = 145.0;
    int rate = 0;
    ChainSettings master; // the master's chain (+ volume and fades)
    double length() const;
    Clip* find (int clipId, int* trackIndex = nullptr);
};

int newId();
Track& addTrack (Session&, const juce::String& name = {});
/** A whole file as a clip on track `track` (tracks are added up to it); null if the file cannot be read. */
Clip* addClip (Session&, int track, const juce::File& file, double start);
Clip* split (Session&, int clipId, double at);
std::vector<int> splitAt (Session&, double at, const std::vector<int>& clipIds, int track);
void trimStart (Session&, int clipId, double newStart);
void trimEnd (Session&, int clipId, double newEnd);
void move (Session&, int clipId, double start, int track);
void remove (Session&, const std::vector<int>& clipIds);
std::vector<int> duplicate (Session&, const std::vector<int>& clipIds);
double gridStep (double bpm, double pixelsPerSecond);
std::vector<double> snapPoints (const Session&, const std::vector<int>& exclude, double playhead);
double snap (double value, const std::vector<double>& points, double tolerance, double step);
double snapMove (double start, double length, const std::vector<double>& points, double tolerance, double step);

/** The session mixed to stereo at its rate (Mixdown in arrange.py): clip gain, fades, track gain, mute and
    solo. Files at another rate are read once and resampled. Also an AudioSource for the player. */
class Mixer : public juce::PositionableAudioSource
{
public:
    explicit Mixer (juce::AudioFormatManager&);
    void setSession (const Session&); // copies it (the page edits its own)
    int rate() const { return rate_; }
    juce::int64 frames() const;
    /** frames [start, start+n) of the raw mix (clips, track gain, mute/solo; no chains) into out. */
    void mix (juce::AudioBuffer<float>& out, juce::int64 start, int n);
    /** one track's raw frames (clips + track gain) into out; -1 = no track filter */
    void mixTrack (juce::AudioBuffer<float>& out, juce::int64 start, int n, int track);
    /** The whole session through every track's chain and the master chain, aligned, with the master's fades
        (the export: what is heard is what is written). */
    juce::AudioBuffer<float> renderAll();
    /** Measures what the LUFS targets ask for (whole tracks / whole mix) before playing. */
    void measureLufs();
    float trackPeak (int track) const { return track >= 0 && track < (int) peaks.size() ? peaks[(size_t) track] : 0.0f; }

    void prepareToPlay (int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock (const juce::AudioSourceChannelInfo&) override;
    void setNextReadPosition (juce::int64 p) override;
    juce::int64 getNextReadPosition() const override { return pos; }
    juce::int64 getTotalLength() const override { return frames(); }
    bool isLooping() const override { return false; }

private:
    const juce::AudioBuffer<float>* audioFor (const juce::String& path);
    juce::AudioFormatManager& formats;
    Session session;
    int rate_ = 44100;
    juce::int64 pos = 0, renderPos = 0;
    std::vector<std::unique_ptr<Chain>> trackChains;
    std::unique_ptr<Chain> masterChain;
    std::vector<double> trackLufs;
    double masterLufs = 0;
    void resetChains();
    void processBlock (juce::AudioBuffer<float>& out, juce::int64 start, int n); // chains, live
    std::map<juce::String, std::unique_ptr<juce::AudioBuffer<float>>> cache;
    std::vector<float> peaks;
    juce::CriticalSection lock;
};
} // namespace aicore::tracks
