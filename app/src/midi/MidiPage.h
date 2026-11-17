// AICore-C — EDIT > MIDI (the old AICORE's midi_gui.py): a Guitar Pro file becomes MIDI, one .mid per track,
// the notes in place (tempo, repeats, tremolos). The reading and writing is AICore's guitar_pro.py, run by
// services/gp/aicore_gp_tool.py in AICore's own Python; this page shows it.
#pragma once

#include "../sample/SamplePage.h"
#include "../ui/Page.h"

namespace aicore::midi
{
class MidiPage : public ui::Page, public juce::FileDragAndDropTarget, private juce::TableListBoxModel
{
public:
    MidiPage();
    void openFile (const juce::String& path = {});
    void exportMidi();

    void paint (juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag (const juce::StringArray& f) override;
    void filesDropped (const juce::StringArray& f, int, int) override { openFile (f[0]); }

    /** Runs the tool; the one JSON line it prints (or {ok:false,error}). */
    static juce::var runTool (const juce::StringArray& args);

private:
    int getNumRows() override { return (int) rows.size(); }
    void paintRowBackground (juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell (juce::Graphics&, int row, int column, int w, int h, bool selected) override;
    void cellClicked (int row, int column, const juce::MouseEvent&) override;

    struct Row { juce::String name, kind; int strings = 0, notes = 0; bool chosen = true; };
    std::vector<Row> rows;
    juce::String songPath;
    juce::TextEditor source, out;
    sample::IconButton pickSource { sample::IconButton::folder }, pickOut { sample::IconButton::folder };
    juce::Label info, state;
    juce::TableListBox table { {}, this };
    ui::AButton create { "Create MIDI" }, openFolder { "Open folder" };
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace aicore::midi
