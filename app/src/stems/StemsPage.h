// AICore-C — EDIT > STEMS and EDIT > VOICE (the old AICORE's StemsPage in audio_studio_gui.py + trackview.py):
// the song field, Separate (Demucs through AICore's audio worker), the stems as tracks (TRACKS: a lane per stem,
// head with M, S, Listen, volume and meter; the clip with its wave) and as Core's mixer (MIX: a strip per stem with
// its inserts and fader, the master strip, the chosen strip's inserts at the right). Export what is heard.
#pragma once

#include "../tracks/Arrange.h"
#include "../tracks/ChainPanel.h"
#include "../ui/Page.h"
#include "../ui/Worker.h"
#include "../sample/SamplePage.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace aicore::stems
{
struct StemRow
{
    juce::String name, path, original, role, sourceHash;
    bool mute = false, solo = false;
    tracks::ChainSettings settings; // gainDb lowDb midDb highDb highpassHz compress limit fadeInS fadeOutS targetLufs
    juce::StringArray versions;
    juce::var extra; // the other fields the worker gave (info ...), kept as they were
};

struct Peaks
{
    double seconds = 0;
    std::vector<std::array<float, 3>> channels[2]; // per column: low, high, rms
    int numChannels = 0;
};

juce::Colour stemColour (const juce::String& name);

class StemsPage;

/** TRACKS: the ruler, and a lane per stem (head + clip). Painted in one component, as Core's arrange. */
class Lanes : public juce::Component
{
public:
    explicit Lanes (StemsPage&);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    int laneHeight() const;
    static constexpr int headW = 456, rulerH = 22, rowMin = 84, waveLeft = 35, waveRight = 5;

private:
    struct Hit { int row = -1; juce::String part; };
    Hit hit (juce::Point<int>) const;
    juce::Rectangle<int> laneBox (int r) const { return { 0, rulerH + r * laneHeight(), getWidth(), laneHeight() }; }
    juce::Rectangle<int> clipBox (int r) const { return laneBox (r).withTrimmedLeft (headW + 4).reduced (1, 3); }
    juce::Rectangle<int> partOf (int r, const juce::String& part) const;
    float xOfSeconds (const juce::Rectangle<int>& clip, double s) const;
    double secondsOfX (const juce::Rectangle<int>& clip, float x) const;
    StemsPage& page;
    int pressRow = -1;
    float pressX = 0;
    double pressSeconds = 0;
    bool draggingVolume = false;
    double volumeStart = 0;
};

/** MIX: the strips, the master strip and the chosen strip's inserts. */
class Mixer : public juce::Component
{
public:
    explicit Mixer (StemsPage&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void refresh(); // the chosen strip's inserts
    static constexpr int stripW = 140, header = 28, insertRow = 22, insertsW = 300;
    juce::Viewport insertsView;
    tracks::ChainPanel inserts;

private:
    void paintStrip (juce::Graphics&, juce::Rectangle<int>, int row);
    juce::Rectangle<int> stripBox (int row) const; // -1 = master
    juce::Rectangle<int> faderBox (int row) const;
    int stripAt (juce::Point<int>) const;
    StemsPage& page;
    int dragRow = -2;
    double dragStart = 0;
    float dragY = 0;
};

class StemsPage : public ui::Page, private juce::Timer
{
public:
    explicit StemsPage (bool voice);
    ~StemsPage() override;

    // the window's top bar
    bool hasTransport() const override { return true; }
    void togglePlay() override { togglePlayKeep (false); }
    void stop() override;
    void goStart() override { seek (0.0); }
    bool isPlaying() const override { return transport.isPlaying(); }
    juce::String clock() const override;
    void visibilityChanged() override;

    void addRows (const std::vector<StemRow>&);
    void importFiles (const juce::StringArray& paths);
    void separate();
    void exportTracks();
    void undo();
    void redo();

    std::function<void (const juce::String& path)> onEditAsSample;
    std::function<void (const juce::StringArray& paths)> onOpenInTracks;
    std::function<void (const juce::String& path)> onTrainingAudio;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    // what the lanes and the mixer read and change
    std::vector<StemRow> rows;
    std::map<juce::String, std::shared_ptr<Peaks>> peaks;
    tracks::ChainSettings master;
    int current = -1;     // the chosen track
    bool masterChosen = false;
    int only = -1;        // the stem playing alone (Listen)
    struct Zone { int row = -1; double t0 = 0, t1 = 0; } zone;
    double playhead = 0;
    double timeline() const;
    void select (int r);
    void chooseMaster();
    void changed (bool remember = false); // a value moved: levels, session, repaint
    void remember();
    void seek (double seconds);
    void listenStem (int r);
    void setZone (Zone z);
    float levelDb (int r) const;
    const bool voiceOnly;
    void showView (int v);

private:
    void timerCallback() override;
    void togglePlayKeep (bool keepOnly);
    void options();
    void fail (const juce::String& m) { state.setText (m, juce::dontSendNotification); }
    void startTask (const juce::String& kind, juce::var fields);
    void done (const juce::var& result);
    void setRows (std::vector<StemRow>);
    void loadPeaks (const juce::String& path);
    void saveSession();
    void openSession();
    void resetMaster();
    void updatePlayer();
    std::vector<StemRow> heardRows() const;
    juce::var rowsVar (const std::vector<StemRow>&) const;
    juce::var masterVar() const;
    bool zoneAudio (int& r, sample::Audio& audio, double& rate, int& a, int& b);
    void writeEdit (int r, const sample::Audio& audio, double rate, const juce::String& note);
    void copyZone();
    void cutZone();
    void deleteZone();
    void paste();
    void renameTrack();
    void deleteTrack();
    void enhance();
    void applyCandidate();

    juce::TextEditor source;
    sample::IconButton folderButton { sample::IconButton::folder }, dotsButton { sample::IconButton::dots };
    juce::ComboBox mode;
    ui::AButton separateButton { "Separate" }, tracksButton { "TRACKS" }, mixButton { "MIX" }, exportButton { "Export" }, stopButton { "Stop" };
    juce::Label state, empty;
    double progress = -1;
    Lanes lanes { *this };
    juce::Viewport lanesView;
    Mixer mixer { *this };
    int view = 0;

    ui::Worker worker;
    juce::String activeKind;
    juce::var candidate;
    int enhancedRow = -1;
    juce::File session;
    std::vector<std::vector<StemRow>> undoStack, redoStack;
    std::shared_ptr<sample::Audio> clipboard;
    double clipboardRate = 0;
    juce::String prompt = "Improve clarity and definition of this isolated stem; preserve its role, rhythm and timing. Do not add other instruments.";
    bool vocal = false;
    juce::File lastExportFolder;
    sample::ExportChoice lastChoice;

    juce::AudioFormatManager formats;
    tracks::Mixer mix { formats };
    juce::AudioTransportSource transport;
    double playFrom = 0;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::ThreadPool peakPool { 2 };
};
} // namespace aicore::stems
