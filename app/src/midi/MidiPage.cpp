#include "MidiPage.h"
#include "../ui/I18n.h"

#include "../ServiceJob.h"
#include "../ui/AppSettings.h"
#include "../ui/Worker.h"

namespace aicore::midi
{
using namespace aicore::ui;

namespace
{
const juce::StringArray extensions { ".gp3", ".gp4", ".gp5", ".gpx", ".gp" };
juce::String kindName (const juce::String& k)
{
    return k == "guitar" ? "Guitar" : k == "bass" ? "Bass" : k == "drums" ? "Drums" : "Other";
}

class TableLook : public AicoreLookAndFeel
{
public:
    void drawTableHeaderBackground (juce::Graphics& g, juce::TableHeaderComponent& h) override
    {
        g.fillAll (col::field);
        g.setColour (col::outline);
        g.fillRect (4, h.getHeight() - 1, h.getWidth() - 8, 1);
    }
    void drawTableHeaderColumn (juce::Graphics& g, juce::TableHeaderComponent&, const juce::String& name, int, int w, int h, bool, bool, int) override
    {
        g.setColour (col::text);
        g.setFont (uiFont (15.0f, true));
        g.drawText (name, 0, 0, w, h, juce::Justification::centred);
    }
};
TableLook& tableLook()
{
    static TableLook look;
    return look;
}
} // namespace

juce::var MidiPage::runTool (const juce::StringArray& args)
{
    auto tool = servicesRoot().getChildFile ("services").getChildFile ("gp").getChildFile ("aicore_gp_tool.py");
    juce::StringArray cmd { Worker::python().getFullPathName(), "-I", tool.getFullPathName() };
    cmd.addArray (args);
    juce::ChildProcess child;
    if (! child.start (cmd, juce::ChildProcess::wantStdOut))
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("ok", false);
        o->setProperty ("error", "AICore's Python could not run.");
        return juce::var (o);
    }
    const auto text = child.readAllProcessOutput();
    child.waitForProcessToFinish (60000);
    juce::var v;
    for (auto& line : juce::StringArray::fromLines (text))
        if (line.trim().startsWithChar ('{') && juce::JSON::parse (line.trim(), v).wasOk())
            return v;
    auto* o = new juce::DynamicObject();
    o->setProperty ("ok", false);
    o->setProperty ("error", text.trim().isNotEmpty() ? text.trim().getLastCharacters (300) : juce::String ("No answer from the Guitar Pro reader."));
    return juce::var (o);
}

MidiPage::MidiPage()
{
    for (auto* t : { &source, &out })
    {
        t->setFont (uiFont (15.0f));
        t->setIndents (8, 5);
        addAndMakeVisible (t);
    }
    source.setTextToShowWhenEmpty ("Guitar Pro file (.gp3 .gp4 .gp5 .gpx .gp)", col::dim);
    source.onReturnKey = [this] { openFile(); };
    source.onFocusLost = [this] { openFile(); };
    source.onTextChange = [this] {
        juce::File f (source.getText().trim().unquoted());
        if (extensions.contains (f.getFileExtension().toLowerCase()) && f.existsAsFile())
            openFile();
    };
    out.setTextToShowWhenEmpty ("Folder for the MIDI files", col::dim);
    pickSource.setTooltip ("Choose a Guitar Pro file");
    pickSource.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser> ("Choose a Guitar Pro file", juce::File (source.getText()), "*.gp3;*.gp4;*.gp5;*.gpx;*.gp");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc) {
            if (fc.getResult() != juce::File())
                openFile (fc.getResult().getFullPathName());
        });
    };
    pickOut.setTooltip ("Choose the folder for the MIDI files");
    pickOut.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser> ("Choose the folder for the MIDI files", juce::File (out.getText()));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc) {
            if (fc.getResult() != juce::File())
                out.setText (fc.getResult().getFullPathName());
        });
    };
    for (auto* l : { &info, &state })
    {
        l->setFont (uiFont (15.0f));
        l->setColour (juce::Label::textColourId, col::text);
        addAndMakeVisible (l);
    }
    state.setText ("Open a Guitar Pro file: one MIDI per track, with the tempo, repeats and tremolos in place.", juce::dontSendNotification);
    auto& h = table.getHeader();
    h.addColumn ("Track", 1, 400, 100, -1, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Instrument", 2, 96, 60, -1, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Strings", 3, 64, 50, -1, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Notes", 4, 60, 50, -1, juce::TableHeaderComponent::notSortable);
    h.setStretchToFitActive (false); // Track takes the room, the others their content (as the old ResizeToContents)
    h.setPopupMenuActive (false);
    h.setLookAndFeel (&tableLook());
    table.setHeaderHeight (44);
    table.setRowHeight (30);
    table.setColour (juce::ListBox::backgroundColourId, col::field);
    table.setColour (juce::ListBox::outlineColourId, col::outline);
    table.setOutlineThickness (1);
    addAndMakeVisible (table);
    create.setEnabled (false);
    create.onClick = [this] { exportMidi(); };
    openFolder.onClick = [this] { juce::File (out.getText()).startAsProcess(); };
    for (auto* b : { &create, &openFolder })
        addChildComponent (b);
    create.setVisible (true);
    addAndMakeVisible (pickSource);
    addAndMakeVisible (pickOut);
}

bool MidiPage::isInterestedInFileDrag (const juce::StringArray& f)
{
    return ! f.isEmpty() && extensions.contains (juce::File (f[0]).getFileExtension().toLowerCase());
}

void MidiPage::openFile (const juce::String& raw)
{
    juce::File path (raw.isNotEmpty() ? raw : source.getText().trim().unquoted());
    if (songPath == path.getFullPathName() && ! rows.empty())
        return;
    if (! path.existsAsFile())
        return;
    if (! extensions.contains (path.getFileExtension().toLowerCase()))
    {
        state.setText ("This is not a Guitar Pro file.", juce::dontSendNotification);
        return;
    }
    auto v = runTool ({ "summary", path.getFullPathName() });
    if (! (bool) v["ok"])
    {
        rows.clear();
        table.updateContent();
        create.setEnabled (false);
        state.setText ("Could not read this file: " + v["error"].toString(), juce::dontSendNotification);
        return;
    }
    songPath = path.getFullPathName();
    if (source.getText() != songPath)
        source.setText (songPath, false);
    out.setText (v["outFolder"].toString(), false);
    info.setText (v["title"].toString().toUpperCase() + juce::String::fromUTF8 ("  \xc2\xb7  ") + (std::abs ((double) v["tempo"] - std::round ((double) v["tempo"])) < 1e-9 ? juce::String ((int) std::round ((double) v["tempo"])) : juce::String ((double) v["tempo"], 1))
                      + juce::String::fromUTF8 (" BPM  \xc2\xb7  ") + v["bars"].toString() + " bars",
                  juce::dontSendNotification);
    rows.clear();
    if (auto* a = v["rows"].getArray())
        for (auto& r : *a)
            rows.push_back ({ r["name"].toString(), r["kind"].toString(), (int) r["strings"], (int) r["notes"], (int) r["notes"] > 0 });
    table.updateContent();
    table.repaint();
    create.setEnabled (! rows.empty());
    openFolder.setVisible (false);
    state.setText (juce::String (rows.size()) + " tracks. Untick the ones you do not want and press Create MIDI.", juce::dontSendNotification);
    resized();
}

void MidiPage::exportMidi()
{
    if (songPath.isEmpty())
        return;
    juce::StringArray chosen;
    for (int i = 0; i < (int) rows.size(); ++i)
        if (rows[(size_t) i].chosen)
            chosen.add (juce::String (i));
    if (chosen.isEmpty())
    {
        state.setText ("Tick at least one track.", juce::dontSendNotification);
        return;
    }
    auto v = runTool ({ "export", songPath, out.getText().trim(), chosen.joinIntoString (",") });
    if (! (bool) v["ok"])
    {
        state.setText ("Could not write the MIDI: " + v["error"].toString(), juce::dontSendNotification);
        return;
    }
    openFolder.setVisible (true);
    const int n = v["files"].getArray() != nullptr ? v["files"].getArray()->size() : 0;
    state.setText (juce::String (n) + " MIDI files in " + out.getText() + ". In Core: File > Import MIDI, or drag a file onto a track.",
                   juce::dontSendNotification);
    resized();
}

void MidiPage::paintRowBackground (juce::Graphics& g, int, int w, int h, bool)
{
    g.setColour (col::grid);
    g.fillRect (4, h - 1, w - 8, 1);
}

void MidiPage::paintCell (juce::Graphics& g, int row, int column, int w, int h, bool)
{
    if (row < 0 || row >= (int) rows.size())
        return;
    const auto& r = rows[(size_t) row];
    g.setColour (col::text);
    g.setFont (uiFont (15.0f));
    if (column == 1)
    {
        juce::Rectangle<float> box (14.0f, h / 2.0f - 6, 12, 12);
        g.setColour (col::field);
        g.fillRect (box);
        g.setColour (col::muted);
        g.drawRect (box, 1.0f);
        if (r.chosen)
        {
            juce::Path tick;
            tick.startNewSubPath (box.getX() + 2.5f, box.getCentreY());
            tick.lineTo (box.getX() + 5, box.getBottom() - 2.5f);
            tick.lineTo (box.getRight() - 2, box.getY() + 2);
            g.setColour (col::text);
            g.strokePath (tick, juce::PathStrokeType (1.5f));
        }
        g.setColour (col::text);
        g.drawText (r.name, 33, 0, w - 36, h, juce::Justification::centredLeft, true);
        return;
    }
    const auto text = column == 2 ? kindName (r.kind) : column == 3 ? (r.strings > 0 ? juce::String (r.strings) : juce::String()) : juce::String (r.notes);
    g.drawText (text, 0, 0, w, h, juce::Justification::centred);
}

void MidiPage::cellClicked (int row, int column, const juce::MouseEvent&)
{
    if (column == 1 && row >= 0 && row < (int) rows.size())
    {
        rows[(size_t) row].chosen = ! rows[(size_t) row].chosen;
        table.repaintRow (row);
    }
}

void MidiPage::paint (juce::Graphics&) {}

void MidiPage::resized()
{
    auto r = getLocalBounds().withTrimmedLeft (9).withTrimmedRight (9).withTrimmedTop (6).withTrimmedBottom (8);
    auto top = r.removeFromTop (28);
    pickSource.setBounds (top.removeFromRight (36));
    top.removeFromRight (6);
    source.setBounds (top);
    r.removeFromTop (4);
    info.setBounds (r.removeFromTop (24).withTrimmedLeft (-4));
    r.removeFromTop (6);
    state.setBounds (r.removeFromBottom (22).withTrimmedLeft (-4));
    r.removeFromBottom (8);
    auto bottom = r.removeFromBottom (28);
    if (openFolder.isVisible())
    {
        openFolder.setBounds (bottom.removeFromRight (openFolder.idealWidth (26)));
        bottom.removeFromRight (6);
    }
    create.setBounds (bottom.removeFromRight (create.idealWidth (26)));
    bottom.removeFromRight (6);
    pickOut.setBounds (bottom.removeFromRight (36));
    bottom.removeFromRight (6);
    out.setBounds (bottom);
    r.removeFromBottom (8);
    table.setBounds (r);
    table.getHeader().setColumnWidth (1, juce::jmax (100, r.getWidth() - 2 - 96 - 64 - 60 - 10));
}
} // namespace aicore::midi
