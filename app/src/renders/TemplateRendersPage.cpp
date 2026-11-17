#include "TemplateRendersPage.h"
#include "../ui/I18n.h"

#include "../ServiceJob.h"
#include "../ui/AppSettings.h"

namespace aicore::renders
{
using namespace aicore::ui;

juce::File TemplateRendersPage::renderer()
{
    auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    for (auto f : { exe.getChildFile ("aicore-render.exe"), exe.getParentDirectory().getParentDirectory().getChildFile ("aicore-render_artefacts").getChildFile ("Release").getChildFile ("aicore-render.exe") })
        if (f.existsAsFile())
            return f;
    return exe.getChildFile ("aicore-render.exe");
}

TemplateRendersPage::TemplateRendersPage()
{
    setWantsKeyboardFocus (true);
    auto setup = [this] (Row& r, const char* label, const juce::String& text, const juce::String& hint, bool chooseButton, bool folder) {
        r.label.setText (label, juce::dontSendNotification);
        r.label.setFont (uiFont (15.0f));
        r.label.setColour (juce::Label::textColourId, col::text);
        r.field.setFont (uiFont (15.0f));
        r.field.setIndents (8, 5);
        r.field.setText (text, false);
        r.field.setTextToShowWhenEmpty (hint, col::dim);
        if (chooseButton)
            r.button = std::make_unique<AButton> (juce::String::fromUTF8 ("Choose\xe2\x80\xa6"));
        else
            r.button = std::make_unique<sample::IconButton> (sample::IconButton::folder);
        auto* field = &r.field;
        r.button->onClick = [this, field, folder, chooseButton] {
            if (chooseButton)
            {
                chooser = std::make_unique<juce::FileChooser> ("Choose MIDI", juce::File (field->getText().upToFirstOccurrenceOf (";", false, false)), "*.mid;*.midi");
                chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                                      [field] (const juce::FileChooser& fc) {
                                          juce::StringArray paths;
                                          for (auto& f : fc.getResults())
                                              paths.add (f.getFullPathName());
                                          if (! paths.isEmpty())
                                              field->setText (paths.joinIntoString (";"));
                                      });
                return;
            }
            chooser = std::make_unique<juce::FileChooser> (folder ? "Choose folder" : "Choose file", juce::File (field->getText()));
            chooser->launchAsync (juce::FileBrowserComponent::openMode | (folder ? juce::FileBrowserComponent::canSelectDirectories : juce::FileBrowserComponent::canSelectFiles),
                                  [field] (const juce::FileChooser& fc) {
                                      if (fc.getResult() != juce::File())
                                          field->setText (fc.getResult().getFullPathName());
                                  });
        };
        addAndMakeVisible (r.label);
        addAndMakeVisible (r.field);
        addAndMakeVisible (*r.button);
    };
    setup (templates, "Sound templates", juce::File (AppSettings::get ("coreTemplatesRoot")).getChildFile ("renders").getFullPathName(),
           "Folder with the sound templates (aicore-render-template .json)", false, true);
    setup (mids, "MIDI", {}, "Guitar Pro .mid files (Studio > MIDI)", true, false);
    setup (programs, "Renderer", renderer().getFullPathName(), "aicore-render.exe (AICore's own VST3 renderer)", false, false);
    setup (out, "Save in", juce::File (AppSettings::get ("trainingRoot")).getChildFile ("template-renders").getChildFile (juce::Time::getCurrentTime().formatted ("%Y-%m-%d")).getFullPathName(),
           {}, false, true);
    title.setText (juce::String::fromUTF8 ("Template renders \xc2\xb7 MIDI training"), juce::dontSendNotification);
    state.setText ("Ready.", juce::dontSendNotification);
    for (auto* l : { &title, &state })
    {
        l->setFont (uiFont (15.0f));
        l->setColour (juce::Label::textColourId, col::text);
        addAndMakeVisible (l);
    }
    go.onClick = [this] { start(); };
    stopButton.onClick = [this] { worker.stop(); };
    addAndMakeVisible (go);
    addChildComponent (stopButton);
    addAndMakeVisible (perTemplate);
    addAndMakeVisible (bars);
    worker.onError = [this] (const juce::String& m) { state.setText (m, juce::dontSendNotification); };
    worker.onEvent = [this] (const juce::var& v) {
        if (v["event"].toString() == "progress")
        {
            const double f = v["fraction"];
            state.setText (v["stage"].toString() + (f > 0 ? juce::String::fromUTF8 (" \xc2\xb7 ") + juce::String (juce::roundToInt (f * 100)) + "%" : juce::String()),
                           juce::dontSendNotification);
        }
    };
    worker.onDone = [this] (const juce::var& r) {
        state.setText ("Done: " + r["pieces"].toString() + juce::String::fromUTF8 (" \xc3\x97 4 s \xc2\xb7 ") + r["renders"].toString()
                           + juce::String::fromUTF8 (" renders \xc2\xb7 ") + r["folder"].toString(),
                       juce::dontSendNotification);
    };
    worker.onFinished = [this] { stopButton.setVisible (false); resized(); };
}

void TemplateRendersPage::start()
{
    juce::StringArray list;
    list.addTokens (mids.field.getText(), ";", "");
    list.trim();
    list.removeEmptyStrings();
    if (list.isEmpty())
    {
        state.setText ("Choose at least one MIDI file.", juce::dontSendNotification);
        return;
    }
    auto* o = new juce::DynamicObject();
    o->setProperty ("templates", templates.field.getText().trim());
    juce::Array<juce::var> m;
    for (auto& s : list)
        m.add (s);
    o->setProperty ("mids", m);
    o->setProperty ("renderer", programs.field.getText().trim());
    o->setProperty ("destination", out.field.getText().trim());
    o->setProperty ("perTemplate", (int) perTemplate.value());
    o->setProperty ("bars", (int) bars.value());
    const auto script = servicesRoot().getChildFile ("services").getChildFile ("render").getChildFile ("aicore_template_renders.py");
    const auto err = worker.startScript (script, juce::var (o));
    if (err.isNotEmpty())
    {
        state.setText (err, juce::dontSendNotification);
        return;
    }
    state.setText (juce::String::fromUTF8 ("Running\xe2\x80\xa6"), juce::dontSendNotification);
    stopButton.setVisible (true);
    resized();
}

void TemplateRendersPage::paint (juce::Graphics&) {}

void TemplateRendersPage::resized()
{
    auto r = getLocalBounds().withTrimmedLeft (9).withTrimmedRight (9).withTrimmedTop (6);
    auto head = r.removeFromTop (30);
    go.setBounds (head.removeFromRight (go.idealWidth (28)));
    if (stopButton.isVisible())
    {
        head.removeFromRight (6);
        stopButton.setBounds (head.removeFromRight (stopButton.idealWidth (26)));
    }
    title.setBounds (head.withTrimmedLeft (-4));
    r.removeFromTop (6);
    for (auto* row : { &templates, &mids, &programs, &out })
    {
        auto line = r.removeFromTop (28);
        row->label.setBounds (line.removeFromLeft (118).withTrimmedLeft (-4));
        const bool text = dynamic_cast<AButton*> (row->button.get()) != nullptr;
        row->button->setBounds (line.removeFromRight (text ? ((AButton*) row->button.get())->idealWidth (26) : 36));
        line.removeFromRight (6);
        row->field.setBounds (line);
        r.removeFromTop (8);
    }
    auto knobs = r.removeFromTop (106);
    perTemplate.setBounds (knobs.removeFromLeft (84).withTrimmedLeft (-2));
    knobs.removeFromLeft (6);
    bars.setBounds (knobs.removeFromLeft (84));
    state.setBounds (r.removeFromTop (24).withTrimmedLeft (-4));
}
} // namespace aicore::renders
