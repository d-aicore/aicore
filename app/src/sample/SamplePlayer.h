// AICore-C — EDIT > SAMPLE's player (live_player.py MasteredPlayer): the part that would be exported (the
// selection, or the whole sound) through the master chain, live; the LUFS target and the master's fades are
// measured on that part. Loops inside the selection. Plays through the app's one output.
#pragma once

#include "SampleEdit.h"
#include "../tracks/MasterChain.h"

#include <juce_audio_devices/juce_audio_devices.h>

namespace aicore::sample
{
class SamplePlayer : public juce::PositionableAudioSource
{
public:
    SamplePlayer();
    ~SamplePlayer() override;
    /** region = [first, last) of audio; plays from `from` (inside it); loops [a, b) when loop. */
    void play (std::shared_ptr<const Audio> audio, double rate, int first, int last, int from, bool loop,
               const tracks::ChainSettings&);
    void updateSettings (const tracks::ChainSettings&);
    void stop();
    bool playing() const { return transport.isPlaying(); }
    /** The frame of the sound being heard (or -1). */
    int position() const;

    // PositionableAudioSource (the transport reads it)
    void prepareToPlay (int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock (const juce::AudioSourceChannelInfo&) override;
    void setNextReadPosition (juce::int64 p) override;
    juce::int64 getNextReadPosition() const override { return pos; }
    juce::int64 getTotalLength() const override { return last - first; }
    bool isLooping() const override { return loop; }

private:
    void restartChain (juce::int64 at);
    juce::AudioTransportSource transport;
    std::shared_ptr<const Audio> audio;
    double rate = 44100;
    int first = 0, last = 0;
    bool loop = false;
    juce::int64 pos = 0, renderPos = 0;
    tracks::ChainSettings settings;
    std::unique_ptr<tracks::Chain> chain;
    double lufsDb = 0;
    bool active = false;
    juce::CriticalSection lock;
};
} // namespace aicore::sample
