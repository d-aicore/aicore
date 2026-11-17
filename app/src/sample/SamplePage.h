// AICore-C — EDIT > SAMPLE (the old AICORE's sample_editor.py): one edit per tab ("+ New edit"), each with the
// song field, the overview and the wave, the edits (Keep selection, Cut, Normalize, Mastering, Undo), Export and
// Selection → new edit, and the SAMPLE inspector (name, what it is, one-shot / loop, BPM, bars, fades, curve,
// key, Save sample). Mastering opens the chain (INSERTS · MASTER) at the right.
#pragma once

#include "SamplePlayer.h"
#include "WaveView.h"
#include "../tracks/ChainPanel.h"
#include "../ui/Page.h"

namespace aicore::sample
{
class IconButton : public juce::Button
{
public:
    enum Kind { folder, end, loop, dots, close };
    IconButton (Kind k, bool dark = true) : juce::Button ({}), kind (k), darkStyle (dark) { setWantsKeyboardFocus (false); }
    void paintButton (juce::Graphics&, bool over, bool down) override;

private:
    Kind kind;
    bool darkStyle;
};

class SampleEditor : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    SampleEditor();
    ~SampleEditor() override;

    void openFile (const juce::String& path);       // in this edit (the tabs decide which edit)
    void loadAudio (Audio audio, double rate, const juce::String& source); // from a selection of another edit
    bool hasSound() const { return wave.frames() > 0 || sourcePath.isNotEmpty(); }
    juce::String sourcePath;
    juce::String title() const;

    std::function<void (const juce::String& path)> onOpenRequest;  // the tabs open it (in a new edit when this one is used)
    std::function<void (Audio, double, juce::String)> onSelectionEdit;
    std::function<void()> onClose, onTitleChanged, onTransportChanged;

    // the window's top bar
    void togglePlay();
    void stopPlayback();
    void goStart();
    void goEnd();
    bool isPlaying() const { return player.playing(); }
    juce::String clock() const;

    // edits
    void apply (std::function<Audio (const Audio&, int, int)> fn, const juce::String& which = {});
    void undo();
    void redo();
    void trim();
    void copy();
    void cutToClipboard();
    void paste();
    void selectAll();
    void selectBars();
    void revert();
    void exportFile (std::optional<ExportChoice> choice = {}, std::optional<juce::File> path = {});
    void save();
    void showMaster (bool on);
    void newFromSelection();
    void setLoopKind() { kind.setSelectedItemIndex (1); }

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int, int) override;

    WaveView wave;
    juce::Label state;

private:
    void timerCallback() override;
    void loaded (std::shared_ptr<Audio> audio, double rate, juce::String error, int token);
    void push (std::shared_ptr<const Audio> old, std::shared_ptr<const Audio> fresh);
    void setAudio (std::shared_ptr<const Audio> a, bool keepView);
    void enable (bool on = true);
    void showSelection();
    void kindChanged();
    void fadesFromWave();
    void fadeKnobsChanged();
    void masterChanged();
    void options();
    void playFrom (int frame);
    Audio mastered (const Audio& part) const;

    Overview overview { wave };
    juce::TextEditor source;
    IconButton folderButton { IconButton::folder }, endButton { IconButton::end }, loopButton { IconButton::loop };
    IconButton dotsButton { IconButton::dots, false }, closeButton { IconButton::close, false };
    ui::AButton zoomOut { juce::String::fromUTF8 ("\xe2\x88\x92") }, zoomIn { "+" }, zoomSel { "Selection" }, zoomAll { "All" };
    juce::Label info;
    ui::AButton keepButton { "Keep selection" }, cutButton { "Cut" }, normalizeButton { "Normalize" }, masterButton { "Mastering" },
        undoButton { "Undo" }, exportButton { juce::String::fromUTF8 ("Export audio\xe2\x80\xa6") },
        selectionButton { juce::String::fromUTF8 ("Selection \xe2\x86\x92 new edit") };
    juce::Rectangle<int> centreBox, inspectorBox;

    // the inspector
    juce::TextEditor name, key;
    juce::Label whatLabel;
    juce::ComboBox role, kind, curve;
    tracks::Knob bpm { "BPM", "", 40, 300, 1, 145 }, bars { "Bars", "", 0.25, 64, 2, 1 };
    tracks::Knob fadeInKnob { "Fade in ms", "", 0, 10000, 0, 0 }, fadeOutKnob { "Fade out ms", "", 0, 10000, 0, 0 };
    ui::AButton barsButton { "Select bars" }, saveButton { "Save sample in Core" };
    tracks::ChainPanel master;

    SamplePlayer player;
    std::vector<std::shared_ptr<const Audio>> undoStack, redoStack;
    std::shared_ptr<Audio> clipboard;
    int loadToken = 0, playStarted = -1;
    bool looping = false, syncing = false, busy = false;
    ExportChoice exportChoice;
    std::unique_ptr<juce::FileChooser> chooser;
};

class SamplePage : public ui::Page
{
public:
    SamplePage();
    SampleEditor* current() const;
    SampleEditor& newEdit();
    void openFile (const juce::String& path);
    void closeEdit (SampleEditor*);

    bool hasTransport() const override { return true; }
    void togglePlay() override { if (auto* e = current()) e->togglePlay(); }
    void stop() override { if (auto* e = current()) e->stopPlayback(); }
    void goStart() override { if (auto* e = current()) e->goStart(); }
    bool isPlaying() const override { auto* e = current(); return e != nullptr && e->isPlaying(); }
    juce::String clock() const override { auto* e = current(); return e != nullptr ? e->clock() : Page::clock(); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void select (int index);
    void refreshTabs();
    juce::OwnedArray<SampleEditor> edits;
    juce::OwnedArray<ui::AButton> tabs;
    ui::AButton newButton { "+ New edit" };
    int currentIndex = 0;
};
} // namespace aicore::sample
