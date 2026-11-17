// AICore-C — the one audio output of the app (the default device). Every page plays through it: a page adds its
// source once, and starts / stops its own transport. Opened the first time something plays.
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>

namespace aicore::ui
{
class AudioOut
{
public:
    static AudioOut& get();
    /** Adds a source to the output mix (once; the source stays the caller's). */
    void attach (juce::AudioSource* source);
    void detach (juce::AudioSource* source);
    double sampleRate();
    /** Closes the device (the app calls it on the way out, before JUCE shuts down). */
    void close();
    ~AudioOut();

private:
    AudioOut() = default;
    void open();
    std::unique_ptr<juce::AudioDeviceManager> device;
    juce::AudioSourcePlayer player;
    juce::MixerAudioSource mixer;
    juce::Array<juce::AudioSource*> attached;
};
} // namespace aicore::ui
