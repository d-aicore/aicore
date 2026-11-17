#include "DatabasePage.h"
#include "../ui/I18n.h"

#include "../ServiceJob.h"
#include "../ui/AppSettings.h"
#include "../ui/Worker.h"

namespace aicore::database
{
using namespace aicore::ui;

namespace
{
const std::pair<const char*, const char*> kVerdicts[] = { { "aprovado", "Approved" }, { "chumbado", "Rejected" }, { "catalogo", "Catalogue only" } };

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
        g.drawText (n, 8, 0, w - 8, h, juce::Justification::centredLeft);
    }
};
TableLook& tableLook()
{
    static TableLook look;
    return look;
}
} // namespace

juce::var DatabasePage::tool (const juce::StringArray& args)
{
    auto script = servicesRoot().getChildFile ("services").getChildFile ("database").getChildFile ("aicore_db_tool.py");
    juce::StringArray cmd { Worker::python().getFullPathName(), "-I", script.getFullPathName() };
    cmd.addArray (args);
    juce::ChildProcess child;
    juce::var v;
    if (child.start (cmd, juce::ChildProcess::wantStdOut))
    {
        const auto text = child.readAllProcessOutput();
        child.waitForProcessToFinish (120000);
        for (auto& line : juce::StringArray::fromLines (text))
            if (line.trim().startsWithChar ('{') && juce::JSON::parse (line.trim(), v).wasOk())
                return v;
    }
    auto* o = new juce::DynamicObject();
    o->setProperty ("ok", false);
    o->setProperty ("error", "AICore's Python could not run the database tool.");
    return juce::var (o);
}

DatabasePage::DatabasePage()
{
    setWantsKeyboardFocus (true);
    title.setText ("Template examples", juce::dontSendNotification);
    addTitle.setText ("Add a template and its music", juce::dontSendNotification);
    for (auto* l : { &title, &addTitle, &state })
    {
        l->setFont (uiFont (15.0f));
        l->setColour (juce::Label::textColourId, col::text);
        addAndMakeVisible (l);
    }
    library.setText (juce::File (AppSettings::get ("trainingRoot")).getChildFile ("exemplos").getFullPathName(), false);
    sections.setTextToShowWhenEmpty ("All, or e.g. Intro|Drop", col::dim);
    tracks.setTextToShowWhenEmpty ("All, or e.g. lead|bass", col::dim);
    for (auto* t : { &library, &templateField, &audio, &sections, &tracks, &why })
    {
        t->setFont (uiFont (15.0f));
        t->applyFontToAllText (uiFont (15.0f));
        t->setIndents (8, 5);
    }
    templateField.onTextChange = [this] {
        juce::File f (templateField.getText().trim());
        if (f.existsAsFile() && audio.getText().trim().isEmpty())
        {
            auto v = tool ({ "companions", f.getFullPathName() });
            if ((bool) v["ok"] && v["audio"].toString().isNotEmpty())
                audio.setText (v["audio"].toString(), false);
        }
    };
    for (auto& [code, label] : kVerdicts)
        verdict.addItem (label, verdict.getNumItems() + 1);
    verdict.setSelectedItemIndex (0, juce::dontSendNotification);
    verdict.setColour (juce::ComboBox::backgroundColourId, col::field);
    verdict.setColour (juce::ComboBox::textColourId, col::text);
    verdict.setColour (juce::ComboBox::outlineColourId, col::outline);

    auto row = [this] (Row& r, const char* label, juce::Component& field, int button, juce::TextEditor* target, const juce::String& pattern) {
        r.label.setText (label, juce::dontSendNotification);
        r.label.setFont (uiFont (15.0f));
        r.label.setColour (juce::Label::textColourId, col::text);
        r.field = &field;
        if (button == 1)
            r.button = std::make_unique<sample::IconButton> (sample::IconButton::folder);
        else if (button == 2)
            r.button = std::make_unique<AButton> (juce::String::fromUTF8 ("Choose\xe2\x80\xa6"));
        if (r.button != nullptr)
        {
            r.button->onClick = [this, target, pattern, button] {
                chooser = std::make_unique<juce::FileChooser> (button == 1 ? "Choose folder" : "Choose a file", juce::File (target->getText()), pattern);
                chooser->launchAsync (juce::FileBrowserComponent::openMode | (button == 1 ? juce::FileBrowserComponent::canSelectDirectories : juce::FileBrowserComponent::canSelectFiles),
                                      [target] (const juce::FileChooser& fc) {
                                          if (fc.getResult() != juce::File())
                                              target->setText (fc.getResult().getFullPathName());
                                      });
            };
            addAndMakeVisible (*r.button);
        }
        addAndMakeVisible (r.label);
        addAndMakeVisible (field);
    };
    row (libraryRow, "Examples library", library, 1, &library, {});
    row (templateRow, "Template (.core-template.json)", templateField, 2, &templateField, "*.core-template.json");
    row (audioRow, "Music (WAV, optional)", audio, 2, &audio, "*.wav;*.flac;*.mp3");
    row (verdictRow, "Verdict", verdict, 0, nullptr, {});
    row (sectionsRow, "Only these sections", sections, 0, nullptr, {});
    row (tracksRow, "Only these tracks", tracks, 0, nullptr, {});
    row (whyRow, "Why", why, 0, nullptr, {});
    updateButton.onClick = [this] { updateDatabase(); };
    addButton.onClick = [this] { add(); };
    addAndMakeVisible (updateButton);
    addAndMakeVisible (addButton);
    auto& h = table.getHeader();
    int id = 1;
    for (auto t : { "Template", "Style", "Verdict", "Music", "Sheet" })
        h.addColumn (t, id++, juce::String (t) == "Music" ? 600 : 80, 50, -1, juce::TableHeaderComponent::notSortable);
    h.setPopupMenuActive (false);
    h.setLookAndFeel (&tableLook());
    table.setHeaderHeight (44);
    table.setRowHeight (28);
    table.setColour (juce::ListBox::backgroundColourId, col::field);
    table.setColour (juce::ListBox::outlineColourId, col::outline);
    table.setOutlineThickness (1);
    addAndMakeVisible (table);
    refresh();
}

void DatabasePage::refresh()
{
    auto v = tool ({ "summary", library.getText().trim() });
    rows.clear();
    if ((bool) v["ok"] && v["rows"].getArray() != nullptr)
        rows = *v["rows"].getArray();
    table.updateContent();
    table.repaint();
}

void DatabasePage::updateDatabase()
{
    auto v = tool ({ "rebuild", library.getText().trim(), AppSettings::get ("coreTemplatesRoot") });
    if (! (bool) v["ok"])
    {
        state.setText (v["error"].toString(), juce::dontSendNotification);
        return;
    }
    state.setText ("Database ready: " + v["templates"].toString() + " templates, " + v["exemplos"].toString() + " examples.", juce::dontSendNotification);
    refresh();
}

void DatabasePage::add()
{
    juce::File tpl (templateField.getText().trim());
    if (! tpl.existsAsFile())
    {
        state.setText ("Choose a template file first.", juce::dontSendNotification);
        return;
    }
    auto v = tool ({ "add", library.getText().trim(), AppSettings::get ("coreTemplatesRoot"), tpl.getFullPathName(),
                     kVerdicts[juce::jlimit (0, 2, verdict.getSelectedItemIndex())].first, why.getText().trim(), audio.getText().trim(),
                     sections.getText().trim(), tracks.getText().trim() });
    if (! (bool) v["ok"])
    {
        state.setText (v["error"].toString(), juce::dontSendNotification);
        return;
    }
    state.setText ("Added: " + v["id"].toString() + ". Database: " + v["exemplos"].toString() + " examples.", juce::dontSendNotification);
    for (auto* t : { &templateField, &audio, &sections, &tracks, &why })
        t->clear();
    refresh();
}

void DatabasePage::paintRowBackground (juce::Graphics& g, int, int w, int h, bool)
{
    g.setColour (col::grid);
    g.fillRect (0, h - 1, w, 1);
}

void DatabasePage::paintCell (juce::Graphics& g, int row, int column, int w, int h, bool)
{
    if (row < 0 || row >= rows.size())
        return;
    const auto& r = rows.getReference (row);
    juce::String text;
    switch (column)
    {
        case 1: text = r["id"].toString(); break;
        case 2: text = r["estilo"].toString(); break;
        case 3:
        {
            const auto code = r["veredito"].toString();
            text = code == "aprovado" ? "Approved" : code == "chumbado" ? "Rejected" : code == "catalogo" ? "Catalogue only" : code == "partes" ? "Some parts" : code;
            break;
        }
        case 4: text = r["audio"].toString(); break;
        default: text = (bool) r["ficha"] ? "yes" : ""; break;
    }
    g.setColour (col::text);
    g.setFont (uiFont (14.0f));
    g.drawText (text, 8, 0, w - 10, h, juce::Justification::centredLeft, true);
}

void DatabasePage::paint (juce::Graphics& g)
{
    g.setColour (col::accent);
    g.fillEllipse ((float) title.getX() + 4 + juce::GlyphArrangement::getStringWidth (uiFont (15.0f), "Template examples") + 10, (float) title.getBounds().getCentreY() - 3, 5, 5);
}

void DatabasePage::resized()
{
    auto r = getLocalBounds().withTrimmedLeft (9).withTrimmedRight (9).withTrimmedTop (8).withTrimmedBottom (8);
    title.setBounds (r.removeFromTop (24).withTrimmedLeft (-4));
    r.removeFromTop (6);
    auto place = [&] (Row& row, int labelW) {
        auto line = r.removeFromTop (28);
        row.label.setBounds (line.removeFromLeft (labelW).withTrimmedLeft (-4));
        if (row.button != nullptr)
        {
            const int bw = dynamic_cast<AButton*> (row.button.get()) != nullptr ? ((AButton*) row.button.get())->idealWidth (26) : 36;
            row.button->setBounds (line.removeFromRight (bw));
            line.removeFromRight (6);
        }
        row.field->setBounds (line);
    };
    place (libraryRow, 115);
    r.removeFromTop (8);
    updateButton.setBounds (r.removeFromTop (30).withWidth (updateButton.idealWidth (26)));
    r.removeFromTop (2);
    addTitle.setBounds (r.removeFromTop (24).withTrimmedLeft (-4));
    r.removeFromTop (4);
    for (auto* row : { &templateRow, &audioRow, &verdictRow, &sectionsRow, &tracksRow, &whyRow })
    {
        place (*row, 207);
        r.removeFromTop (6);
    }
    addButton.setBounds (r.removeFromTop (30).withWidth (addButton.idealWidth (26)));
    r.removeFromTop (6);
    state.setBounds (r.removeFromTop (20).withTrimmedLeft (-4));
    r.removeFromTop (6);
    table.setBounds (r);
}
} // namespace aicore::database
