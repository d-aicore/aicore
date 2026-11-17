#include "SamplePage.h"
#include "../ui/I18n.h"

#include "../ui/AppSettings.h"
#include "../ui/Keybinds.h"

#include <cmath>

namespace aicore::sample
{
using namespace aicore::ui;

namespace
{
constexpr size_t maxUndo = 30;

juce::String clockText (double seconds)
{
    seconds = juce::jmax (0.0, seconds);
    const int m = (int) (seconds / 60);
    return juce::String (m) + ":" + juce::String (seconds - m * 60, 3).paddedLeft ('0', 6);
}

void styleField (juce::TextEditor& t, const juce::String& placeholder)
{
    t.setFont (uiFont (15.0f));
    t.setTextToShowWhenEmpty (placeholder, col::dim);
    t.setIndents (8, 5);
}

void styleCombo (juce::ComboBox& c)
{
    c.setColour (juce::ComboBox::backgroundColourId, col::field);
    c.setColour (juce::ComboBox::textColourId, col::text);
    c.setColour (juce::ComboBox::outlineColourId, col::outline);
}

/** Export audio: the quality before the file name (format, bits, sample rate, mono / stereo). */
class ExportDialog : public juce::Component
{
public:
    ExportDialog (double seconds, int channels, double rate, bool selection, const ExportChoice& last)
    {
        summary.setText ((selection ? "The selection" : "The whole sound") + juce::String (": ") + juce::String (seconds, 2) + " s, "
                             + juce::String (channels) + (channels == 1 ? " channel" : " channels") + ", " + juce::String (rate / 1000, 3).trimCharactersAtEnd ("0").trimCharactersAtEnd (".")
                             + " kHz.",
                         juce::dontSendNotification);
        for (auto* l : { &summary, &fl, &bl, &rl, &cl })
        {
            l->setFont (uiFont (15.0f));
            l->setColour (juce::Label::textColourId, col::text);
            addAndMakeVisible (l);
        }
        fl.setText ("Format:", juce::dontSendNotification);
        bl.setText ("Bits:", juce::dontSendNotification);
        rl.setText ("Sample rate:", juce::dontSendNotification);
        cl.setText ("Channels:", juce::dontSendNotification);
        format.addItemList ({ "WAV (*.wav)", "FLAC (*.flac)", "MP3 (*.mp3)" }, 1);
        const juce::String khz = juce::String (rate / 1000, 3).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
        rateBox.addItem ("Same as the sound (" + khz + " kHz)", 1);
        int id = 2;
        for (int r : { 44100, 48000, 88200, 96000 })
            rateBox.addItem (juce::String (r / 1000.0, 1).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " kHz", id++);
        channelsBox.addItemList ({ "Stereo (as it is)", "Mono (left + right in one channel)" }, 1);
        for (auto* c : { &format, &bits, &rateBox, &channelsBox })
        {
            styleCombo (*c);
            addAndMakeVisible (c);
        }
        format.onChange = [this] { fillBits(); };
        format.setSelectedItemIndex (last.format == "flac" ? 1 : last.format == "mp3" ? 2 : 0, juce::dontSendNotification);
        fillBits (last.bits);
        const int rates[] = { 0, 44100, 48000, 88200, 96000 };
        for (int i = 0; i < 5; ++i)
            if (rates[i] == last.rate)
                rateBox.setSelectedItemIndex (i, juce::dontSendNotification);
        if (rateBox.getSelectedItemIndex() < 0)
            rateBox.setSelectedItemIndex (0, juce::dontSendNotification);
        channelsBox.setSelectedItemIndex (last.mono ? 1 : 0, juce::dontSendNotification);
        ok.onClick = [this] { finish (true); };
        cancel.onClick = [this] { finish (false); };
        addAndMakeVisible (ok);
        addAndMakeVisible (cancel);
        setSize (460, 250);
    }
    std::function<void (std::optional<ExportChoice>)> onDone;

    void paint (juce::Graphics& g) override { g.fillAll (col::card); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        summary.setBounds (r.removeFromTop (26));
        r.removeFromTop (8);
        for (auto [l, c] : { std::pair<juce::Label*, juce::ComboBox*> { &fl, &format }, { &bl, &bits }, { &rl, &rateBox }, { &cl, &channelsBox } })
        {
            auto row = r.removeFromTop (28);
            l->setBounds (row.removeFromLeft (110));
            c->setBounds (row);
            r.removeFromTop (6);
        }
        auto row = r.removeFromBottom (30);
        cancel.setBounds (row.removeFromRight (90));
        row.removeFromRight (8);
        ok.setBounds (row.removeFromRight (100));
    }

private:
    void fillBits (juce::String keep = {})
    {
        if (keep.isEmpty())
            keep = bitKeys[juce::jmax (0, bits.getSelectedItemIndex())];
        bits.clear (juce::dontSendNotification);
        bitKeys.clear();
        const int f = format.getSelectedItemIndex();
        juce::StringArray keys = f == 0 ? juce::StringArray { "16", "24", "32f" } : f == 1 ? juce::StringArray { "16", "24" } : juce::StringArray {};
        for (auto& k : keys)
        {
            bits.addItem (k == "16" ? "16-bit" : k == "24" ? "24-bit" : "32-bit float", bits.getNumItems() + 1);
            bitKeys.add (k);
        }
        if (keys.isEmpty())
        {
            bits.addItem ("320 kbps", 1);
            bitKeys.add ("");
        }
        const int at = bitKeys.indexOf (keys.contains (keep) ? keep : juce::String ("24"));
        bits.setSelectedItemIndex (juce::jmax (0, at), juce::dontSendNotification);
        bits.setEnabled (bits.getNumItems() > 1);
    }
    void finish (bool accepted)
    {
        std::optional<ExportChoice> c;
        if (accepted)
        {
            ExportChoice e;
            const int f = format.getSelectedItemIndex();
            e.format = f == 1 ? "flac" : f == 2 ? "mp3" : "wav";
            e.bits = bitKeys[juce::jmax (0, bits.getSelectedItemIndex())];
            const int rates[] = { 0, 44100, 48000, 88200, 96000 };
            e.rate = rates[juce::jlimit (0, 4, rateBox.getSelectedItemIndex())];
            e.mono = channelsBox.getSelectedItemIndex() == 1;
            c = e;
        }
        auto cb = onDone;
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState (0);
        if (cb)
            cb (c);
    }
    juce::Label summary, fl, bl, rl, cl;
    juce::ComboBox format, bits, rateBox, channelsBox;
    juce::StringArray bitKeys;
    AButton ok { juce::String::fromUTF8 ("Export\xe2\x80\xa6") }, cancel { "Cancel" };
};
} // namespace

//==============================================================================
void IconButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    juce::Colour ink;
    if (darkStyle)
    {
        if (! isEnabled())
            g.setColour (col::disabledFill), g.fillRoundedRectangle (r, 2.0f);
        else
        {
            const bool on = over || down || getToggleState();
            g.setGradientFill (on ? juce::ColourGradient (col::onTop, 0, r.getY(), col::onBottom, 0, r.getBottom(), false)
                                  : juce::ColourGradient (col::buttonTop, 0, r.getY(), col::buttonBottom, 0, r.getBottom(), false));
            g.fillRoundedRectangle (r, 2.0f);
        }
        g.setColour (! isEnabled() ? col::disabledEdge : over ? col::onEdge : col::edge);
        g.drawRoundedRectangle (r, 2.0f, 1.0f);
        ink = ! isEnabled() ? col::disabledText : over || down || getToggleState() ? col::onText : col::selectedText;
    }
    else
    {
        g.setColour (over ? col::hover : col::control);
        g.fillRoundedRectangle (r, 2.0f);
        ink = col::selectedText;
    }
    g.setColour (ink);
    const auto c = r.getCentre();
    switch (kind)
    {
        case folder:
        {
            juce::Path p;
            p.startNewSubPath (c.x - 7, c.y - 4);
            p.lineTo (c.x - 7, c.y + 5);
            p.lineTo (c.x + 7, c.y + 5);
            p.lineTo (c.x + 7, c.y - 2);
            p.lineTo (c.x - 1, c.y - 2);
            p.lineTo (c.x - 3, c.y - 5);
            p.lineTo (c.x - 7, c.y - 5);
            p.closeSubPath();
            g.strokePath (p, juce::PathStrokeType (1.3f));
            break;
        }
        case end:
        {
            juce::Path p;
            p.addTriangle (c.x - 6, c.y - 5, c.x - 6, c.y + 5, c.x + 2, c.y);
            g.fillPath (p);
            g.fillRect (c.x + 3, c.y - 5, 2.0f, 10.0f);
            break;
        }
        case loop:
        {
            juce::Path p;
            p.addRoundedRectangle (c.x - 7, c.y - 4, 14, 8, 4);
            g.strokePath (p, juce::PathStrokeType (1.5f));
            break;
        }
        case dots:
            for (int i = -1; i <= 1; ++i)
                g.fillEllipse (c.x + i * 4.0f - 1.2f, c.y - 1.2f, 2.4f, 2.4f);
            break;
        case close:
            g.drawLine (c.x - 5, c.y - 5, c.x + 5, c.y + 5, 1.4f);
            g.drawLine (c.x + 5, c.y - 5, c.x - 5, c.y + 5, 1.4f);
            break;
    }
}

//==============================================================================
SampleEditor::SampleEditor()
{
    setWantsKeyboardFocus (true);
    styleField (source, juce::String::fromUTF8 ("Song or sound to edit (WAV, MP3, FLAC\xe2\x80\xa6)"));
    source.onReturnKey = [this] { if (onOpenRequest) onOpenRequest (source.getText()); };
    folderButton.setTooltip ("Open a song");
    folderButton.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser> ("Open a song", juce::File (source.getText()), "*.wav;*.flac;*.mp3;*.ogg;*.aif;*.aiff");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc) {
            if (fc.getResult() != juce::File() && onOpenRequest)
                onOpenRequest (fc.getResult().getFullPathName());
        });
    };
    zoomOut.setTooltip ("Zoom out (Ctrl + wheel)");
    zoomIn.setTooltip ("Zoom in (Ctrl + wheel)");
    zoomSel.setTooltip ("Zoom to the selection");
    zoomAll.setTooltip ("Show the whole sound");
    zoomOut.onClick = [this] { wave.zoom (2.0); };
    zoomIn.onClick = [this] { wave.zoom (0.5); };
    zoomSel.onClick = [this] { if (wave.frames()) wave.zoomToSelection(); };
    zoomAll.onClick = [this] { wave.zoomAll(); };
    for (juce::Component* c : { (juce::Component*) &source, (juce::Component*) &folderButton, (juce::Component*) &zoomOut, (juce::Component*) &zoomIn,
                                (juce::Component*) &zoomSel, (juce::Component*) &zoomAll, (juce::Component*) &overview, (juce::Component*) &wave,
                                (juce::Component*) &endButton, (juce::Component*) &loopButton, (juce::Component*) &info, (juce::Component*) &dotsButton,
                                (juce::Component*) &closeButton })
        addAndMakeVisible (c);
    dotsButton.setTooltip ("More");
    dotsButton.onClick = [this] { options(); };
    closeButton.setTooltip ("Close edit");
    closeButton.onClick = [this] { if (onClose) onClose(); };
    endButton.setTooltip ("To the end (End)");
    endButton.onClick = [this] { goEnd(); };
    loopButton.setTooltip ("Loop the selection (L)");
    loopButton.setClickingTogglesState (true);
    loopButton.onClick = [this] { looping = loopButton.getToggleState(); };
    info.setFont (uiFont (15.0f));
    info.setColour (juce::Label::textColourId, col::muted);
    info.setText ("Drag on the wave to select. Ctrl + wheel zooms, the wheel scrolls, Space plays.", juce::dontSendNotification);

    keepButton.setTooltip ("Keep only the selected part; the rest goes.");
    cutButton.setTooltip ("Take the selected part out and join the rest.");
    normalizeButton.setTooltip ("The loudest point of the selection goes to -1 dB.");
    keepButton.onClick = [this] { apply ([] (const Audio& a, int x, int y) { return keep (a, x, y); }, "keep"); };
    cutButton.onClick = [this] { apply ([] (const Audio& a, int x, int y) { return cut (a, x, y); }, "cut"); };
    normalizeButton.onClick = [this] { apply ([] (const Audio& a, int x, int y) { return normalize (a, x, y); }); };
    masterButton.setClickingTogglesState (false);
    masterButton.setTooltip ("The master chain of this sound: volume, EQ, high-pass, compressor, LUFS target, limiter and fades. "
                             "Heard when it plays, applied when it is exported or saved.");
    masterButton.onClick = [this] { showMaster (! master.isVisible()); };
    undoButton.onClick = [this] { undo(); };
    exportButton.setTooltip ("Write the selection (or the whole sound) to a file: WAV, FLAC or MP3, bits, sample rate, mono or stereo. "
                             "The original song is never changed.");
    exportButton.onClick = [this] { exportFile(); };
    selectionButton.onClick = [this] { newFromSelection(); };
    for (auto* b : { &keepButton, &cutButton, &normalizeButton, &masterButton, &undoButton, &exportButton, &selectionButton })
        addAndMakeVisible (b);
    state.setFont (uiFont (15.0f));
    state.setColour (juce::Label::textColourId, col::text);
    state.setText ("Ready.", juce::dontSendNotification);
    addAndMakeVisible (state);

    // the inspector
    styleField (name, "Name (optional)");
    styleField (key, "Key (optional: F#m, C, E1)");
    whatLabel.setText ("What it is", juce::dontSendNotification);
    whatLabel.setFont (uiFont (15.0f));
    whatLabel.setColour (juce::Label::textColourId, col::text);
    for (auto& r : kRoles)
        role.addItem (r.folder, role.getNumItems() + 1);
    role.setSelectedItemIndex (18, juce::dontSendNotification); // Other
    kind.addItemList ({ "One-shot", "Loop" }, 1);
    kind.setSelectedItemIndex (0, juce::dontSendNotification);
    kind.onChange = [this] { kindChanged(); showSelection(); };
    curve.addItemList ({ "Smooth", "Linear", "Exponential" }, 1);
    curve.setSelectedItemIndex (0, juce::dontSendNotification);
    curve.setTooltip ("Curve");
    curve.onChange = [this] { fadeKnobsChanged(); };
    for (auto* c : { &role, &kind, &curve })
        styleCombo (*c);
    barsButton.setTooltip ("Loop mode: select the number of bars at this BPM, starting at the selection or playhead. One bar at 120 BPM is 2 seconds (4/4).");
    barsButton.onClick = [this] { selectBars(); };
    fadeInKnob.setTooltip ("How long the start rises from silence. Or drag the handle at the top left of the wave.");
    fadeOutKnob.setTooltip ("How long the end falls to silence. Or drag the handle at the top right of the wave.");
    fadeInKnob.onChange = [this] (double) { fadeKnobsChanged(); };
    fadeOutKnob.onChange = [this] (double) { fadeKnobsChanged(); };
    bpm.onChange = [this] (double) { showSelection(); };
    saveButton.onClick = [this] { save(); };
    for (juce::Component* c : { (juce::Component*) &name, (juce::Component*) &whatLabel, (juce::Component*) &role, (juce::Component*) &kind,
                                (juce::Component*) &bpm, (juce::Component*) &bars, (juce::Component*) &barsButton, (juce::Component*) &fadeInKnob,
                                (juce::Component*) &fadeOutKnob, (juce::Component*) &curve, (juce::Component*) &key, (juce::Component*) &saveButton })
        addAndMakeVisible (c);
    master.setVertical (juce::String::fromUTF8 ("INSERTS \xc2\xb7 MASTER"));
    master.showSettings ({}, true);
    master.onChanged = [this] (const tracks::ChainSettings&) { masterChanged(); };
    addChildComponent (master);

    wave.onSelectionChanged = [this] { showSelection(); overview.repaint(); };
    wave.onFadesChanged = [this] { fadesFromWave(); };
    wave.onViewChanged = [this] { overview.repaint(); };
    kindChanged();
    enable();
}

SampleEditor::~SampleEditor()
{
    stopTimer();
    player.stop();
}

juce::String SampleEditor::title() const
{
    auto t = sourcePath.isNotEmpty() ? juce::File (sourcePath).getFileNameWithoutExtension() : juce::String ("New edit");
    return t.substring (0, 30);
}

void SampleEditor::openFile (const juce::String& raw)
{
    auto path = raw.trim().unquoted();
    ++loadToken;
    if (path.isEmpty())
        return;
    if (! juce::File (path).existsAsFile())
    {
        state.setText ("File not found: " + path, juce::dontSendNotification);
        return;
    }
    player.stop();
    source.setText (path, false);
    sourcePath = path;
    state.setText (juce::String::fromUTF8 ("Opening\xe2\x80\xa6"), juce::dontSendNotification);
    enable (false);
    const int token = loadToken;
    juce::Component::SafePointer<SampleEditor> safe (this);
    juce::Thread::launch ([safe, path, token] {
        auto audio = std::make_shared<Audio>();
        double rate = 0;
        juce::String error;
        load (juce::File (path), *audio, rate, error);
        juce::MessageManager::callAsync ([safe, audio, rate, error, token] {
            if (safe != nullptr)
                safe->loaded (audio, rate, error, token);
        });
    });
}

void SampleEditor::loaded (std::shared_ptr<Audio> audio, double rate, juce::String error, int token)
{
    if (token != loadToken)
        return; // another file was asked for meanwhile
    if (error.isNotEmpty())
    {
        state.setText (error, juce::dontSendNotification);
        enable();
        return;
    }
    undoStack.clear();
    redoStack.clear();
    wave.playhead = 0;
    wave.setAudio (audio, rate);
    if (name.getText().isEmpty())
        name.setTextToShowWhenEmpty (juce::File (sourcePath).getFileNameWithoutExtension().substring (0, 60), col::dim);
    state.setText ("Opened: " + juce::File (sourcePath).getFileName() + juce::String::fromUTF8 (" \xc2\xb7 ") + clockText (audio->getNumSamples() / rate),
                   juce::dontSendNotification);
    enable();
    if (onTitleChanged) onTitleChanged();
    if (onTransportChanged) onTransportChanged();
}

void SampleEditor::loadAudio (Audio audio, double rate, const juce::String& src)
{
    sourcePath = src;
    source.setText (src, false);
    loaded (std::make_shared<Audio> (std::move (audio)), rate, {}, loadToken);
}

void SampleEditor::filesDropped (const juce::StringArray& files, int, int)
{
    if (! files.isEmpty() && onOpenRequest)
        onOpenRequest (files[0]);
}

//------------------------------------------------------------------------------ edits
void SampleEditor::setAudio (std::shared_ptr<const Audio> a, bool keepView)
{
    wave.setAudio (std::move (a), wave.rate(), keepView);
}

void SampleEditor::push (std::shared_ptr<const Audio> old, std::shared_ptr<const Audio> fresh)
{
    player.stop();
    undoStack.push_back (std::move (old));
    if (undoStack.size() > maxUndo)
        undoStack.erase (undoStack.begin());
    redoStack.clear();
    const bool keepView = fresh->getNumSamples() == wave.frames();
    setAudio (std::move (fresh), keepView);
    enable();
}

void SampleEditor::apply (std::function<Audio (const Audio&, int, int)> fn, const juce::String& which)
{
    auto audio = wave.audioPtr();
    if (audio == nullptr)
    {
        state.setText ("Open a song first.", juce::dontSendNotification);
        return;
    }
    auto [a, b] = wave.selection();
    const bool had = wave.hasSelection();
    auto out = std::make_shared<const Audio> (fn (*audio, a, b));
    if (out->getNumSamples() == 0)
    {
        state.setText ("That would leave nothing. Select a part first.", juce::dontSendNotification);
        return;
    }
    push (audio, out);
    if (which == "keep")
        wave.setSelection (0, 0);
    else if (which == "cut")
    {
        wave.setSelection (0, 0);
        wave.playhead = juce::jmin (a, wave.frames());
    }
    else if (had)
        wave.setSelection (a, b);
    state.setText ("Done. Undo goes back.", juce::dontSendNotification);
}

void SampleEditor::trim()
{
    if (auto a = wave.audioPtr())
        push (a, std::make_shared<const Audio> (trimSilence (*a, wave.rate())));
}

void SampleEditor::undo()
{
    if (undoStack.empty())
        return;
    player.stop();
    redoStack.push_back (wave.audioPtr());
    auto a = undoStack.back();
    undoStack.pop_back();
    setAudio (a, true);
    enable();
}

void SampleEditor::redo()
{
    if (redoStack.empty())
        return;
    player.stop();
    undoStack.push_back (wave.audioPtr());
    auto a = redoStack.back();
    redoStack.pop_back();
    setAudio (a, true);
    enable();
}

void SampleEditor::revert()
{
    if (sourcePath.isNotEmpty())
        openFile (sourcePath);
}

void SampleEditor::selectBars()
{
    if (wave.frames() == 0)
        return;
    juce::String error;
    const int n = barsFrames (bpm.value(), bars.value(), wave.rate(), error);
    if (n < 0)
    {
        state.setText (error, juce::dontSendNotification);
        return;
    }
    const int a = wave.hasSelection() ? wave.sel.first : juce::jmax (0, wave.playhead);
    wave.setSelection (a, a + n);
}

void SampleEditor::copy()
{
    if (wave.frames() && wave.hasSelection())
    {
        clipboard = std::make_shared<Audio> (range (*wave.audio(), wave.sel.first, wave.sel.second));
        state.setText ("Copied. Paste puts it at the red line.", juce::dontSendNotification);
    }
}

void SampleEditor::cutToClipboard()
{
    if (wave.hasSelection())
    {
        copy();
        apply ([] (const Audio& a, int x, int y) { return cut (a, x, y); }, "cut");
    }
}

void SampleEditor::paste()
{
    if (wave.frames() == 0 || clipboard == nullptr || clipboard->getNumSamples() == 0)
        return;
    const int at = wave.hasSelection() ? wave.sel.first : juce::jlimit (0, wave.frames(), wave.playhead);
    push (wave.audioPtr(), std::make_shared<const Audio> (insert (*wave.audio(), at, *clipboard)));
    wave.setSelection (at, at + clipboard->getNumSamples());
}

void SampleEditor::selectAll()
{
    if (wave.frames())
        wave.setSelection (0, wave.frames());
}

void SampleEditor::newFromSelection()
{
    if (wave.frames() && wave.hasSelection() && onSelectionEdit)
    {
        int first = 0;
        onSelectionEdit (wave.rendered (first), wave.rate(), sourcePath);
    }
}

void SampleEditor::options()
{
    const bool has = wave.frames() > 0;
    juce::PopupMenu m;
    m.addSectionHeader ("File");
    m.addItem (juce::String::fromUTF8 ("Open a song\xe2\x80\xa6"), [this] { folderButton.onClick(); });
    m.addItem (wave.hasSelection() ? juce::String::fromUTF8 ("Export selection\xe2\x80\xa6") : juce::String::fromUTF8 ("Export audio\xe2\x80\xa6"), has, false, [this] { exportFile(); });
    m.addItem (juce::String::fromUTF8 ("Selection \xe2\x86\x92 new edit"), wave.hasSelection(), false, [this] { newFromSelection(); });
    m.addItem ("Open the saved samples folder", [this] {
        auto root = AppSettings::samplesRoot();
        if (root == juce::File())
        {
            state.setText ("The samples folder was not found. Choose it in Settings > Folders > Samples folder.", juce::dontSendNotification);
            return;
        }
        auto folder = root.getChildFile (kEditFolder);
        folder.createDirectory();
        folder.startAsProcess();
    });
    m.addSectionHeader ("Select");
    m.addItem ("Select all", has, false, [this] { selectAll(); });
    m.addItem ("Zoom to the selection", has, false, [this] { wave.zoomToSelection(); });
    m.addItem ("Show the whole sound", has, false, [this] { wave.zoomAll(); });
    m.addItem ("Cut at zero crossings (no clicks)", true, wave.snap, [this] { wave.snap = ! wave.snap; });
    m.addSectionHeader ("More edits");
    m.addItem ("Reverse", has, false, [this] { apply ([] (const Audio& a, int x, int y) { return reverse (a, x, y); }); });
    m.addItem ("Silence the selection", has, false, [this] { apply ([] (const Audio& a, int x, int y) { return silence (a, x, y); }); });
    m.addItem ("Louder (+3 dB)", has, false, [this] { apply ([] (const Audio& a, int x, int y) { return gainDb (a, x, y, 3.0); }); });
    m.addItem ("Quieter (-3 dB)", has, false, [this] { apply ([] (const Audio& a, int x, int y) { return gainDb (a, x, y, -3.0); }); });
    m.addItem ("Remove silence at the ends", has, false, [this] { trim(); });
    m.addItem ("Redo", ! redoStack.empty(), false, [this] { redo(); });
    m.addItem ("Back to the original", has && sourcePath.isNotEmpty(), false, [this] { revert(); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&dotsButton));
}

//------------------------------------------------------------------------------ play
void SampleEditor::togglePlay()
{
    if (player.playing())
    {
        const int at = player.position();
        player.stop();
        if (at >= 0)
            wave.playhead = at; // pause: the playhead stays where the sound was
        stopTimer();
        wave.repaint();
        if (onTransportChanged) onTransportChanged();
        return;
    }
    playFrom (wave.playhead);
}

void SampleEditor::playFrom (int frame)
{
    auto audio = wave.audioPtr();
    if (audio == nullptr)
        return;
    if (wave.fadeIn > 0 || wave.fadeOut > 0) // the fades as they will be saved
    {
        int first = 0;
        auto part = wave.rendered (first);
        auto copy = std::make_shared<Audio> (*audio);
        for (int ch = 0; ch < 2; ++ch)
            copy->copyFrom (ch, first, part, ch, 0, part.getNumSamples());
        audio = copy;
    }
    const int n = wave.frames();
    int a, b, start;
    if (wave.hasSelection())
    {
        a = wave.sel.first;
        b = wave.sel.second;
        start = looping ? a : (a <= frame && frame < b ? frame : a);
    }
    else
    {
        a = 0;
        b = n;
        start = 0 <= frame && frame < n ? frame : 0;
    }
    playStarted = start;
    player.play (audio, wave.rate(), a, b, start, looping && wave.hasSelection(), master.settings());
    startTimer (30);
    if (onTransportChanged) onTransportChanged();
}

void SampleEditor::stopPlayback()
{
    player.stop();
    stopTimer();
    wave.playhead = wave.hasSelection() ? wave.sel.first : juce::jmax (0, playStarted);
    wave.repaint();
    overview.repaint();
    if (onTransportChanged) onTransportChanged();
}

void SampleEditor::goStart()
{
    const bool was = player.playing();
    player.stop();
    wave.playhead = 0;
    wave.setView (0, wave.viewSpan);
    if (was)
        playFrom (0);
    if (onTransportChanged) onTransportChanged();
}

void SampleEditor::goEnd()
{
    player.stop();
    stopTimer();
    wave.playhead = wave.frames();
    wave.setView (wave.frames() - wave.viewSpan, wave.viewSpan);
    if (onTransportChanged) onTransportChanged();
}

void SampleEditor::timerCallback()
{
    if (! player.playing())
    {
        stopTimer();
        if (onTransportChanged) onTransportChanged();
        return;
    }
    const int f = player.position();
    if (f >= 0)
    {
        wave.playhead = f;
        if (f < wave.viewStart || f > wave.viewStart + wave.viewSpan) // follow the sound
            wave.setView (f - wave.viewSpan * 0.05, wave.viewSpan);
        wave.repaint();
        overview.repaint();
    }
    if (onTransportChanged) onTransportChanged();
}

juce::String SampleEditor::clock() const
{
    const double rate = wave.rate() > 0 ? wave.rate() : 44100;
    return clockText (juce::jmax (0, wave.playhead) / rate) + " / " + clockText (wave.frames() / rate);
}

//------------------------------------------------------------------------------ export / save / master
Audio SampleEditor::mastered (const Audio& part) const
{
    auto s = master.settings();
    if (! tracks::chainActive (s))
        return part;
    Audio out (part);
    tracks::renderChain (out, (int) wave.rate(), s, true);
    return out;
}

void SampleEditor::exportFile (std::optional<ExportChoice> choice, std::optional<juce::File> path)
{
    if (wave.frames() == 0)
    {
        state.setText ("Open a song first.", juce::dontSendNotification);
        return;
    }
    int first = 0;
    auto part = std::make_shared<Audio> (mastered (wave.rendered (first)));
    const double rate = wave.rate();
    if (! choice)
    {
        auto* dlg = new ExportDialog (part->getNumSamples() / rate, part->getNumChannels(), rate, wave.hasSelection(), exportChoice);
        juce::Component::SafePointer<SampleEditor> safe (this);
        dlg->onDone = [safe, path] (std::optional<ExportChoice> c) {
            if (safe != nullptr && c)
                juce::MessageManager::callAsync ([safe, c, path] { if (safe) safe->exportFile (c, path); });
        };
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (dlg);
        o.dialogTitle = "Export audio";
        o.dialogBackgroundColour = col::card;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
        return;
    }
    exportChoice = *choice;
    const auto ext = "." + choice->format;
    auto run = [this, part, rate, c = *choice] (const juce::File& file) {
        if (file == juce::File())
            return;
        ExportResult res;
        juce::String error;
        if (! exportAudio (*part, rate, file, c, res, error))
        {
            state.setText ("Export audio: " + error, juce::dontSendNotification);
            return;
        }
        const auto bitsText = res.bits == "16" ? "16-bit" : res.bits == "24" ? "24-bit" : res.bits == "32f" ? "32-bit float" : "320 kbps";
        state.setText ("Exported: " + res.file.getFullPathName() + juce::String::fromUTF8 (" \xc2\xb7 ") + juce::String (res.seconds, 2) + " s, "
                           + juce::String (res.rate / 1000.0, 1).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " kHz, " + bitsText + ", "
                           + (res.channels == 1 ? "mono" : "stereo"),
                       juce::dontSendNotification);
    };
    if (path)
    {
        run (*path);
        return;
    }
    auto nameText = cleanName (name.getText().trim());
    if (nameText.isEmpty())
        nameText = sourcePath.isNotEmpty() ? juce::File (sourcePath).getFileNameWithoutExtension() : juce::String ("audio");
    nameText += juce::String (" - edit") + (choice->mono ? " (mono)" : "");
    auto folder = sourcePath.isNotEmpty() ? juce::File (sourcePath).getParentDirectory() : juce::File (AppSettings::get ("workspaceRoot"));
    chooser = std::make_unique<juce::FileChooser> ("Export audio to a new file", folder.getChildFile (nameText + ext), "*" + ext);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [run] (const juce::FileChooser& fc) { run (fc.getResult()); });
}

void SampleEditor::save()
{
    if (wave.frames() == 0)
    {
        state.setText ("Open a song first.", juce::dontSendNotification);
        return;
    }
    int first = 0;
    auto part = mastered (wave.rendered (first));
    auto nameText = name.getText().trim();
    if (nameText.isEmpty())
        nameText = name.getTextToShowWhenEmpty() == "Name (optional)" ? juce::String() : name.getTextToShowWhenEmpty();
    SaveResult res;
    juce::String error;
    if (! saveSample (part, wave.rate(), AppSettings::samplesRoot(), nameText, kRoles[juce::jlimit (0, 18, role.getSelectedItemIndex())].key,
                      kind.getSelectedItemIndex() == 1 ? "loop" : "one-shot", bpm.value(), key.getText().trim(), sourcePath, res, error))
    {
        state.setText (error, juce::dontSendNotification);
        return;
    }
    state.setText ("Saved in the samples folder: " + res.file.getFileName(), juce::dontSendNotification);
}

void SampleEditor::showMaster (bool on)
{
    master.setVisible (on);
    masterButton.setToggleState (on, juce::dontSendNotification);
    resized();
    repaint();
}

void SampleEditor::masterChanged()
{
    masterButton.setButtonText (juce::String ("Mastering") + (master.isActive() ? juce::String::fromUTF8 (" \xe2\x97\x8f") : juce::String()));
    if (player.playing())
        player.updateSettings (master.settings());
}

//------------------------------------------------------------------------------ fades and view state
void SampleEditor::fadeKnobsChanged()
{
    if (syncing)
        return;
    const juce::String curves[] = { "smooth", "linear", "exponential" };
    wave.setFades (fadeInKnob.value() / 1000, fadeOutKnob.value() / 1000, curves[juce::jlimit (0, 2, curve.getSelectedItemIndex())]);
}

void SampleEditor::fadesFromWave()
{
    syncing = true;
    fadeInKnob.setValue (std::round (wave.fadeIn * 1000), false);
    fadeOutKnob.setValue (std::round (wave.fadeOut * 1000), false);
    syncing = false;
}

void SampleEditor::kindChanged()
{
    const bool loopKind = kind.getSelectedItemIndex() == 1;
    for (juce::Component* c : { (juce::Component*) &bpm, (juce::Component*) &bars, (juce::Component*) &barsButton })
        c->setEnabled (loopKind);
}

void SampleEditor::showSelection()
{
    if (wave.frames() == 0)
        return;
    const double rate = wave.rate();
    juce::String text;
    if (wave.hasSelection())
    {
        auto [a, b] = wave.sel;
        text = "Selection: " + clockText (a / rate) + juce::String::fromUTF8 (" \xe2\x80\x93 ") + clockText (b / rate) + " (" + juce::String ((b - a) / rate, 3) + " s)";
        if (kind.getSelectedItemIndex() == 1 && bpm.value() > 0)
            text += juce::String::fromUTF8 (" \xc2\xb7 ") + juce::String ((b - a) / rate * bpm.value() / 240, 2) + " bars";
    }
    else
        text = "Whole sound: " + clockText (wave.frames() / rate) + juce::String::fromUTF8 (" \xc2\xb7 drag on the wave to select a part.");
    info.setText (text, juce::dontSendNotification);
    exportButton.setButtonText (wave.hasSelection() ? juce::String::fromUTF8 ("Export selection\xe2\x80\xa6") : juce::String::fromUTF8 ("Export audio\xe2\x80\xa6"));
    selectionButton.setEnabled (wave.hasSelection());
    resized();
    if (onTransportChanged) onTransportChanged();
}

void SampleEditor::enable (bool on)
{
    const bool has = on && wave.frames() > 0;
    for (juce::Component* b : { (juce::Component*) &keepButton, (juce::Component*) &cutButton, (juce::Component*) &normalizeButton,
                                (juce::Component*) &zoomOut, (juce::Component*) &zoomIn, (juce::Component*) &zoomSel, (juce::Component*) &zoomAll,
                                (juce::Component*) &endButton, (juce::Component*) &loopButton, (juce::Component*) &saveButton, (juce::Component*) &exportButton })
        b->setEnabled (has);
    selectionButton.setEnabled (has && wave.hasSelection());
    undoButton.setEnabled (on && ! undoStack.empty());
}

bool SampleEditor::keyPressed (const juce::KeyPress& k)
{
    const auto cmd = k.getModifiers().isCommandDown();
    if (ui::Keys::matches (k, "toggle_play")) { togglePlay(); return true; }
    if (ui::Keys::matches (k, "go_start")) { goStart(); return true; }
    if (k == juce::KeyPress::endKey) { goEnd(); return true; }
    if (! cmd && (k.getTextCharacter() == 'l' || k.getTextCharacter() == 'L'))
    {
        loopButton.setToggleState (! loopButton.getToggleState(), juce::dontSendNotification);
        looping = loopButton.getToggleState();
        return true;
    }
    if (ui::Keys::matches (k, "undo")) { undo(); return true; }
    if (ui::Keys::matches (k, "redo") || (cmd && k.getKeyCode() == 'Y')) { redo(); return true; }
    if (ui::Keys::matches (k, "select_all")) { selectAll(); return true; }
    if (ui::Keys::matches (k, "copy")) { copy(); return true; }
    if (ui::Keys::matches (k, "cut")) { cutToClipboard(); return true; }
    if (ui::Keys::matches (k, "paste")) { paste(); return true; }
    if (ui::Keys::matches (k, "import_audio")) { folderButton.onClick(); return true; }
    if (ui::Keys::matches (k, "export")) { exportFile(); return true; }
    if (ui::Keys::matches (k, "delete_selection") && wave.hasSelection())
    {
        apply ([] (const Audio& a, int x, int y) { return cut (a, x, y); }, "cut");
        return true;
    }
    return false;
}

void SampleEditor::paint (juce::Graphics& g)
{
    for (auto box : { centreBox, inspectorBox })
    {
        auto r = box.toFloat();
        g.setGradientFill (juce::ColourGradient (col::centreTop, 0, r.getY(), col::centreBottom, 0, r.getBottom(), false));
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (col::outline);
        g.drawRoundedRectangle (r.reduced (0.5f), 2.0f, 1.0f);
        g.setColour (col::cardEdge);
        g.fillRect (r.getX() + 1, r.getBottom() - 2, r.getWidth() - 2, 2.0f);
    }
    if (master.isVisible())
    {
        auto r = master.getBounds().expanded (4).toFloat();
        g.setColour (col::card);
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (col::outline);
        g.drawRoundedRectangle (r.reduced (0.5f), 2.0f, 1.0f);
    }
    g.setColour (col::title);
    g.setFont (uiFont (15.0f, true).withExtraKerningFactor (0.1f));
    g.drawText (aicore::ui::tr ("EDIT A SONG INTO A SAMPLE"), centreBox.getX() + 20, centreBox.getY() + 12, centreBox.getWidth() - 90, 26, juce::Justification::centredLeft);
    g.drawText (aicore::ui::tr ("SAMPLE"), inspectorBox.getX() + 20, inspectorBox.getY() + 12, 300, 26, juce::Justification::centredLeft);
    g.setColour (col::outline);
    g.fillRect (inspectorBox.getX() + 16, inspectorBox.getY() + 50, inspectorBox.getWidth() - 32, 1);
    // the dot after "What it is" (the field the sample needs)
    g.setColour (col::accent);
    auto w = whatLabel.getBounds();
    const float tx = (float) w.getX() + 4 + juce::GlyphArrangement::getStringWidth (uiFont (15.0f), "What it is") + 9;
    g.fillEllipse (tx, (float) w.getCentreY() - 3, 5.0f, 5.0f);
}

void SampleEditor::resized()
{
    auto r = getLocalBounds().reduced (4, 0).withTrimmedTop (8).withTrimmedBottom (4);
    if (master.isVisible())
    {
        auto m = r.removeFromRight (320);
        master.setBounds (m.reduced (4));
        r.removeFromRight (8);
    }
    inspectorBox = r.removeFromRight (338);
    r.removeFromRight (8);
    centreBox = r;

    // the centre
    auto c = centreBox.withTrimmedTop (14).withTrimmedBottom (14);
    auto head = c.removeFromTop (30);
    closeButton.setBounds (head.removeFromRight (20).translated (-0, 0));
    head.removeFromRight (2);
    dotsButton.setBounds (head.removeFromRight (20));
    c.removeFromTop (10);
    c = c.reduced (16, 0);
    auto top = c.removeFromTop (30);
    for (auto* b : { &zoomAll, &zoomSel, &zoomIn, &zoomOut })
    {
        b->setBounds (top.removeFromRight (b->getButtonText().length() == 1 ? 34 : b->idealWidth (12)));
        top.removeFromRight (10);
    }
    folderButton.setBounds (top.removeFromRight (36));
    top.removeFromRight (6);
    source.setBounds (top.withSizeKeepingCentre (top.getWidth(), 28));
    c.removeFromTop (12);
    overview.setBounds (c.removeFromTop (38));
    c.removeFromTop (10);
    state.setBounds (c.removeFromBottom (26));
    c.removeFromBottom (6);
    auto exports = c.removeFromBottom (30);
    exportButton.setBounds (exports.removeFromLeft (exportButton.idealWidth (26)));
    exports.removeFromLeft (10);
    selectionButton.setBounds (exports.removeFromLeft (selectionButton.idealWidth (26)));
    c.removeFromBottom (10);
    auto edits = c.removeFromBottom (30);
    for (auto* b : { &keepButton, &cutButton, &normalizeButton })
    {
        b->setBounds (edits.removeFromLeft (b->idealWidth (26)));
        edits.removeFromLeft (10);
    }
    undoButton.setBounds (edits.removeFromRight (undoButton.idealWidth (26)));
    edits.removeFromRight (10);
    masterButton.setBounds (edits.removeFromRight (juce::roundToInt (juce::GlyphArrangement::getStringWidth (uiFont (15.0f), "Mastering ")) + 26));
    c.removeFromBottom (10);
    auto bar = c.removeFromBottom (20);
    endButton.setBounds (bar.removeFromLeft (40));
    bar.removeFromLeft (6);
    loopButton.setBounds (bar.removeFromLeft (40));
    bar.removeFromLeft (34);
    info.setBounds (bar.withSizeKeepingCentre (bar.getWidth(), 24));
    c.removeFromBottom (10);
    wave.setBounds (c);

    // the inspector
    auto in = inspectorBox.withTrimmedTop (60).reduced (16, 0).withTrimmedBottom (16);
    name.setBounds (in.removeFromTop (28));
    in.removeFromTop (10);
    whatLabel.setBounds (in.removeFromTop (22));
    in.removeFromTop (8);
    role.setBounds (in.removeFromTop (28));
    in.removeFromTop (10);
    kind.setBounds (in.removeFromTop (28));
    in.removeFromTop (10);
    auto knobsRow = [&] (tracks::Knob& a, tracks::Knob& b) {
        auto row = in.removeFromTop (110);
        const int half = row.getWidth() / 2;
        a.setBounds (row.removeFromLeft (half).reduced (2, 0));
        b.setBounds (row.reduced (2, 0));
    };
    knobsRow (bpm, bars);
    in.removeFromTop (10);
    barsButton.setBounds (in.removeFromTop (30));
    in.removeFromTop (10);
    knobsRow (fadeInKnob, fadeOutKnob);
    in.removeFromTop (10);
    curve.setBounds (in.removeFromTop (28));
    in.removeFromTop (10);
    key.setBounds (in.removeFromTop (28));
    saveButton.setBounds (in.removeFromBottom (30));
}

//==============================================================================
SamplePage::SamplePage()
{
    newButton.setTooltip ("Open another edit without closing the current one.");
    newButton.onClick = [this] { newEdit(); };
    addAndMakeVisible (newButton);
    newEdit();
}

SampleEditor* SamplePage::current() const
{
    return edits[currentIndex];
}

SampleEditor& SamplePage::newEdit()
{
    auto* e = edits.add (new SampleEditor());
    addChildComponent (e);
    e->onOpenRequest = [this] (const juce::String& p) { openFile (p); };
    e->onClose = [this, e] { closeEdit (e); };
    e->onTitleChanged = [this] { refreshTabs(); };
    e->onTransportChanged = [this] { if (onTransportChanged) onTransportChanged(); };
    e->onSelectionEdit = [this, e] (Audio audio, double rate, juce::String src) {
        auto& fresh = newEdit();
        fresh.loadAudio (std::move (audio), rate, src);
        refreshTabs();
        tabs[edits.indexOf (&fresh)]->setButtonText (fresh.title() + juce::String::fromUTF8 (" \xc2\xb7 Selection"));
        juce::ignoreUnused (e);
        resized();
    };
    select (edits.size() - 1);
    return *e;
}

void SamplePage::openFile (const juce::String& path)
{
    auto* e = current();
    if (e == nullptr || e->hasSound())
        e = &newEdit();
    e->openFile (path);
}

void SamplePage::closeEdit (SampleEditor* e)
{
    auto doClose = [this, safe = juce::Component::SafePointer<SampleEditor> (e)] {
        if (safe == nullptr)
            return;
        auto* ed = safe.getComponent();
        ed->stopPlayback();
        const int i = edits.indexOf (ed);
        edits.removeObject (ed);
        if (edits.isEmpty())
            newEdit();
        else
            select (juce::jlimit (0, edits.size() - 1, i));
    };
    if (e->wave.frames() > 0)
    {
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Close edit",
                                            "Close this edit? Export any parts you want to keep first.", "Yes", "No", nullptr,
                                            juce::ModalCallbackFunction::create ([doClose] (int r) { if (r == 1) doClose(); }));
        return;
    }
    doClose();
}

void SamplePage::select (int index)
{
    for (int i = 0; i < edits.size(); ++i)
        if (i != index && edits[i]->isPlaying())
            edits[i]->stopPlayback(); // only the edit in front plays
    currentIndex = juce::jlimit (0, juce::jmax (0, edits.size() - 1), index);
    for (int i = 0; i < edits.size(); ++i)
        edits[i]->setVisible (i == currentIndex);
    refreshTabs();
    if (onTransportChanged) onTransportChanged();
}

void SamplePage::refreshTabs()
{
    while (tabs.size() > edits.size())
        tabs.removeLast();
    while (tabs.size() < edits.size())
    {
        auto* b = tabs.add (new AButton());
        const int i = tabs.size() - 1;
        b->onClick = [this, b] { select (tabs.indexOf (b)); };
        addAndMakeVisible (b);
        juce::ignoreUnused (i);
    }
    for (int i = 0; i < edits.size(); ++i)
    {
        if (! tabs[i]->getButtonText().contains (juce::String::fromUTF8 ("\xc2\xb7 Selection")) || edits[i]->sourcePath.isEmpty())
            tabs[i]->setButtonText (edits[i]->title());
        tabs[i]->setTooltip (edits[i]->sourcePath);
        tabs[i]->setToggleState (i == currentIndex, juce::dontSendNotification);
    }
    resized();
}

void SamplePage::paint (juce::Graphics&) {}

void SamplePage::resized()
{
    auto r = getLocalBounds();
    auto row = r.removeFromTop (40);
    newButton.setBounds (row.removeFromRight (newButton.idealWidth (28)).withTrimmedTop (5).withHeight (30));
    auto left = row.withTrimmedTop (1).withHeight (28);
    for (auto* b : tabs)
    {
        b->setBounds (left.removeFromLeft (b->idealWidth (28)));
        left.removeFromLeft (4);
    }
    for (auto* e : edits)
        e->setBounds (r.withTrimmedTop (-4));
}
} // namespace aicore::sample
