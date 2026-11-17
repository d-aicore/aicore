#include "LoraPage.h"
#include "../ui/I18n.h"

#include "../ui/AppSettings.h"

#include <juce_cryptography/juce_cryptography.h>

namespace aicore::lora
{
using namespace aicore::ui;

namespace
{
juce::String nowIso()
{
    return juce::Time::getCurrentTime().toISO8601 (true);
}

juce::String sha256 (const juce::File& f)
{
    juce::FileInputStream in (f);
    return in.openedOk() ? juce::SHA256 (in).toHexString() : juce::String();
}

juce::String slugOf (const juce::String& text, int max, bool lower)
{
    juce::String s;
    bool dash = false;
    for (auto c : (lower ? text.toLowerCase() : text))
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z' && ! lower) || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (ok)
        {
            s << juce::String::charToString (c);
            dash = false;
        }
        else if (! dash)
        {
            s << "-";
            dash = true;
        }
    }
    s = s.trimCharactersAtStart ("-").trimCharactersAtEnd ("-").substring (0, max);
    return s;
}

void styleCombo (juce::ComboBox& c)
{
    c.setColour (juce::ComboBox::backgroundColourId, col::field);
    c.setColour (juce::ComboBox::textColourId, col::text);
    c.setColour (juce::ComboBox::outlineColourId, col::outline);
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
    void drawTableHeaderColumn (juce::Graphics& g, juce::TableHeaderComponent&, const juce::String& n, int, int w, int h, bool, bool, int) override
    {
        g.setColour (col::text);
        g.setFont (uiFont (15.0f, true));
        g.drawText (n, 0, 0, w, h, juce::Justification::centred);
    }
};
TableLook& tableLook()
{
    static TableLook look;
    return look;
}

const char* columnKey (int column)
{
    switch (column)
    {
        case 1: return "name";
        case 2: return "caption";
        case 3: return "lyrics";
        case 4: return "bpm";
        case 5: return "language";
        default: return "lineageGroup";
    }
}
} // namespace

//==============================================================================
Collections::Collections (juce::File r) : root (std::move (r)) { root.createDirectory(); }

bool Collections::write (const juce::File& f, const juce::var& v)
{
    f.getParentDirectory().createDirectory();
    auto tmp = f.getSiblingFile ("." + f.getFileName() + juce::Uuid().toString().substring (0, 6));
    return tmp.replaceWithText (juce::JSON::toString (v, false)) && tmp.moveFileTo (f);
}

juce::Array<juce::var> Collections::list() const
{
    juce::Array<juce::var> out;
    for (auto& d : root.findChildFiles (juce::File::findDirectories, false))
    {
        auto f = d.getChildFile ("collection.json");
        if (! f.existsAsFile())
            continue;
        auto v = juce::JSON::parse (f);
        if (v["schemaVersion"].toString() == "audio-collection-1")
            out.add (v);
    }
    std::sort (out.begin(), out.end(), [] (const juce::var& a, const juce::var& b) { return a["name"].toString().compareIgnoreCase (b["name"].toString()) < 0; });
    return out;
}

juce::File Collections::directory (const juce::String& id) const
{
    for (auto& d : root.findChildFiles (juce::File::findDirectories, false))
    {
        auto f = d.getChildFile ("collection.json");
        if (f.existsAsFile() && juce::JSON::parse (f)["id"].toString() == id)
            return d;
    }
    return {};
}

juce::var Collections::get (const juce::String& id) const
{
    auto d = directory (id);
    return d == juce::File() ? juce::var() : juce::JSON::parse (d.getChildFile ("collection.json"));
}

juce::String Collections::create (const juce::String& name, const juce::String& kind, int limit, const juce::String& caption, juce::String& newId)
{
    if (name.trim().isEmpty() || name.length() > 80 || (kind != "voice" && kind != "samples") || (limit != 10 && limit != 20 && limit != 30))
        return "Name (up to 80 characters), Voice/Samples and limit 10/20/30 are needed.";
    newId = juce::Uuid().toString().removeCharacters ("-");
    auto* o = new juce::DynamicObject();
    o->setProperty ("schemaVersion", "audio-collection-1");
    o->setProperty ("id", newId);
    o->setProperty ("name", name.trim());
    o->setProperty ("kind", kind);
    o->setProperty ("limit", limit);
    o->setProperty ("caption", caption.trim());
    o->setProperty ("createdAt", nowIso());
    o->setProperty ("version", 1);
    o->setProperty ("items", juce::Array<juce::var>());
    auto slug = slugOf (name, 50, true);
    if (slug.isEmpty())
        slug = "template";
    return write (root.getChildFile (slug + "-" + newId.substring (0, 8)).getChildFile ("collection.json"), juce::var (o)) ? juce::String() : "Could not write the collection.";
}

juce::String Collections::add (const juce::String& id, const juce::StringArray& paths)
{
    auto item = get (id);
    auto dir = directory (id);
    if (! item.isObject())
        return "Collection not found in the current training folder.";
    juce::StringArray known;
    for (auto& s : *item["items"].getArray())
        known.add (s["sourceHash"].toString());
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    juce::Array<juce::var> pending;
    juce::Array<juce::File> sources;
    for (auto& p : paths)
    {
        juce::File path (p);
        const auto hash = sha256 (path);
        if (hash.isEmpty() || known.contains (hash))
            continue;
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (path));
        if (r == nullptr || r->numChannels < 1 || r->numChannels > 2 || r->sampleRate < 8000 || r->sampleRate > 192000 || r->lengthInSamples < 1)
            return "Use mono/stereo audio at 8-192 kHz: " + path.getFileName();
        const auto sampleId = juce::Uuid().toString().removeCharacters ("-");
        auto lyricsFile = path.withFileExtension ("lyrics.txt"), captionFile = path.withFileExtension ("caption.txt"), lineageFile = path.withFileExtension ("aicore.json");
        auto lineage = lineageFile.existsAsFile() ? juce::JSON::parse (lineageFile) : juce::var();
        auto slug = slugOf (path.getFileNameWithoutExtension(), 60, false);
        if (slug.isEmpty())
            slug = "sample";
        auto* s = new juce::DynamicObject();
        s->setProperty ("id", sampleId);
        s->setProperty ("name", path.getFileName());
        s->setProperty ("sourceHash", hash);
        s->setProperty ("sourcePath", path.getFullPathName());
        s->setProperty ("lineageGroup", lineage["lineageGroup"].toString().isNotEmpty() ? lineage["lineageGroup"].toString() : hash);
        s->setProperty ("audio", slug + "-" + sampleId.substring (0, 8) + path.getFileExtension().toLowerCase());
        s->setProperty ("sampleRate", (int) r->sampleRate);
        s->setProperty ("channels", (int) r->numChannels);
        s->setProperty ("frames", (juce::int64) r->lengthInSamples);
        s->setProperty ("seconds", r->lengthInSamples / r->sampleRate);
        s->setProperty ("subtype", r->usesFloatingPointData ? juce::String ("FLOAT") : "PCM_" + juce::String (r->bitsPerSample));
        s->setProperty ("caption", captionFile.existsAsFile() ? captionFile.loadFileAsString().trim() : item["caption"].toString());
        s->setProperty ("captionOrigin", captionFile.existsAsFile() ? "sidecar" : "collection-intent");
        s->setProperty ("lyrics", lyricsFile.existsAsFile() ? lyricsFile.loadFileAsString().trim() : juce::String());
        s->setProperty ("instrumental", item["kind"].toString() == "samples");
        s->setProperty ("bpm", juce::var());
        s->setProperty ("key", "");
        s->setProperty ("language", "unknown");
        pending.add (juce::var (s));
        sources.add (path);
        known.add (hash);
    }
    if (item["items"].getArray()->size() + pending.size() > (int) item["limit"])
        return "This collection takes " + item["limit"].toString() + " samples; raise the limit or choose fewer files.";
    juce::Array<juce::File> copied;
    for (int i = 0; i < pending.size(); ++i)
    {
        auto target = dir.getChildFile (pending[i]["audio"].toString());
        if (! sources[i].copyFileTo (target) || sha256 (target) != pending[i]["sourceHash"].toString())
        {
            for (auto& c : copied)
                c.deleteFile();
            target.deleteFile();
            return "The file changed while it was imported.";
        }
        copied.add (target);
    }
    for (auto& p : pending)
        item["items"].getArray()->add (p);
    item.getDynamicObject()->setProperty ("version", (int) item["version"] + 1);
    return write (dir.getChildFile ("collection.json"), item) ? juce::String() : "Could not write the collection.";
}

juce::String Collections::update (const juce::String& id, const juce::var& changes)
{
    auto item = get (id);
    if (! item.isObject())
        return "Collection not found in the current training folder.";
    for (auto& s : *item["items"].getArray())
    {
        auto edit = changes[juce::Identifier (s["id"].toString())];
        if (! edit.isObject())
            continue;
        for (auto key : { "caption", "lyrics", "language", "key", "lineageGroup" })
            if (edit.hasProperty (key))
                s.getDynamicObject()->setProperty (key, edit[key].toString().trim());
        if (edit.hasProperty ("bpm"))
        {
            auto bpm = edit["bpm"];
            if (! bpm.isVoid() && ((double) bpm < 30 || (double) bpm > 300))
                return "ACE training BPM: 30-300 or empty.";
            s.getDynamicObject()->setProperty ("bpm", bpm);
        }
    }
    item.getDynamicObject()->setProperty ("version", (int) item["version"] + 1);
    return write (directory (id).getChildFile ("collection.json"), item) ? juce::String() : "Could not write the collection.";
}

juce::String Collections::rename (const juce::String& id, const juce::String& name, int limit, const juce::String& caption)
{
    auto item = get (id);
    if (! item.isObject() || name.trim().isEmpty() || name.length() > 80 || (limit != 10 && limit != 20 && limit != 30) || limit < item["items"].getArray()->size())
        return "Name or limit not valid for this collection.";
    auto* o = item.getDynamicObject();
    o->setProperty ("name", name.trim());
    o->setProperty ("limit", limit);
    o->setProperty ("caption", caption.trim());
    o->setProperty ("version", (int) item["version"] + 1);
    for (auto& s : *item["items"].getArray())
        if (s["caption"].toString().isEmpty())
        {
            s.getDynamicObject()->setProperty ("caption", caption.trim());
            s.getDynamicObject()->setProperty ("captionOrigin", "collection-intent");
        }
    return write (directory (id).getChildFile ("collection.json"), item) ? juce::String() : "Could not write the collection.";
}

juce::String Collections::remove (const juce::String& id, const juce::StringArray& sampleIds)
{
    auto item = get (id);
    if (! item.isObject())
        return "Collection not found in the current training folder.";
    juce::Array<juce::var> keep;
    for (auto& s : *item["items"].getArray())
        if (! sampleIds.contains (s["id"].toString()))
            keep.add (s);
    item.getDynamicObject()->setProperty ("items", keep);
    item.getDynamicObject()->setProperty ("version", (int) item["version"] + 1);
    return write (directory (id).getChildFile ("collection.json"), item) ? juce::String() : "Could not write the collection.";
}

//==============================================================================
LoraPage::LoraPage()
{
    setWantsKeyboardFocus (true);
    collections.onChange = [this] { select(); };
    kind.addItemList ({ "Samples (instrumental)", "Voice" }, 1);
    kind.setSelectedItemIndex (0, juce::dontSendNotification);
    limit.addItemList ({ "10", "20", "30" }, 1);
    limit.setSelectedItemIndex (0, juce::dontSendNotification);
    for (auto* c : { &collections, &kind, &limit })
    {
        styleCombo (*c);
        addAndMakeVisible (c);
    }
    for (auto* t : { &name, &caption })
    {
        t->setFont (uiFont (15.0f));
        t->setIndents (8, 5);
        addAndMakeVisible (t);
    }
    name.setTextToShowWhenEmpty ("Template name", col::dim);
    caption.setTextToShowWhenEmpty ("Shared description of the sound or voice; captions per sample can differ", col::dim);
    for (auto [l, t] : { std::pair<juce::Label*, const char*> { &nameLabel, "Template name" }, { &kindLabel, "Type" }, { &limitLabel, "Most samples" },
                         { &captionLabel, "Collection description" } })
    {
        l->setText (t, juce::dontSendNotification);
        l->setFont (uiFont (13.0f));
        l->setColour (juce::Label::textColourId, col::text);
        addAndMakeVisible (l);
    }
    state.setText ("Ready.", juce::dontSendNotification);
    state.setFont (uiFont (15.0f));
    state.setColour (juce::Label::textColourId, col::text);
    addAndMakeVisible (state);
    epochs.prefix = "Epochs  ";
    addAndMakeVisible (epochs);
    go.onClick = [this] { prepare (true); };
    addButton.onClick = [this] { addFiles(); };
    dots.onClick = [this] { options(); };
    stopButton.onClick = [this] { worker.stop(); };
    for (juce::Component* c : { (juce::Component*) &go, (juce::Component*) &addButton, (juce::Component*) &dots })
        addAndMakeVisible (c);
    addChildComponent (stopButton);
    auto& h = table.getHeader();
    int id = 1;
    for (auto t : { "Sample", "Description / caption", "Exact words (voice)", "BPM", "Language", "Source group" })
        h.addColumn (t, id++, 170, 60, -1, juce::TableHeaderComponent::notSortable);
    h.setStretchToFitActive (true);
    h.setPopupMenuActive (false);
    h.setLookAndFeel (&tableLook());
    table.setHeaderHeight (44);
    table.setRowHeight (30);
    table.setColour (juce::ListBox::backgroundColourId, col::field);
    table.setColour (juce::ListBox::outlineColourId, col::outline);
    table.setOutlineThickness (1);
    addAndMakeVisible (table);
    worker.onError = [this] (const juce::String& m) { fail (m); };
    worker.onDone = [this] (const juce::var& r) { done (r); };
    worker.onEvent = [this] (const juce::var& v0) {
        auto v = v0["event"].toString() == "provider" ? v0["value"] : v0;
        if (v["event"].toString() != "progress")
            return;
        auto text = v["stage"].toString().isNotEmpty() ? v["stage"].toString() : juce::String ("Running");
        if (! v["step"].isVoid())
            text += juce::String::fromUTF8 (" \xc2\xb7 step ") + v["step"].toString() + juce::String::fromUTF8 (" \xc2\xb7 loss ") + v["loss"].toString();
        state.setText (text, juce::dontSendNotification);
    };
    worker.onFinished = [this] {
        stopButton.setVisible (false);
        resized();
        if (pendingTrain && dataset.isNotEmpty())
        {
            pendingTrain = false;
            auto* o = new juce::DynamicObject();
            o->setProperty ("dataset", dataset);
            o->setProperty ("epochs", (int) epochs.value());
            startTask ("train", juce::var (o));
        }
    };
    refresh();
}

Collections LoraPage::store() const { return Collections (juce::File (AppSettings::get ("trainingRoot"))); }

void LoraPage::refresh()
{
    const auto old = current;
    collections.clear (juce::dontSendNotification);
    ids.clear();
    for (auto& c : store().list())
    {
        collections.addItem (c["name"].toString() + juce::String::fromUTF8 (" \xc2\xb7 ") + c["kind"].toString() + juce::String::fromUTF8 (" \xc2\xb7 ")
                                 + juce::String (c["items"].getArray() != nullptr ? c["items"].getArray()->size() : 0) + "/" + c["limit"].toString(),
                             ids.size() + 1);
        ids.add (c["id"].toString());
    }
    collections.setSelectedItemIndex (juce::jmax (0, ids.indexOf (old)), juce::dontSendNotification);
    select();
}

void LoraPage::select()
{
    current = ids[collections.getSelectedItemIndex()];
    items.clear();
    dataset = {};
    if (current.isNotEmpty())
    {
        auto c = store().get (current);
        name.setText (c["name"].toString(), false);
        caption.setText (c["caption"].toString(), false);
        limit.setSelectedItemIndex (juce::jmax (0, juce::StringArray { "10", "20", "30" }.indexOf (c["limit"].toString())), juce::dontSendNotification);
        kind.setSelectedItemIndex (c["kind"].toString() == "voice" ? 1 : 0, juce::dontSendNotification);
        if (auto* a = c["items"].getArray())
            items = *a;
    }
    table.updateContent();
    table.repaint();
}

int LoraPage::getNumRows() { return items.size(); }

void LoraPage::paintRowBackground (juce::Graphics& g, int, int w, int h, bool selected)
{
    if (selected)
    {
        g.setColour (col::hover.withAlpha (0.5f));
        g.fillRect (0, 0, w, h);
    }
    g.setColour (col::grid);
    g.fillRect (0, h - 1, w, 1);
}

juce::Component* LoraPage::refreshComponentForCell (int row, int column, bool, juce::Component* existing)
{
    auto* l = dynamic_cast<juce::Label*> (existing);
    if (l == nullptr)
    {
        delete existing;
        l = new juce::Label();
        l->setFont (uiFont (14.0f));
        l->setColour (juce::Label::textColourId, col::text);
        l->setJustificationType (juce::Justification::centred);
    }
    if (row < 0 || row >= items.size())
        return l;
    const auto key = columnKey (column);
    auto v = items.getReference (row)[key];
    l->setText (v.isVoid() ? juce::String() : v.toString(), juce::dontSendNotification);
    l->setEditable (false, column != 1, false);
    l->onTextChange = [this, row, key, l] {
        if (row < items.size())
            items.getReference (row).getDynamicObject()->setProperty (key, l->getText());
    };
    l->onEditorShow = [this, row] { table.selectRow (row); };
    return l;
}

void LoraPage::createCollection()
{
    juce::String newId;
    const auto err = store().create (name.getText(), kind.getSelectedItemIndex() == 1 ? "voice" : "samples", limit.getText().getIntValue(), caption.getText(), newId);
    if (err.isNotEmpty())
    {
        fail (err);
        return;
    }
    current = newId;
    refresh();
    if (! pendingAudio.isEmpty())
    {
        const auto e2 = store().add (current, pendingAudio);
        pendingAudio.clear();
        if (e2.isNotEmpty())
            fail (e2);
        refresh();
    }
}

void LoraPage::renameCollection()
{
    if (current.isEmpty())
        return;
    const auto err = store().rename (current, name.getText(), limit.getText().getIntValue(), caption.getText());
    if (err.isNotEmpty())
        fail (err);
    refresh();
}

void LoraPage::removeSample()
{
    const int r = table.getSelectedRow();
    if (r < 0 || current.isEmpty())
        return;
    store().remove (current, { items[r]["id"].toString() });
    refresh();
}

void LoraPage::addAudio (const juce::StringArray& paths)
{
    if (current.isEmpty())
    {
        pendingAudio.addArray (paths);
        state.setText ("Create a collection (Template name, ...): the track goes into it.", juce::dontSendNotification);
        return;
    }
    const auto err = store().add (current, paths);
    if (err.isNotEmpty())
        fail (err);
    else
        state.setText ("Samples copied; originals kept.", juce::dontSendNotification);
    refresh();
}

void LoraPage::addFiles()
{
    if (current.isEmpty())
    {
        fail ("Create or choose a template first.");
        return;
    }
    chooser = std::make_unique<juce::FileChooser> ("Add samples", juce::File(), "*.wav;*.flac;*.mp3");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc) {
                              juce::StringArray paths;
                              for (auto& f : fc.getResults())
                                  paths.add (f.getFullPathName());
                              if (! paths.isEmpty())
                                  addAudio (paths);
                          });
}

bool LoraPage::saveLabels()
{
    if (current.isEmpty())
        return false;
    auto* changes = new juce::DynamicObject();
    for (auto& s : items)
    {
        auto* e = new juce::DynamicObject();
        for (auto key : { "caption", "lyrics", "language", "lineageGroup" })
            e->setProperty (key, s[key].toString());
        const auto bpm = s["bpm"].toString().trim();
        e->setProperty ("bpm", bpm.isEmpty() ? juce::var() : juce::var (bpm.getDoubleValue()));
        changes->setProperty (juce::Identifier (s["id"].toString()), juce::var (e));
    }
    const auto err = store().update (current, juce::var (changes));
    if (err.isNotEmpty())
    {
        fail (err);
        return false;
    }
    state.setText ("Labels saved.", juce::dontSendNotification);
    return true;
}

void LoraPage::startTask (const juce::String& k, juce::var fields)
{
    const auto err = worker.start (k, fields);
    if (err.isNotEmpty())
    {
        fail (err);
        return;
    }
    state.setText (juce::String::fromUTF8 ("Running\xe2\x80\xa6"), juce::dontSendNotification);
    stopButton.setVisible (true);
    resized();
}

void LoraPage::prepare (bool train)
{
    if (! saveLabels())
        return;
    pendingTrain = train;
    auto* o = new juce::DynamicObject();
    o->setProperty ("collection", current);
    startTask ("prepare-dataset", juce::var (o));
}

void LoraPage::loadAdapter()
{
    chooser = std::make_unique<juce::FileChooser> ("Load adapter to try", juce::File (AppSettings::get ("trainingRoot")), "adapter-manifest.json");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc) {
        if (fc.getResult() == juce::File())
            return;
        auto* o = new juce::DynamicObject();
        o->setProperty ("manifest", fc.getResult().getFullPathName());
        startTask ("load-adapter", juce::var (o));
    });
}

void LoraPage::done (const juce::var& result)
{
    if (result["dataset"].isString() && ! result["manifest"].isVoid())
    {
        dataset = result["dataset"].toString();
        if (! pendingTrain)
            state.setText ("Dataset ready: " + dataset, juce::dontSendNotification);
    }
    else
        state.setText ("Training finished: " + (result["status"].isString() ? result["status"].toString() : juce::String ("result saved")) + " (evaluate before turning it on).",
                       juce::dontSendNotification);
}

void LoraPage::options()
{
    juce::PopupMenu m;
    m.addSectionHeader ("Collection");
    m.addItem ("Create training template", [this] { createCollection(); });
    m.addItem ("Save name / limit", [this] { renameCollection(); });
    m.addItem (juce::String::fromUTF8 ("Add samples\xe2\x80\xa6"), [this] { addFiles(); });
    m.addItem ("Remove selected sample", [this] { removeSample(); });
    m.addItem ("Save labels", [this] { saveLabels(); });
    m.addItem ("Refresh", [this] { refresh(); });
    m.addSectionHeader ("Training");
    m.addItem ("Prepare dataset", [this] { prepare (false); });
    m.addItem ("Create LoRA", [this] { prepare (true); });
    m.addItem (juce::String::fromUTF8 ("Load LoRA to try\xe2\x80\xa6"), [this] { loadAdapter(); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&dots));
}

void LoraPage::paint (juce::Graphics& g)
{
    drawPanel (g, leftBox);
    drawPanel (g, centreBox);
    drawHeading (g, "Collection", leftBox.getX() + 14, leftBox.getY() + 12, leftBox.getWidth() - 28);
    g.setColour (col::title);
    g.setFont (uiFont (15.0f, true).withExtraKerningFactor (0.1f));
    g.drawText (aicore::ui::tr ("SAMPLES"), centreBox.getX() + 20, centreBox.getY() + 14, 300, 28, juce::Justification::centredLeft);
}

void LoraPage::resized()
{
    auto r = getLocalBounds().reduced (4, 0).withTrimmedTop (8).withTrimmedBottom (4);
    auto foot = r.removeFromBottom (24);
    if (stopButton.isVisible())
        stopButton.setBounds (foot.removeFromRight (stopButton.idealWidth (26)));
    state.setBounds (foot.withTrimmedLeft (-4));
    r.removeFromBottom (6);
    leftBox = r.removeFromLeft (320);
    r.removeFromLeft (8);
    centreBox = r;
    auto l = leftBox.reduced (14, 0).withTrimmedTop (50).withTrimmedBottom (14);
    collections.setBounds (l.removeFromTop (28));
    l.removeFromTop (14);
    for (auto [lab, field] : { std::pair<juce::Label*, juce::Component*> { &nameLabel, &name }, { &kindLabel, &kind }, { &limitLabel, &limit }, { &captionLabel, &caption } })
    {
        lab->setBounds (l.removeFromTop (22).withTrimmedLeft (-4));
        l.removeFromTop (4);
        field->setBounds (l.removeFromTop (28));
        l.removeFromTop (8);
    }
    l.removeFromTop (4);
    epochs.setBounds (l.removeFromTop (26));
    go.setBounds (l.removeFromBottom (30));
    auto c = centreBox.reduced (16, 0).withTrimmedTop (14).withTrimmedBottom (14);
    auto head = c.removeFromTop (30);
    dots.setBounds (head.removeFromRight (36));
    head.removeFromRight (8);
    addButton.setBounds (head.removeFromRight (addButton.idealWidth (26)));
    c.removeFromTop (8);
    table.setBounds (c);
}
} // namespace aicore::lora
