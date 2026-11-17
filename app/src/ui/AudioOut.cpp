#include "AudioOut.h"

namespace aicore::ui
{
AudioOut& AudioOut::get()
{
    static AudioOut out;
    return out;
}

void AudioOut::open()
{
    if (device != nullptr)
        return;
    device = std::make_unique<juce::AudioDeviceManager>();
    device->initialiseWithDefaultDevices (0, 2);
    player.setSource (&mixer);
    device->addAudioCallback (&player);
}

void AudioOut::attach (juce::AudioSource* source)
{
    open();
    if (source == nullptr || attached.contains (source))
        return;
    attached.add (source);
    mixer.addInputSource (source, false);
}

void AudioOut::detach (juce::AudioSource* source)
{
    if (! attached.contains (source))
        return;
    attached.removeFirstMatchingValue (source);
    mixer.removeInputSource (source);
}

double AudioOut::sampleRate()
{
    open();
    if (auto* d = device->getCurrentAudioDevice())
        return d->getCurrentSampleRate();
    return 44100.0;
}

void AudioOut::close()
{
    if (device != nullptr)
        device->removeAudioCallback (&player);
    player.setSource (nullptr);
    mixer.removeAllInputs();
    attached.clear();
    device.reset();
}

AudioOut::~AudioOut()
{
    if (device != nullptr)
        device->removeAudioCallback (&player);
    player.setSource (nullptr);
    mixer.removeAllInputs();
}
} // namespace aicore::ui
