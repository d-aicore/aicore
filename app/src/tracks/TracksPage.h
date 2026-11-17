// AICore-C — EDIT > TRACKS (old AICORE multitrack.py TracksPage): the tracks, the row under them (chosen clip,
// clip gain, fades, last message) and the CHAIN · MASTER dock; its tools (Import audio…, Split, −, +, Options,
// Export the mix…) sit in the section row, as in the old app.
#pragma once

#include "ArrangeView.h"
#include "ChainPanel.h"
#include "../ui/AicoreTheme.h"
#include "../ui/Page.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace aicore::tracks
{
class NumberField : public juce::TextEditor
{
public:
    NumberField (double lo, double hi, int decimals) : lo_ (lo), hi_ (hi), decimals_ (decimals)
    {
        setJustification (juce::Justification::centred);
        setInputRestrictions (8, "-0123456789.");
        setFont (ui::uiFont (14.0f));
        onReturnKey = [this] { commit(); };
        onFocusLost = [this] { commit(); };
        setValue (0, false);
    }
    double value() const { return value_; }
    void setValue (double v, bool notify = true)
    {
        value_ = juce::jlimit (lo_, hi_, v);
        setText (juce::String (value_, decimals_), false);
        if (notify && onValue)
            onValue (value_);
    }
    std::function<void (double)> onValue;

private:
    void commit() { setValue (getText().getDoubleValue()); }
    double lo_, hi_, value_ = 0;
    int decimals_;
};

class TracksPage : public ui::Page, private juce::Timer, private juce::ScrollBar::Listener
{
public:
    TracksPage();
    ~TracksPage() override;

    /** The tools for the section row (the old app puts them beside TRACKS SAMPLE STEMS VOICE MIDI). */
    juce::Component& toolbar() { return tools; }
    juce::Component& exportButtonComponent() { return exportButton; }
    juce::Component* sectionTools() override { return &tools; }
    juce::Component* sectionRight() override { return &exportButton; }
    bool hasTransport() const override { return true; }

    void importAudio();
    juce::StringArray addFiles (const juce::StringArray& paths, int track = -1, double at = -1);
    void split (std::optional<double> at = {});
    void exportMix (const juce::File& target = {});
    void undo();
    void redo();
    void deleteSelection();
    void duplicate();
    void addTrack();
    /** Opens the CHAIN dock on the master (track < 0) or on a track, as its FX button does. */
    void openChain (int track);
    ChainPanel& chainPanel() { return chain; }

    // transport (the window's top bar drives these)
    void togglePlay() override;
    void stop() override;
    void goStart() override;
    bool isPlaying() const override { return transport.isPlaying(); }
    juce::String clock() const override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    ArrangeView view;

private:
    void timerCallback() override;
    void scrollBarMoved (juce::ScrollBar*, double start) override { if (! syncing) view.setView (start); }
    void commit (const std::optional<Session>& before, const juce::String& message = {});
    void apply (std::function<void (Session&)> fn, const juce::String& message = {});
    void refresh();
    void selectionChanged();
    void clipValues();
    void showOptions();
    void contextMenu (const juce::MouseEvent&, int track, Clip*);
    void ensureDevice();
    void restartPlayerAt (double seconds);

    Session session;
    std::vector<Session> undoStack, redoStack;
    juce::AudioFormatManager formats;
    Mixer mixer { formats };
    juce::AudioTransportSource transport;
    double playStart = 0;

    struct Tools : juce::Component
    {
        void resized() override;
        juce::OwnedArray<ui::AButton> buttons;
    } tools;
    ui::AButton exportButton { juce::String::fromUTF8 ("Export the mix\xe2\x80\xa6") };
    juce::ScrollBar hscroll { false };
    juce::Label clipLabel, state, gainLabel, fadeInLabel, fadeOutLabel;
    NumberField clipGain { -60, 12, 1 }, fadeIn { 0, 60, 2 }, fadeOut { 0, 60, 2 };
    bool syncing = false;

    // CHAIN · MASTER: minimised to a bar at first, like the old SideDock (▴ ▢ ×)
    ui::AButton dockUp { juce::String::fromUTF8 ("\xe2\x96\xb4") }, dockMax { juce::String::fromUTF8 ("\xe2\x96\xa2") },
        dockClose { juce::String::fromUTF8 ("\xc3\x97") };
    bool dockOpen = false;
    ChainPanel chain;
    bool chainMaster = true;
    int chainTrack = 0;
    void showChain();
    juce::String chainTitle() const;
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace aicore::tracks
