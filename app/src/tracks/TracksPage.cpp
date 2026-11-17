#include "TracksPage.h"
#include "../ui/I18n.h"
#include "../ui/AudioOut.h"
#include "../ui/Keybinds.h"

namespace aicore::tracks
{
using namespace aicore::ui;
namespace
{
constexpr size_t maxUndo = 200;
}

void TracksPage::Tools::resized()
{
    auto r = getLocalBounds();
    for (auto* b : buttons)
    {
        b->setBounds (r.removeFromLeft (b->idealWidth (b->getButtonText().length() <= 1 ? 26 : 28)).withSizeKeepingCentre (b->idealWidth (b->getButtonText().length() <= 1 ? 26 : 28), 30));
        r.removeFromLeft (6);
    }
}

TracksPage::TracksPage()
{
    formats.registerBasicFormats();
    setWantsKeyboardFocus (true);
    addAndMakeVisible (view);
    view.onEdited = [this] (const Session& before) {
        session = view.session;
        commit (before);
    };
    view.onSeek = [this] (double s) { restartPlayerAt (s); };
    view.onSelectionChanged = [this] { selectionChanged(); };
    view.onViewChanged = [this] {
        syncing = true;
        const double total = juce::jmax (view.session.length() + view.visibleSeconds(), view.visibleSeconds());
        hscroll.setRangeLimits (0.0, total);
        hscroll.setCurrentRange (view.scrollX, view.visibleSeconds());
        syncing = false;
    };
    view.onFilesDropped = [this] (const juce::StringArray& paths, int track, double at) { addFiles (paths, track, at); };
    view.onContextMenu = [this] (const juce::MouseEvent& e, int ti, Clip* c) { contextMenu (e, ti, c); };
    view.onFx = [this] (int ti) {
        chainMaster = false;
        chainTrack = ti;
        dockOpen = true;
        showChain();
        resized();
    };
    addChildComponent (chain);
    chain.onChanged = [this] (const ChainSettings& s) {
        const bool m = chainMaster;
        const int ti = chainTrack;
        apply ([m, ti, s] (Session& ss) {
            if (m)
                ss.master = s;
            else if (ti >= 0 && ti < (int) ss.tracks.size())
                ss.tracks[(size_t) ti].fx = s;
        });
    };
    chain.onPathChosen = [this] (bool m) {
        chainMaster = m || session.tracks.empty();
        if (! m)
            chainTrack = juce::jlimit (0, juce::jmax (0, (int) session.tracks.size() - 1), view.currentTrack);
        showChain();
    };
    showChain();

    auto tool = [this] (const juce::String& text, const juce::String& tip, std::function<void()> fn) {
        auto* b = tools.buttons.add (new AButton (text, std::move (fn)));
        b->setTooltip (tip);
        tools.addAndMakeVisible (b);
        return b;
    };
    tool (juce::String::fromUTF8 ("Import audio…"), "Audio files onto the chosen track, at the playhead (Ctrl+O). Or drag them from the Explorer onto a track.",
          [this] { importAudio(); });
    tool ("Split", "Cut the chosen clips (or the clips under the playhead) at the playhead (S)", [this] { split(); });
    tool (juce::String::fromUTF8 ("\xe2\x88\x92"), "Zoom out (Ctrl+wheel)", [this] { view.zoom (0.67); });
    tool ("+", "Zoom in (Ctrl+wheel)", [this] { view.zoom (1.5); });
    tool ("Options", "Mastering, magnet, BPM, new track, duplicate, delete, fit, undo, zoom", [this] { showOptions(); });
    exportButton.setTooltip ("Every track through its inserts and the master: WAV. The files are never changed.");
    exportButton.onClick = [this] { exportMix(); };

    hscroll.setAutoHide (false);
    hscroll.addListener (this);
    addAndMakeVisible (hscroll);

    for (auto* l : { &clipLabel, &state, &gainLabel, &fadeInLabel, &fadeOutLabel })
    {
        l->setFont (uiFont (15.0f));
        l->setColour (juce::Label::textColourId, l == &clipLabel || l == &state ? col::muted : col::text);
        addAndMakeVisible (l);
    }
    clipLabel.setText ("No clip chosen", juce::dontSendNotification);
    gainLabel.setText ("Clip gain dB", juce::dontSendNotification);
    fadeInLabel.setText ("Fade in s", juce::dontSendNotification);
    fadeOutLabel.setText ("Fade out s", juce::dontSendNotification);
    for (auto* f : { &clipGain, &fadeIn, &fadeOut })
    {
        f->onValue = [this] (double) { clipValues(); };
        addAndMakeVisible (f);
    }
    for (auto* b : { &dockUp, &dockMax, &dockClose })
        addAndMakeVisible (b);
    dockUp.onClick = [this] { dockOpen = ! dockOpen; resized(); repaint(); };
    dockMax.onClick = [this] { dockOpen = true; resized(); repaint(); };
    dockClose.onClick = [this] { dockOpen = false; resized(); repaint(); };

    refresh();
    startTimer (40);
}

TracksPage::~TracksPage()
{
    stopTimer();
    ui::AudioOut::get().detach (&transport);
    transport.setSource (nullptr);
}

void TracksPage::paint (juce::Graphics& g)
{
    g.fillAll (col::background);
    // the centre card (QWidget#centrePanel) and the dock card
    auto area = getLocalBounds().reduced (4, 0).withTrimmedTop (8).withTrimmedBottom (4);
    auto dock = area.removeFromBottom (dockOpen ? 280 : 38);
    area.removeFromBottom (8);
    for (auto r : { area.toFloat(), dock.toFloat() })
    {
        g.setGradientFill (juce::ColourGradient (col::centreTop, r.getX(), r.getY(), col::centreBottom, r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (col::outline);
        g.drawRoundedRectangle (r.reduced (0.5f), 2.0f, 1.0f);
        g.setColour (col::cardEdge);
        g.fillRect (r.getX() + 1, r.getBottom() - 2, r.getWidth() - 2, 2.0f);
    }
    g.setColour (col::title);
    g.setFont (uiFont (15.0f, true).withExtraKerningFactor (0.1f)); // panelHeading: letter-spacing 1.5px
    g.drawText (aicore::ui::tr (chainTitle()), dock.withHeight (38).withTrimmedLeft (16), juce::Justification::centredLeft);
}

void TracksPage::openChain (int track)
{
    chainMaster = track < 0;
    chainTrack = juce::jmax (0, track);
    dockOpen = true;
    showChain();
    resized();
}

juce::String TracksPage::chainTitle() const
{
    const auto dot = juce::String::fromUTF8 (" \xc2\xb7 ");
    if (chainMaster || chainTrack < 0 || chainTrack >= (int) session.tracks.size())
        return "CHAIN" + dot + "MASTER";
    const auto& t = session.tracks[(size_t) chainTrack];
    return "CHAIN" + dot + (t.name.isNotEmpty() ? t.name : "Track " + juce::String (chainTrack + 1)).toUpperCase();
}

void TracksPage::showChain()
{
    if (! chainMaster && (chainTrack < 0 || chainTrack >= (int) session.tracks.size()))
        chainMaster = true;
    if (chainMaster)
        chain.showSettings (session.master, true);
    else
    {
        const auto& t = session.tracks[(size_t) chainTrack];
        chain.showSettings (t.fx, false, trackColour (t.name, chainTrack));
    }
    repaint();
}

void TracksPage::resized()
{
    auto area = getLocalBounds().reduced (4, 0).withTrimmedTop (8).withTrimmedBottom (4);
    auto dock = area.removeFromBottom (dockOpen ? 280 : 38);
    area.removeFromBottom (8);
    auto bar = dock.removeFromTop (38).reduced (8, 6);
    dockClose.setBounds (bar.removeFromRight (24));
    bar.removeFromRight (4);
    dockMax.setBounds (bar.removeFromRight (24));
    bar.removeFromRight (4);
    dockUp.setBounds (bar.removeFromRight (24));
    chain.setVisible (dockOpen);
    chain.setBounds (dock.reduced (8, 4));

    auto inner = area.reduced (16, 14);
    auto row = inner.removeFromBottom (32);
    inner.removeFromBottom (10);
    hscroll.setBounds (inner.removeFromBottom (12));
    inner.removeFromBottom (4);
    view.setBounds (inner);

    row.removeFromLeft (20);
    clipLabel.setBounds (row.removeFromLeft (160));
    for (auto [label, field] : { std::pair<juce::Label*, NumberField*> { &gainLabel, &clipGain }, { &fadeInLabel, &fadeIn }, { &fadeOutLabel, &fadeOut } })
    {
        label->setBounds (row.removeFromLeft (juce::roundToInt (juce::GlyphArrangement::getStringWidth (uiFont (15.0f), label->getText())) + 10));
        field->setBounds (row.removeFromLeft (72).withSizeKeepingCentre (72, 26));
        row.removeFromLeft (6);
    }
    row.removeFromLeft (8);
    state.setBounds (row);
}

//==============================================================================
void TracksPage::commit (const std::optional<Session>& before, const juce::String& message)
{
    if (before)
    {
        undoStack.push_back (*before);
        if (undoStack.size() > maxUndo)
            undoStack.erase (undoStack.begin());
        redoStack.clear();
    }
    if (message.isNotEmpty())
        state.setText (message, juce::dontSendNotification);
    refresh();
}

void TracksPage::apply (std::function<void (Session&)> fn, const juce::String& message)
{
    const Session before = session;
    fn (session);
    commit (before, message);
}

void TracksPage::refresh()
{
    view.setSession (session);
    mixer.setSession (session);
    if (! transport.isPlaying())
        transport.setSource (session.tracks.empty() ? nullptr : &mixer, 0, nullptr, mixer.rate());
    exportButton.setEnabled (session.length() > 0);
    selectionChanged();
    if (onTransportChanged)
        onTransportChanged();
}

void TracksPage::undo()
{
    if (undoStack.empty())
        return;
    redoStack.push_back (session);
    session = undoStack.back();
    undoStack.pop_back();
    state.setText ("Undone.", juce::dontSendNotification);
    refresh();
    showChain();
}

void TracksPage::redo()
{
    if (redoStack.empty())
        return;
    undoStack.push_back (session);
    session = redoStack.back();
    redoStack.pop_back();
    state.setText ("Done again.", juce::dontSendNotification);
    refresh();
    showChain();
}

void TracksPage::selectionChanged()
{
    std::vector<Clip*> chosen;
    for (int id : view.selected)
        if (auto* c = session.find (id))
            chosen.push_back (c);
    const bool any = ! chosen.empty();
    for (auto* f : { &clipGain, &fadeIn, &fadeOut })
        f->setEnabled (any);
    if (! any)
    {
        clipLabel.setText ("No clip chosen", juce::dontSendNotification);
        return;
    }
    clipLabel.setText (chosen.size() == 1 ? chosen[0]->label() : juce::String ((int) chosen.size()) + " clips", juce::dontSendNotification);
    clipGain.setValue (chosen[0]->gainDb, false);
    fadeIn.setValue (chosen[0]->fadeIn, false);
    fadeOut.setValue (chosen[0]->fadeOut, false);
}

void TracksPage::clipValues()
{
    if (view.selected.empty())
        return;
    apply ([this] (Session& s) {
        for (int id : view.selected)
            if (auto* c = s.find (id))
            {
                c->gainDb = clipGain.value();
                c->fadeIn = juce::jmin (fadeIn.value(), c->length);
                c->fadeOut = juce::jmin (fadeOut.value(), c->length - c->fadeIn);
            }
    });
}

juce::StringArray TracksPage::addFiles (const juce::StringArray& paths, int track, double at)
{
    juce::StringArray added;
    const Session before = session;
    int t = track >= 0 ? track : (session.tracks.empty() ? 0 : view.currentTrack);
    const double start = at >= 0 ? at : view.playhead;
    for (auto& p : paths)
    {
        // each file on its own track from the chosen one down; an empty or new track takes the file's name
        while ((int) session.tracks.size() > t && ! session.tracks[(size_t) t].clips.empty() && paths.size() > 1)
            ++t;
        const bool fresh = t >= (int) session.tracks.size() || session.tracks[(size_t) t].clips.empty();
        if (auto* c = addClip (session, t, juce::File (p), start))
        {
            if (fresh)
                session.tracks[(size_t) t].name = juce::File (p).getFileNameWithoutExtension();
            added.add (juce::File (p).getFileName());
        }
        ++t;
    }
    if (added.isEmpty())
    {
        state.setText ("Could not read: " + paths.joinIntoString (", "), juce::dontSendNotification);
        return added;
    }
    bool otherRate = false;
    for (auto& tr : session.tracks)
        for (auto& c : tr.clips)
        {
            std::unique_ptr<juce::AudioFormatReader> rd (formats.createReaderFor (juce::File (c.path)));
            otherRate = otherRate || (rd != nullptr && (int) rd->sampleRate != session.rate);
        }
    commit (before, "Added: " + added.joinIntoString (", ")
                        + (otherRate ? "  Files at another sample rate are converted when they play." : ""));
    if (session.tracks.size() == 1 || view.pps == 40)
        view.zoomAll();
    return added;
}

void TracksPage::importAudio()
{
    chooser = std::make_unique<juce::FileChooser> ("Import audio", juce::File(), "*.wav;*.flac;*.mp3;*.ogg;*.aif;*.aiff");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc) {
                              juce::StringArray paths;
                              for (auto& f : fc.getResults())
                                  paths.add (f.getFullPathName());
                              if (! paths.isEmpty())
                                  addFiles (paths);
                          });
}

void TracksPage::split (std::optional<double> at)
{
    const double t = at ? *at : view.playhead;
    std::vector<int> ids (view.selected.begin(), view.selected.end());
    Session s = session;
    auto made = splitAt (s, t, ids, ids.empty() ? -1 : -1);
    if (made.empty())
    {
        state.setText ("Put the playhead over a clip to split it.", juce::dontSendNotification);
        return;
    }
    const Session before = session;
    session = s;
    commit (before, "Split at " + clockText (t));
}

void TracksPage::deleteSelection()
{
    if (view.selected.empty())
        return;
    std::vector<int> ids (view.selected.begin(), view.selected.end());
    apply ([&] (Session& s) { remove (s, ids); }, "Deleted. Undo brings it back.");
}

void TracksPage::duplicate()
{
    std::vector<int> ids (view.selected.begin(), view.selected.end());
    if (ids.empty())
        return;
    std::vector<int> made;
    apply ([&] (Session& s) { made = tracks::duplicate (s, ids); }, "Duplicated.");
    view.selected = std::set<int> (made.begin(), made.end());
    view.repaint();
}

void TracksPage::addTrack()
{
    apply ([] (Session& s) { tracks::addTrack (s); }, "New track");
}

void TracksPage::showOptions()
{
    juce::PopupMenu m;
    m.addItem (1, "Magnet", true, view.snapping);
    m.addItem (2, "New track (Ctrl+T)");
    m.addItem (3, "Duplicate (Ctrl+D)", ! view.selected.empty());
    m.addItem (4, "Delete (Del)", ! view.selected.empty());
    m.addSeparator();
    m.addItem (5, "Undo", ! undoStack.empty());
    m.addItem (6, "Redo", ! redoStack.empty());
    m.addSeparator();
    m.addItem (7, "Show the whole session");
    m.addItem (8, "BPM: " + juce::String (session.bpm, 0) + juce::String::fromUTF8 ("…"));
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (tools.buttons[4]), [this] (int r) {
        if (r == 1)
            view.snapping = ! view.snapping;
        else if (r == 2)
            addTrack();
        else if (r == 3)
            duplicate();
        else if (r == 4)
            deleteSelection();
        else if (r == 5)
            undo();
        else if (r == 6)
            redo();
        else if (r == 7)
            view.zoomAll();
        else if (r == 8)
        {
            auto* w = new juce::AlertWindow ("BPM", "BPM of the grid (the magnet snaps to its beats)", juce::MessageBoxIconType::NoIcon, this);
            w->addTextEditor ("bpm", juce::String (session.bpm, 0));
            w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w] (int ok) {
                const double v = w->getTextEditorContents ("bpm").getDoubleValue();
                if (ok == 1 && v >= 40 && v <= 300)
                    apply ([v] (Session& s) { s.bpm = v; });
            }), true);
        }
    });
}

void TracksPage::contextMenu (const juce::MouseEvent& e, int ti, Clip* c)
{
    juce::PopupMenu m;
    const double at = view.secondsAt (e.position.x);
    if (c != nullptr)
    {
        m.addItem (1, "Split here");
        m.addItem (2, "Duplicate (Ctrl+D)");
        m.addItem (4, "Delete (Del)");
    }
    else if (ti >= 0 && ti < (int) session.tracks.size())
    {
        view.currentTrack = ti;
        m.addItem (5, juce::String::fromUTF8 ("Import audio…"));
        m.addItem (6, "Rename track");
        m.addItem (7, "Delete track");
    }
    else
    {
        m.addItem (8, "New track (Ctrl+T)");
        m.addItem (5, juce::String::fromUTF8 ("Import audio…"));
    }
    m.showMenuAsync ({}, [this, at, ti] (int r) {
        switch (r)
        {
            case 1: split (at); break;
            case 2: duplicate(); break;
            case 4: deleteSelection(); break;
            case 5: importAudio(); break;
            case 6: view.renameTrack (ti); break;
            case 7: apply ([ti] (Session& s) { if (ti < (int) s.tracks.size()) s.tracks.erase (s.tracks.begin() + ti); }, "Deleted. Undo brings it back."); break;
            case 8: addTrack(); break;
            default: break;
        }
    });
}

bool TracksPage::keyPressed (const juce::KeyPress& k)
{
    using ui::Keys; // SETTINGS > KEYBINDS
    const auto mods = k.getModifiers();
    if (Keys::matches (k, "toggle_play")) { togglePlay(); return true; }
    if (Keys::matches (k, "go_start")) { goStart(); return true; }
    if (Keys::matches (k, "split") || Keys::matches (k, "export")) { split(); return true; } // Tracks: Export = split (old app)
    if (Keys::matches (k, "delete_selection") || k.getKeyCode() == juce::KeyPress::backspaceKey) { deleteSelection(); return true; }
    if (Keys::matches (k, "duplicate")) { duplicate(); return true; }
    if (Keys::matches (k, "add_track")) { addTrack(); return true; }
    if (Keys::matches (k, "import_audio")) { importAudio(); return true; }
    if (Keys::matches (k, "undo")) { undo(); return true; }
    if (Keys::matches (k, "redo") || (mods.isCtrlDown() && k.getKeyCode() == 'Y')) { redo(); return true; }
    return false;
}

//==============================================================================
void TracksPage::ensureDevice()
{
    ui::AudioOut::get().attach (&transport);
}

void TracksPage::restartPlayerAt (double seconds)
{
    transport.setPosition (seconds);
    view.setPlayhead (seconds);
    if (onTransportChanged)
        onTransportChanged();
}

void TracksPage::togglePlay()
{
    if (session.tracks.empty())
        return;
    ensureDevice();
    if (transport.isPlaying())
        transport.stop();
    else
    {
        mixer.setSession (session);
        mixer.measureLufs();
        if (transport.getCurrentPosition() >= session.length() - 0.01)
            transport.setPosition (0);
        playStart = transport.getCurrentPosition();
        transport.start();
    }
    if (onTransportChanged)
        onTransportChanged();
}

void TracksPage::stop()
{
    transport.stop();
    transport.setPosition (playStart);
    view.setPlayhead (playStart);
    if (onTransportChanged)
        onTransportChanged();
}

void TracksPage::goStart()
{
    transport.setPosition (0);
    playStart = 0;
    view.setPlayhead (0);
    if (onTransportChanged)
        onTransportChanged();
}

juce::String TracksPage::clock() const
{
    return clockText (transport.isPlaying() ? transport.getCurrentPosition() : view.playhead) + " / " + clockText (session.length());
}

void TracksPage::timerCallback()
{
    if (transport.isPlaying())
    {
        view.levels.resize (session.tracks.size());
        for (size_t i = 0; i < session.tracks.size(); ++i)
            view.levels[i] = mixer.trackPeak ((int) i);
        view.setPlayhead (transport.getCurrentPosition(), true);
        if (onTransportChanged)
            onTransportChanged();
    }
    else if (! view.levels.empty())
    {
        view.levels.clear();
        view.repaint();
    }
}

void TracksPage::exportMix (const juce::File& target)
{
    auto run = [this] (const juce::File& file) {
        if (file == juce::File())
            return;
        auto out = file.withFileExtension ("wav");
        state.setText (juce::String::fromUTF8 ("Exporting…"), juce::dontSendNotification);
        Mixer m (formats);
        m.setSession (session);
        auto all = m.renderAll(); // every track through its inserts, then the master: what is heard is written
        out.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (out);
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (m.rate()).withNumChannels (2).withBitsPerSample (24));
        if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (all, 0, all.getNumSamples()))
        {
            state.setText ("Could not write: " + out.getFullPathName(), juce::dontSendNotification);
            return;
        }
        writer.reset();
        state.setText ("Exported: " + out.getFileName(), juce::dontSendNotification);
    };
    if (target != juce::File())
    {
        run (target);
        return;
    }
    chooser = std::make_unique<juce::FileChooser> ("Export the mix", juce::File(), "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [run] (const juce::FileChooser& fc) { run (fc.getResult()); });
}
} // namespace aicore::tracks
