// AICore-C — AI > ACE · FLAMINGO (the old AICORE's ai_gui.py): two buttons switch ACE · create and
// FLAMINGO · listen. ACE: START FROM (Import music / From scratch), the song as a wave where a drag chooses the
// PART TO CHANGE, Repaint part / Cover whole song, the prompt, BPM (length / strength), GENERATE (the audio
// worker's ace-generate job) and the takes (play, Open in EDIT, Save). FLAMINGO: the listener's screen, as the
// old one (Diogo 2026-11-16: Flamingo paused, so the screen only, no service behind it yet).
#pragma once

#include "../ui/Page.h"
#include "../ui/Panels.h"
#include "../ui/Worker.h"
#include "../sample/SamplePage.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace aicore::ai
{
/** One file played through the app's output (a take, the part). */
class FilePlayer
{
public:
    ~FilePlayer();
    bool load (const juce::File&);
    void play (double from = 0);
    void stop();
    bool playing() const { return transport.isPlaying(); }
    double seconds() const { return transport.getCurrentPosition(); }
    double length() const { return transport.getLengthInSeconds(); }
    juce::File file;

private:
    juce::AudioFormatManager formats;
    std::unique_ptr<juce::AudioFormatReaderSource> reader;
    juce::AudioTransportSource transport;
};

class PartWave : public juce::Component
{
public:
    PartWave();
    bool load (const juce::File&, juce::String& error);
    void setPart (double a, double b);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { anchor = -1; }
    std::vector<std::pair<float, float>> peaks;
    double seconds = 0, start = 0, end = 0;
    bool whole = false;

private:
    double toSeconds (float x) const { return juce::jlimit (0.0, seconds, x / juce::jmax (1, getWidth()) * seconds); }
    double anchor = -1;
};

class AiPage : public ui::Page, private juce::Timer
{
public:
    AiPage();
    ~AiPage() override;
    void openMusic (const juce::String& path);
    void showFlamingo (bool on);
    void setStart (bool scratch);
    void setCover (bool cover);

    std::function<void (const juce::StringArray&)> onOpenInTracks;

    bool hasTransport() const override { return true; }
    void togglePlay() override;
    void stop() override;
    void goStart() override;
    bool isPlaying() const override { return player.playing(); }
    juce::String clock() const override;
    void visibilityChanged() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void layoutTakes();
    void generate();
    void done (const juce::var&);
    void playTake (int);
    void playPart();
    void fail (const juce::String& m) { state.setText (m, juce::dontSendNotification); }

    ui::AButton aceButton { juce::String::fromUTF8 ("ACE \xc2\xb7 create") }, flamingoButton { juce::String::fromUTF8 ("FLAMINGO \xc2\xb7 listen") };
    bool flamingo = false;

    // ACE
    ui::AButton fromMusic { "Import music" }, fromScratch { "From scratch" }, musicButton { juce::String::fromUTF8 ("Import\xe2\x80\xa6") };
    juce::Label music;
    PartWave wave;
    ui::AButton repaintButton { "Repaint part" }, coverButton { "Cover whole song" }, listenPart { "Listen to the part" };
    juce::Label promptLabel, bpmLabel, secondsLabel, strengthLabel, noResults, state;
    juce::TextEditor prompt;
    ui::BarSpin bpm { 0, 400, 0, 0, " BPM", "Auto" }, length { 5, 600, 32, 0, " s" }, strength { 0, 1, 0.55, 2 };
    ui::AButton generateButton { "GENERATE" }, stopButton { "Stop" };
    struct Take
    {
        juce::var result;
        std::unique_ptr<ui::AButton> play, openEdit, save;
        juce::Rectangle<int> box;
    };
    std::vector<std::unique_ptr<Take>> takes;
    juce::String source;
    bool scratch = false, cover = false;
    int playingTake = -1;
    juce::Rectangle<int> leftBox, centreBox, partBox, askY, resultsY;
    int headingAskY = 0, headingResultsY = 0, headingMusicY = 0;
    ui::Worker worker;
    FilePlayer player;
    std::unique_ptr<juce::FileChooser> chooser;

    // FLAMINGO (screen only)
    sample::IconButton flamingoDots { sample::IconButton::dots };
    juce::Label flamingoState, chooseAudio, referenceLabel;
    juce::TextEditor reference;
    ui::AButton chooseReference { juce::String::fromUTF8 ("Choose reference\xe2\x80\xa6") }, analyse { "Analyse reference" };
    juce::ComboBox goal;
    juce::Rectangle<int> askBox, setupBox, refBox;
};
} // namespace aicore::ai
