#include "SettingsPages.h"
#include "../ui/I18n.h"

#include "../renders/TemplateRendersPage.h"
#include "../ui/Panels.h"

namespace aicore::settings
{
using namespace aicore::ui;

namespace
{
void styleLabel (juce::Label& l, float px = 15.0f, bool bold = false, juce::Colour c = col::text)
{
    l.setFont (uiFont (px, bold));
    l.setColour (juce::Label::textColourId, c);
}

void styleField (juce::TextEditor& t)
{
    t.setFont (uiFont (15.0f));
    t.applyFontToAllText (uiFont (15.0f));
    t.setIndents (8, 5);
}

void styleCombo (juce::ComboBox& c)
{
    c.setColour (juce::ComboBox::backgroundColourId, col::field);
    c.setColour (juce::ComboBox::textColourId, col::text);
    c.setColour (juce::ComboBox::outlineColourId, col::outline);
}

void setupPath (juce::Component& owner, PathRow& r, const char* label, const juce::String& value, bool folder, std::unique_ptr<juce::FileChooser>& chooser)
{
    r.label.setText (label, juce::dontSendNotification);
    styleLabel (r.label);
    styleField (r.field);
    r.field.setText (value, false);
    r.folder = folder;
    auto* field = &r.field;
    r.pick.onClick = [field, folder, &chooser] {
        chooser = std::make_unique<juce::FileChooser> (folder ? "Choose folder" : "Choose python.exe", juce::File (field->getText()), folder ? juce::String() : juce::String ("*.exe"));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | (folder ? juce::FileBrowserComponent::canSelectDirectories : juce::FileBrowserComponent::canSelectFiles),
                              [field] (const juce::FileChooser& fc) {
                                  if (fc.getResult() != juce::File())
                                      field->setText (fc.getResult().getFullPathName());
                              });
    };
    owner.addAndMakeVisible (r.label);
    owner.addAndMakeVisible (r.field);
    owner.addAndMakeVisible (r.pick);
}
} // namespace

//============================================================================== FOLDERS
FoldersPage::FoldersPage()
{
    setWantsKeyboardFocus (true);
    languageLabel.setText ("Language", juce::dontSendNotification);
    urlLabel.setText ("Local ACE (advanced)", juce::dontSendNotification);
    runtimeLabel.setText ("ACE environment", juce::dontSendNotification);
    for (auto* l : { &languageLabel, &languageNote, &urlLabel, &runtimeLabel, &state })
    {
        styleLabel (*l);
        addAndMakeVisible (l);
    }
    styleLabel (languageNote, 14.0f, false, col::muted);
    language.addItemList ({ "English", juce::String::fromUTF8 ("Portugu\xc3\xaas") }, 1);
    runtime.addItemList ({ "Native Windows/Linux", "ACE in WSL" }, 1);
    styleCombo (runtime);
    addAndMakeVisible (runtime);
    languageLabel.setVisible (false); // Diogo 2026-11-16: the interface is English only, so no language choice
    languageNote.setVisible (false);
    language.onChange = [this] {
        AppSettings::set ("language", language.getSelectedItemIndex() == 1 ? "pt" : "en");
        languageNote.setText ("Saved. AICORE opens in this language next time.", juce::dontSendNotification);
    };
    styleField (url);
    addAndMakeVisible (url);
    setupPath (*this, training, "Training files and templates", {}, true, chooser);
    setupPath (*this, workspace, "Projects, stems and exports", {}, true, chooser);
    setupPath (*this, samples, "Samples folder", {}, true, chooser);
    setupPath (*this, python, "Existing Demucs Python (optional)", {}, false, chooser);
    saveButton.onClick = [this] { save(); };
    addAndMakeVisible (saveButton);
    load();
}

void FoldersPage::load()
{
    language.setSelectedItemIndex (AppSettings::get ("language") == "pt" ? 1 : 0, juce::dontSendNotification);
    training.field.setText (AppSettings::get ("trainingRoot"), false);
    workspace.field.setText (AppSettings::get ("workspaceRoot"), false);
    samples.field.setText (AppSettings::get ("sampleRoot"), false);
    python.field.setText (AppSettings::get ("demucsPython"), false);
    url.setText (AppSettings::get ("aceUrl"), false);
    runtime.setSelectedItemIndex (AppSettings::get ("aceRuntime") == "wsl" ? 1 : 0, juce::dontSendNotification);
}

void FoldersPage::save()
{
    for (auto* f : { &training.field, &workspace.field })
        if (! juce::File::isAbsolutePath (f->getText().trim()))
        {
            state.setText ("Choose absolute folders for data, projects and models.", juce::dontSendNotification);
            return;
        }
    AppSettings::set ("trainingRoot", training.field.getText().trim());
    AppSettings::set ("workspaceRoot", workspace.field.getText().trim());
    AppSettings::set ("sampleRoot", samples.field.getText().trim());
    AppSettings::set ("demucsPython", python.field.getText().trim());
    AppSettings::set ("aceUrl", url.getText().trim());
    AppSettings::set ("aceRuntime", runtime.getSelectedItemIndex() == 1 ? "wsl" : "native");
    state.setText ("Settings saved. Existing data stays in the previous folders.", juce::dontSendNotification);
    if (onSaved) onSaved();
}

void FoldersPage::paint (juce::Graphics& g)
{
    auto box = getLocalBounds().withTrimmedTop (2).withTrimmedBottom (4);
    drawPanel (g, box);
    drawHeading (g, "Folders", 12, box.getY() + 8, getWidth() - 24);
}

void FoldersPage::resized()
{
    auto r = getLocalBounds().withTrimmedTop (2).reduced (9, 0).withTrimmedTop (46);
    for (auto* row : { &training, &workspace, &samples, &python })
    {
        auto l = r.removeFromTop (28);
        row->label.setBounds (l.removeFromLeft (233).withTrimmedLeft (-4));
        row->pick.setBounds (l.removeFromRight (36));
        l.removeFromRight (6);
        row->field.setBounds (l);
        r.removeFromTop (8);
    }
    auto u = r.removeFromTop (28);
    urlLabel.setBounds (u.removeFromLeft (233).withTrimmedLeft (-4));
    url.setBounds (u);
    r.removeFromTop (6);
    auto rt = r.removeFromTop (28);
    runtimeLabel.setBounds (rt.removeFromLeft (233).withTrimmedLeft (-4));
    runtime.setBounds (rt);
    r.removeFromTop (6);
    saveButton.setBounds (r.removeFromTop (30));
    r.removeFromTop (4);
    state.setBounds (r.removeFromTop (24).withTrimmedLeft (-4));
}

//============================================================================== MODELS
ModelsPage::ModelsPage()
{
    setWantsKeyboardFocus (true);
    for (auto [l, t] : { std::pair<juce::Label*, const char*> { &accountLabel, "Account" }, { &tokenLabel, "Token" }, { &folderLabel, "Folder" },
                         { &fromLabel, "Download from" }, { &mirrorLabel, "Mirror" }, { &localTitle, "Local models" }, { &workerLabel, "Training worker" },
                         { &workerNote, "Stopped." }, { &state, "Ready." }, { &account, "Not linked" } })
    {
        l->setText (t, juce::dontSendNotification);
        styleLabel (*l);
        addAndMakeVisible (l);
    }
    styleLabel (account, 15.0f, true);
    styleLabel (setupState, 14.0f, false, col::muted);
    addAndMakeVisible (setupState);
    for (auto* t : { &token, &folder, &mirror })
    {
        styleField (*t);
        addAndMakeVisible (t);
    }
    token.setPasswordCharacter ((juce::juce_wchar) 0x2022);
    mirror.setTextToShowWhenEmpty ("https://mirror.example", col::dim);
    folder.setText (AppSettings::get ("modelsRoot"), false);
    mirror.setText (AppSettings::get ("modelSourceUrl"), false);
    from.addItemList ({ "Hugging Face (official)", "Custom HF-compatible mirror" }, 1);
    from.setSelectedItemIndex (AppSettings::get ("modelSource") == "mirror" ? 1 : 0, juce::dontSendNotification);
    from.onChange = [this] { mirror.setEnabled (from.getSelectedItemIndex() == 1); };
    mirror.setEnabled (from.getSelectedItemIndex() == 1);
    styleCombo (from);
    addAndMakeVisible (from);
    check.onClick = [this] { checkAccount(); };
    linkButton.onClick = [this] { link(); };
    unlinkButton.onClick = [this] { unlink(); };
    change.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser> ("Choose folder", juce::File (folder.getText()));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc) {
            if (fc.getResult() != juce::File())
                folder.setText (fc.getResult().getFullPathName());
        });
    };
    save.onClick = [this] { saveFolder(); };
    sourcesButton.onClick = [this] { sources(); };
    workerStart.onClick = [this] { startWorker(); };
    workerStop.onClick = [this] { stopWorker(); };
    workerStop.setEnabled (false);
    stopButton.onClick = [this] { worker.stop(); };
    dots.onClick = [this] {
        juce::PopupMenu m;
        m.addSectionHeader ("ACE");
        m.addItem ("Install ACE 1.5", [this] { startTask ("install-ace"); });
        m.addItem ("Start ACE", [this] { models[0]->buttons[2]->onClick(); });
        m.addItem ("Check ACE", [this] { startTask ("probe-ace"); });
        m.addItem ("Stop own ACE", [this] { service.stop(); });
        m.addSectionHeader ("Demucs");
        m.addItem ("Install Demucs", [this] { startTask ("install-demucs"); });
        m.addItem ("Check Demucs", [this] { startTask ("probe-demucs"); });
        m.addSectionHeader ("Music Flamingo");
        m.addItem ("Download / install", false, false, [] {});
        m.addItem ("Check installation", false, false, [] {});
        m.addItem (juce::String::fromUTF8 ("Download sources\xe2\x80\xa6"), [this] { sources(); });
        m.addSeparator();
        m.addItem ("Reconcile previous job", [this] { startTask ("reconcile"); });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&dots));
    };
    for (juce::Component* c : { (juce::Component*) &check, (juce::Component*) &linkButton, (juce::Component*) &unlinkButton, (juce::Component*) &change,
                                (juce::Component*) &save, (juce::Component*) &sourcesButton, (juce::Component*) &workerStart, (juce::Component*) &workerStop,
                                (juce::Component*) &dots, (juce::Component*) &modelsView })
        addAndMakeVisible (c);
    addChildComponent (stopButton);
    modelsView.setViewedComponent (&modelsBody, false);
    modelsView.setScrollBarsShown (true, false);
    const char* defs[][4] = { { "ace", "ACE-Step 1.5", "install-ace", "probe-ace" },
                              { "flamingo", "Music Flamingo", "install-flamingo", "probe-flamingo" },
                              { "demucs", "Demucs / HTDemucs", "install-demucs", "probe-demucs" } };
    for (auto& d : defs)
    {
        auto* m = models.add (new Model());
        m->key = d[0];
        m->title = d[1];
        m->install = d[2];
        m->probe = d[3];
        m->state.setText (m->key == "flamingo" ? "Paused for now (Flamingo comes back later)." : "Not checked.", juce::dontSendNotification);
        styleLabel (m->state);
        modelsBody.addAndMakeVisible (m->state);
        auto addButton = [&] (const juce::String& text, std::function<void()> fn) {
            auto* b = m->buttons.add (new AButton (text, std::move (fn)));
            modelsBody.addAndMakeVisible (b);
            return b;
        };
        const auto install = m->install, probe = m->probe;
        auto* bi = addButton ("Download / install", [this, install] {
            auto* o = new juce::DynamicObject();
            if (install == "install-demucs")
                o->setProperty ("model", demucsModel.getText());
            startTask (install, juce::var (o));
        });
        auto* bp = addButton ("Check installation", [this, probe] {
            auto* o = new juce::DynamicObject();
            if (probe == "probe-demucs")
                o->setProperty ("model", demucsModel.getText());
            startTask (probe, juce::var (o));
        });
        if (m->key == "flamingo")
        {
            bi->setEnabled (false); // Diogo 2026-11-16: Flamingo paused
            bp->setEnabled (false);
        }
        if (m->key == "ace")
        {
            addButton ("Start ACE", [this] {
                const auto err = service.start ("serve-ace", juce::var (new juce::DynamicObject()));
                state.setText (err.isNotEmpty() ? err : juce::String ("ACE loading; the first use may download weights. It does not start training."),
                               juce::dontSendNotification);
            });
            addButton ("Stop own ACE", [this] { service.stop(); });
        }
    }
    demucsModel.addItemList ({ "htdemucs", "htdemucs_ft", "htdemucs_6s" }, 1);
    demucsModel.setSelectedItemIndex (0, juce::dontSendNotification);
    styleCombo (demucsModel);
    modelsBody.addAndMakeVisible (demucsModel);
    worker.onError = [this] (const juce::String& e) { state.setText (e, juce::dontSendNotification); };
    worker.onEvent = [this] (const juce::var& v0) {
        auto v = v0["event"].toString() == "provider" ? v0["value"] : v0;
        if (v["event"].toString() == "progress")
            state.setText (v["stage"].toString().isNotEmpty() ? v["stage"].toString() : juce::String ("Running"), juce::dontSendNotification);
    };
    worker.onDone = [this] (const juce::var& r) { modelDone (r); };
    worker.onFinished = [this] {
        stopButton.setVisible (false);
        for (auto* m : models)
            for (auto* b : m->buttons)
                b->setEnabled (m->key != "flamingo");
        resized();
    };
    service.onError = [this] (const juce::String& e) { state.setText (e, juce::dontSendNotification); };
    service.onDone = [this] (const juce::var&) { state.setText ("Own ACE stopped.", juce::dontSendNotification); };
    showToken();
}

ModelsPage::~ModelsPage()
{
    stopTimer();
    stopWorker();
}

juce::File ModelsPage::tokenFile() const
{
    return juce::File (AppSettings::get ("modelsRoot")).getChildFile ("huggingface").getChildFile ("token");
}

void ModelsPage::showToken()
{
    const auto t = tokenFile().loadFileAsString().trim();
    token.clear();
    token.setTextToShowWhenEmpty (t.length() > 8 ? t.substring (0, 3) + juce::String::repeatedString (juce::String::fromUTF8 ("\xe2\x80\xa2"), 16) + t.getLastCharacters (3)
                                                 : juce::String::fromUTF8 ("hf_\xe2\x80\xa6"),
                                  col::dim);
    account.setText (t.isNotEmpty() ? "Linked" : "Not linked", juce::dontSendNotification);
    unlinkButton.setEnabled (t.isNotEmpty());
    check.setEnabled (t.isNotEmpty());
}

void ModelsPage::link()
{
    const auto t = token.getText().trim();
    if (! t.startsWith ("hf_") || t.length() < 20 || t.containsAnyOf (" \t\r\n"))
    {
        setupState.setText ("A Hugging Face token starts with hf_ (huggingface.co, Settings, Access Tokens).", juce::dontSendNotification);
        return;
    }
    tokenFile().getParentDirectory().createDirectory();
    if (! tokenFile().replaceWithText (t))
    {
        setupState.setText ("Could not save the token.", juce::dontSendNotification);
        return;
    }
    showToken();
    checkAccount();
}

void ModelsPage::unlink()
{
    tokenFile().deleteFile();
    showToken();
    setupState.setText ("Unlinked.", juce::dontSendNotification);
}

void ModelsPage::checkAccount()
{
    const auto t = tokenFile().loadFileAsString().trim();
    if (t.isEmpty())
        return;
    account.setText (juce::String::fromUTF8 ("Checking\xe2\x80\xa6"), juce::dontSendNotification);
    auto base = (AppSettings::get ("modelSource") == "mirror" && AppSettings::get ("modelSourceUrl").isNotEmpty()) ? AppSettings::get ("modelSourceUrl")
                                                                                                                    : juce::String ("https://huggingface.co");
    juce::Component::SafePointer<ModelsPage> safe (this);
    juce::Thread::launch ([safe, t, base] {
        juce::String name, error;
        int status = 0;
        auto stream = juce::URL (base.trimCharactersAtEnd ("/") + "/api/whoami-v2")
                          .createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                  .withExtraHeaders ("Authorization: Bearer " + t)
                                                  .withConnectionTimeoutMs (15000)
                                                  .withStatusCode (&status));
        if (stream != nullptr && status == 200)
            name = juce::JSON::parse (stream->readEntireStreamAsString())["name"].toString();
        else
            error = status > 0 ? "HTTP " + juce::String (status) : juce::String ("no connection");
        juce::MessageManager::callAsync ([safe, name, error] {
            if (safe == nullptr)
                return;
            if (name.isNotEmpty())
            {
                safe->account.setText (name + juce::String::fromUTF8 (" \xc2\xb7 linked"), juce::dontSendNotification);
                safe->setupState.setText ({}, juce::dontSendNotification);
            }
            else
            {
                safe->account.setText ("Linked", juce::dontSendNotification);
                safe->setupState.setText ("Could not reach Hugging Face: " + error, juce::dontSendNotification);
            }
        });
    });
}

void ModelsPage::saveFolder()
{
    if (worker.running())
    {
        setupState.setText ("Stop the jobs before changing folders or environment.", juce::dontSendNotification);
        return;
    }
    const auto oldToken = tokenFile().loadFileAsString().trim();
    if (! juce::File::isAbsolutePath (folder.getText().trim()))
    {
        setupState.setText ("Choose an absolute folder for the models.", juce::dontSendNotification);
        return;
    }
    AppSettings::set ("modelsRoot", folder.getText().trim());
    AppSettings::set ("modelSource", from.getSelectedItemIndex() == 1 ? "mirror" : "huggingface");
    AppSettings::set ("modelSourceUrl", mirror.getText().trim());
    if (oldToken.isNotEmpty() && ! tokenFile().existsAsFile())
    {
        tokenFile().getParentDirectory().createDirectory();
        tokenFile().replaceWithText (oldToken); // the account follows the models to their new folder
    }
    showToken();
    setupState.setText ("Saved. Models already downloaded stay in the previous folder.", juce::dontSendNotification);
}

void ModelsPage::startTask (const juce::String& kind, juce::var fields)
{
    if (worker.running())
        return;
    modelKind = kind;
    const auto err = worker.start (kind, fields.isObject() ? fields : juce::var (new juce::DynamicObject()));
    if (err.isNotEmpty())
    {
        state.setText (err, juce::dontSendNotification);
        return;
    }
    for (auto* m : models)
        for (auto* b : m->buttons)
            b->setEnabled (false);
    state.setText (juce::String::fromUTF8 ("Running\xe2\x80\xa6"), juce::dontSendNotification);
    stopButton.setVisible (true);
    resized();
}

void ModelsPage::modelDone (const juce::var& result)
{
    const auto key = modelKind.contains ("flamingo") ? "flamingo" : modelKind.contains ("demucs") ? "demucs" : "ace";
    const auto status = result["status"].toString();
    juce::String text;
    if (status == "downloaded-associated-inference-unchecked")
        text = "Downloaded and associated. Inference has not been checked. " + result["snapshot"].toString();
    else if (status == "installed-associated-cpu-loaded")
        text = "Downloaded, associated and loaded on CPU. GPU separation has not been checked.";
    else if (status == "cached-cpu-loaded")
        text = "Cached weights verified and loaded on CPU. GPU separation has not been checked.";
    else if (status == "installed-load-pending")
        text = "ACE environment installed and associated. Start ACE to download/load its weights.";
    else if (result["python"].isString())
        text = "Demucs environment found: " + result["python"].toString();
    else
        text = juce::JSON::toString (result, true);
    for (auto* m : models)
        if (m->key == key)
            m->state.setText (text, juce::dontSendNotification);
    state.setText (text, juce::dontSendNotification);
}

void ModelsPage::sources()
{
    if (worker.running() || service.running())
        return;
    auto* w = new juce::AlertWindow ("Model download sources", "Changing a source affects future downloads and loads. Existing files are preserved.",
                                     juce::MessageBoxIconType::NoIcon);
    const std::pair<const char*, const char*> keys[] = { { "modelHubUrl", "Hugging Face server" }, { "flamingoRepository", "Flamingo repository (author/model)" },
                                                         { "flamingoRevision", "Flamingo version / commit" }, { "demucsNamespace", "Demucs publisher on Hugging Face" },
                                                         { "aceRepository", "ACE source repository (HTTPS)" }, { "aceRevision", "ACE version / commit" },
                                                         { "aceWeightsRepository", "ACE weights repository (author/model)" },
                                                         { "aceWeightsRevision", "ACE weights version / commit" }, { "wslDistribution", "Ubuntu in WSL" } };
    for (auto& [k, label] : keys)
        w->addTextEditor (k, AppSettings::get (k), label);
    w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([w] (int r) {
                            if (r != 1)
                                return;
                            for (auto k : { "modelHubUrl", "flamingoRepository", "flamingoRevision", "demucsNamespace", "aceRepository", "aceRevision",
                                            "aceWeightsRepository", "aceWeightsRevision", "wslDistribution" })
                                AppSettings::set (k, w->getTextEditorContents (k).trim());
                        }),
                        true);
}

void ModelsPage::startWorker()
{
    if (trainer != nullptr && trainer->isRunning())
        return;
    const auto models = juce::File (AppSettings::get ("modelsRoot"));
    const auto folder = models.getChildFile ("worker");
    folder.createDirectory();
    const auto src = AppSettings::home().getChildFile ("app").getChildFile ("workers").getChildFile ("trainer").getChildFile ("src");
    if (! src.getChildFile ("aicore_worker").getChildFile ("cli.py").existsAsFile())
    {
        workerNote.setText ("The training worker is not in this install (app\\workers\\trainer).", juce::dontSendNotification);
        return;
    }
    trainerLog = folder.getChildFile ("worker-stdout.log");
    juce::String sets;
    auto env = Worker::environment();
    env.set ("PYTHONPATH", src.getFullPathName());
    for (auto& k : env.getAllKeys())
        sets << "set \"" << k << "=" << env[k] << "\"&& ";
    juce::String args = " -m aicore_worker.cli serve --data-dir " + folder.getFullPathName().quoted() + " --import-root " + models.getFullPathName().quoted();
    if (AppSettings::get ("modelSource") == "mirror" && AppSettings::get ("modelSourceUrl").isNotEmpty())
        args += " --download-base " + AppSettings::get ("modelSourceUrl").quoted();
    trainer = std::make_unique<juce::ChildProcess>();
    const auto cmd = "cmd.exe /s /c \"" + sets + Worker::python().getFullPathName().quoted() + args + " > " + trainerLog.getFullPathName().quoted() + " 2>&1\"";
    trainerLog.deleteFile();
    if (! trainer->start (cmd, 0))
    {
        workerNote.setText ("Failed.", juce::dontSendNotification);
        trainer.reset();
        return;
    }
    workerNote.setText (juce::String::fromUTF8 ("Starting\xe2\x80\xa6"), juce::dontSendNotification);
    workerStart.setEnabled (false);
    workerStop.setEnabled (true);
    startTimer (500);
}

void ModelsPage::stopWorker()
{
    if (trainer != nullptr)
    {
        trainer->kill();
        trainer.reset();
    }
    stopTimer();
    workerNote.setText ("Stopped.", juce::dontSendNotification);
    workerStart.setEnabled (true);
    workerStop.setEnabled (false);
}

void ModelsPage::timerCallback()
{
    if (trainer == nullptr || ! trainer->isRunning())
    {
        stopTimer();
        trainer.reset();
        workerNote.setText ("Failed. " + trainerLog.loadFileAsString().trim().getLastCharacters (200), juce::dontSendNotification);
        workerStart.setEnabled (true);
        workerStop.setEnabled (false);
        return;
    }
    const auto text = trainerLog.loadFileAsString();
    if (text.contains ("hello") || text.contains ("\"backend\""))
        workerNote.setText ("Ready. " + juce::File (AppSettings::get ("modelsRoot")).getChildFile ("worker").getFullPathName(), juce::dontSendNotification);
}

void ModelsPage::visibilityChanged() {}

void ModelsPage::paint (juce::Graphics& g)
{
    auto box = getLocalBounds().withTrimmedTop (2).withTrimmedBottom (4);
    drawPanel (g, box);
    drawHeading (g, "Hugging Face", 12, box.getY() + 8, getWidth() - 24);
    drawHeading (g, "Where models are saved", 12, headingY2, getWidth() - 24, true);
    g.setColour (col::grid);
    g.fillRect (9, localTitle.getY() - 9, getWidth() - 18, 1);
    for (auto* m : models)
    {
        auto b = m->box.translated (modelsView.getX(), modelsView.getY() - modelsView.getViewPositionY());
        if (! modelsView.getBounds().intersects (b))
            continue;
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (modelsView.getBounds());
        g.setColour (col::field);
        g.fillRect (b);
        g.setColour (col::outline);
        g.drawRect (b, 1);
        g.setColour (col::grid);
        g.fillRect (b.withHeight (24));
        g.setColour (col::text);
        g.setFont (uiFont (15.0f));
        g.drawText (m->title, b.getX() + 8, b.getY(), b.getWidth(), 24, juce::Justification::centredLeft);
    }
}

void ModelsPage::resized()
{
    auto r = getLocalBounds().withTrimmedTop (2).reduced (9, 0).withTrimmedBottom (8).withTrimmedTop (46);
    auto line = r.removeFromTop (28);
    accountLabel.setBounds (line.removeFromLeft (63).withTrimmedLeft (-4));
    check.setBounds (line.removeFromRight (130));
    account.setBounds (line);
    r.removeFromTop (10);
    line = r.removeFromTop (28);
    tokenLabel.setBounds (line.removeFromLeft (63).withTrimmedLeft (-4));
    unlinkButton.setBounds (line.removeFromRight (unlinkButton.idealWidth (26)));
    line.removeFromRight (6);
    linkButton.setBounds (line.removeFromRight (linkButton.idealWidth (26)));
    line.removeFromRight (8);
    token.setBounds (line);
    r.removeFromTop (22);
    headingY2 = r.getY();
    r.removeFromTop (44);
    line = r.removeFromTop (28);
    folderLabel.setBounds (line.removeFromLeft (112).withTrimmedLeft (-4));
    change.setBounds (line.removeFromRight (change.idealWidth (26)));
    line.removeFromRight (8);
    folder.setBounds (line);
    r.removeFromTop (8);
    line = r.removeFromTop (28);
    fromLabel.setBounds (line.removeFromLeft (112).withTrimmedLeft (-4));
    from.setBounds (line);
    r.removeFromTop (8);
    line = r.removeFromTop (28);
    mirrorLabel.setBounds (line.removeFromLeft (112).withTrimmedLeft (-4));
    mirror.setBounds (line);
    r.removeFromTop (8);
    line = r.removeFromTop (30);
    save.setBounds (line.removeFromRight (save.idealWidth (26)));
    setupState.setBounds (line.withTrimmedLeft (-4));
    r.removeFromTop (18);
    line = r.removeFromTop (30);
    localTitle.setBounds (line.removeFromLeft (200).withTrimmedLeft (-4));
    dots.setBounds (line.removeFromRight (36));
    line.removeFromRight (6);
    sourcesButton.setBounds (line.removeFromRight (sourcesButton.idealWidth (26)));
    r.removeFromTop (6);
    auto foot = r.removeFromBottom (24);
    if (stopButton.isVisible())
        stopButton.setBounds (foot.removeFromRight (stopButton.idealWidth (26)));
    state.setBounds (foot.withTrimmedLeft (-4));
    r.removeFromBottom (6);
    line = r.removeFromBottom (30);
    workerLabel.setBounds (line.removeFromLeft (110).withTrimmedLeft (-4));
    workerStop.setBounds (line.removeFromRight (workerStop.idealWidth (26)));
    line.removeFromRight (6);
    workerStart.setBounds (line.removeFromRight (workerStart.idealWidth (26)));
    workerNote.setBounds (line);
    r.removeFromBottom (6);
    modelsView.setBounds (r);
    // the models, one box each
    const int w = r.getWidth() - modelsView.getScrollBarThickness();
    int y = 0;
    for (auto* m : models)
    {
        m->box = { 0, y, w, 120 };
        int x = 20;
        const int by = y + 24 + 22;
        if (m->key == "demucs")
        {
            demucsModel.setBounds (x, by, 146, 28);
            x += 152;
        }
        for (auto* b : m->buttons)
        {
            b->setBounds (x, by, b->idealWidth (26), 30);
            x += b->getWidth() + 6;
        }
        m->state.setBounds (16, by + 34, w - 32, 24);
        y += 126;
    }
    modelsBody.setBounds (0, 0, w, y);
    repaint();
}

//============================================================================== KEYBINDS
KeybindsPage::KeybindsPage()
{
    setWantsKeyboardFocus (true);
    search.setTextToShowWhenEmpty (juce::String::fromUTF8 ("Search an action or a key\xe2\x80\xa6"), col::dim);
    styleField (search);
    search.onTextChange = [this] { filter(); };
    addAndMakeVisible (search);
    reset.onClick = [this] {
        for (auto* r : rows)
            for (auto& k : kKeybinds)
                if (r->name == k.name)
                    r->key.setText (k.key, false);
        save();
    };
    addAndMakeVisible (reset);
    styleLabel (state, 14.0f, false, col::muted);
    addAndMakeVisible (state);
    for (auto& k : kKeybinds)
    {
        auto* r = rows.add (new Row());
        r->name = k.name;
        r->what.setText (k.what, juce::dontSendNotification);
        styleLabel (r->what, 15.0f, true);
        r->where.setText (k.where, juce::dontSendNotification);
        styleLabel (r->where, 15.0f, false, col::muted);
        styleField (r->key);
        r->key.setText (Keys::keyFor (k.name), false);
        r->key.setReadOnly (true);
        r->key.setMouseClickGrabsKeyboardFocus (false);
        r->change.onClick = [this, r] {
            changing = r->name;
            r->key.setText ({}, false);
            state.setText ("Press the new key.", juce::dontSendNotification);
            grabKeyboardFocus();
        };
        for (juce::Component* c : { (juce::Component*) &r->what, (juce::Component*) &r->where, (juce::Component*) &r->key, (juce::Component*) &r->change })
            addAndMakeVisible (c);
    }
}

bool KeybindsPage::keyPressed (const juce::KeyPress& k)
{
    if (changing.isEmpty())
        return false;
    const auto text = Keys::toText (k);
    Row* row = nullptr;
    for (auto* r : rows)
        if (r->name == changing)
            row = r;
    changing = {};
    if (row == nullptr)
        return true;
    if (text.isEmpty() || k.getKeyCode() == juce::KeyPress::escapeKey)
    {
        row->key.setText (Keys::keyFor (row->name), false);
        state.setText ({}, juce::dontSendNotification);
        return true;
    }
    for (auto* r : rows)
        if (r != row && r->key.getText() == text)
        {
            row->key.setText (Keys::keyFor (row->name), false);
            state.setText ("That key is already used by: " + r->what.getText(), juce::dontSendNotification);
            return true;
        }
    row->key.setText (text, false);
    save();
    return true;
}

void KeybindsPage::save()
{
    auto* o = new juce::DynamicObject();
    for (auto* r : rows)
        for (auto& k : kKeybinds)
            if (r->name == k.name && r->key.getText() != juce::String (k.key))
                o->setProperty (juce::Identifier (r->name), r->key.getText());
    AppSettings::setVar ("keybinds", juce::var (o));
    state.setText ("Saved.", juce::dontSendNotification);
}

void KeybindsPage::filter()
{
    const auto text = search.getText().trim().toLowerCase();
    for (auto* r : rows)
    {
        r->shown = text.isEmpty() || r->what.getText().toLowerCase().contains (text) || r->key.getText().toLowerCase().contains (text)
                   || r->where.getText().toLowerCase().contains (text);
        for (juce::Component* c : { (juce::Component*) &r->what, (juce::Component*) &r->where, (juce::Component*) &r->key, (juce::Component*) &r->change })
            c->setVisible (r->shown);
    }
    resized();
}

void KeybindsPage::paint (juce::Graphics& g)
{
    auto box = getLocalBounds().withTrimmedTop (2).withTrimmedBottom (4);
    drawPanel (g, box);
    g.setColour (col::title);
    g.setFont (uiFont (15.0f, true).withExtraKerningFactor (0.1f));
    g.drawText (aicore::ui::tr ("KEYBINDS"), 20, box.getY() + 14, 300, 26, juce::Justification::centredLeft);
}

void KeybindsPage::resized()
{
    auto r = getLocalBounds().withTrimmedTop (2).reduced (16, 0).withTrimmedTop (14).withTrimmedBottom (14);
    auto head = r.removeFromTop (28);
    reset.setBounds (head.removeFromRight (reset.idealWidth (26)));
    head.removeFromRight (8);
    search.setBounds (head.removeFromRight (head.getWidth() * 2 / 3 - 8));
    state.setBounds (r.removeFromBottom (20));
    r.removeFromTop (18);
    for (auto* row : rows)
    {
        if (! row->shown)
            continue;
        auto line = r.removeFromTop (28).withTrimmedRight (8);
        row->what.setBounds (line.removeFromLeft (150).withTrimmedLeft (-4));
        row->where.setBounds (line.removeFromLeft (58));
        row->change.setBounds (line.removeFromRight (row->change.idealWidth (26)));
        line.removeFromRight (14);
        row->key.setBounds (line);
        r.removeFromTop (8);
    }
}

//============================================================================== PLUGINS
PluginsPage::PluginsPage()
{
    setWantsKeyboardFocus (true);
    for (auto* l : { &found, &missing, &state, &note })
    {
        styleLabel (*l);
        addAndMakeVisible (l);
    }
    styleLabel (note, 14.0f, false, col::muted);
    note.setText ("AICore's own plugin list (open source: Vital, sfizz, Airwindows, Dragonfly). Only these are looked for; "
                  "the other plugins of the PC are for manual use. Each role plays with the plugin chosen here.",
                  juce::dontSendNotification);
    note.setJustificationType (juce::Justification::topLeft);
    note.setMinimumHorizontalScale (1.0f);
    scanButton.onClick = [this] { scan(); };
    addAndMakeVisible (scanButton);
    for (auto role : { "bass", "lead", "pad", "pluck", "riser", "drums", "filter", "drive", "delay", "sidechain", "reverb" })
    {
        auto* r = roles.add (new Role());
        r->role = role;
        r->label.setText (juce::String (role).substring (0, 1).toUpperCase() + juce::String (role).substring (1), juce::dontSendNotification);
        styleLabel (r->label);
        styleCombo (r->plugin);
        r->plugin.onChange = [this, r] {
            const int i = r->plugin.getSelectedItemIndex();
            auto v = i <= 0 ? run ({ "roles", "clear", r->role }) : run ({ "roles", "set", r->role, r->files[i - 1] });
            state.setText (v.isObject() && v["error"].isObject() ? v["error"]["message"].toString() : juce::String ("Saved: ") + r->role + " = " + r->plugin.getText(),
                           juce::dontSendNotification);
        };
        addAndMakeVisible (r->label);
        addAndMakeVisible (r->plugin);
    }
    refresh();
}

juce::var PluginsPage::run (const juce::StringArray& args)
{
    juce::StringArray cmd { renders::TemplateRendersPage::renderer().getFullPathName() };
    cmd.addArray (args);
    juce::ChildProcess child;
    juce::var last;
    if (child.start (cmd, juce::ChildProcess::wantStdOut))
    {
        const auto text = child.readAllProcessOutput();
        child.waitForProcessToFinish (600000);
        for (auto& line : juce::StringArray::fromLines (text))
        {
            juce::var v;
            if (line.trim().startsWithChar ('{') && juce::JSON::parse (line.trim(), v).wasOk())
                last = v;
        }
    }
    return last;
}

void PluginsPage::scan()
{
    state.setText (juce::String::fromUTF8 ("Looking for the plugins\xe2\x80\xa6"), juce::dontSendNotification);
    auto v = run ({ "scan" });
    if (! v.isObject())
        state.setText ("aicore-render did not answer.", juce::dontSendNotification);
    else
        state.setText ("Found " + v["plugins"].toString() + " plugins (" + v["instruments"].toString() + " instruments, " + v["effects"].toString() + " effects).",
                       juce::dontSendNotification);
    refresh();
}

void PluginsPage::refresh()
{
    const auto list = AppSettings::file().getParentDirectory().getParentDirectory().getChildFile ("plugins.json");
    auto v = list.existsAsFile() ? juce::JSON::parse (list) : juce::var();
    plugins.clear();
    if (auto* a = v["plugins"].getArray())
        plugins = *a;
    juce::StringArray miss;
    if (auto* a = v["missing"].getArray())
        for (auto& m : *a)
            miss.add (m.isObject() ? m["name"].toString() : m.toString());
    found.setText (list.existsAsFile() ? "Found: " + juce::String (plugins.size()) + " plugins" + (v["scannedAt"].isString() ? juce::String::fromUTF8 (" \xc2\xb7 ") + v["scannedAt"].toString() : juce::String())
                                       : juce::String ("Not looked for yet: press Find plugins."),
                   juce::dontSendNotification);
    missing.setText (miss.isEmpty() ? juce::String() : "Missing: " + miss.joinIntoString (", "), juce::dontSendNotification);
    auto current = run ({ "roles" });
    for (auto* r : roles)
    {
        r->plugin.clear (juce::dontSendNotification);
        r->files.clear();
        r->plugin.addItem ("(none)", 1);
        for (auto& p : plugins)
            if (auto* rl = p["roles"].getArray(); rl != nullptr && rl->contains (r->role))
            {
                r->files.add (p["file"].toString());
                r->plugin.addItem (p["name"].toString() + " " + p["version"].toString(), r->plugin.getNumItems() + 1);
            }
        auto chosen = current["roles"][juce::Identifier (r->role)];
        const auto file = chosen.isObject() ? chosen["file"].toString() : chosen.toString();
        const int at = r->files.indexOf (file);
        r->plugin.setSelectedItemIndex (at >= 0 ? at + 1 : 0, juce::dontSendNotification);
    }
    resized();
    repaint();
}

void PluginsPage::paint (juce::Graphics& g)
{
    drawPanel (g, listBox);
    drawPanel (g, rolesBox);
    drawHeading (g, "Plugins", listBox.getX() + 12, listBox.getY() + 8, listBox.getWidth() - 24);
    drawHeading (g, "Roles", rolesBox.getX() + 12, rolesBox.getY() + 8, rolesBox.getWidth() - 24);
    int y = found.getBottom() + 30;
    for (auto& p : plugins)
    {
        juce::Rectangle<int> card (listBox.getX() + 16, y, listBox.getWidth() - 32, 62);
        g.setColour (col::field);
        g.fillRoundedRectangle (card.toFloat(), 2.0f);
        g.setColour (col::outline);
        g.drawRoundedRectangle (card.toFloat().reduced (0.5f), 2.0f, 1.0f);
        g.setColour (col::text);
        g.setFont (uiFont (15.0f, true));
        g.drawText (p["name"].toString() + "  " + p["version"].toString(), card.getX() + 10, card.getY() + 6, card.getWidth() - 20, 20, juce::Justification::centredLeft);
        g.setFont (uiFont (13.0f));
        g.setColour (col::muted);
        juce::StringArray rl;
        if (auto* a = p["roles"].getArray())
            for (auto& x : *a)
                rl.add (x.toString());
        g.drawText (juce::String ((bool) p["isInstrument"] ? "Instrument" : "Effect") + juce::String::fromUTF8 (" \xc2\xb7 ") + rl.joinIntoString (", "), card.getX() + 10, card.getY() + 24, card.getWidth() - 20, 18,
                    juce::Justification::centredLeft);
        g.drawText (p["file"].toString(), card.getX() + 10, card.getY() + 40, card.getWidth() - 20, 18, juce::Justification::centredLeft, true);
        y += 70;
    }
}

void PluginsPage::resized()
{
    auto r = getLocalBounds().withTrimmedTop (2).withTrimmedBottom (4);
    rolesBox = r.removeFromRight (400);
    r.removeFromRight (8);
    listBox = r;
    auto l = listBox.reduced (16, 0).withTrimmedTop (50);
    auto head = l.removeFromTop (30);
    scanButton.setBounds (head.removeFromRight (scanButton.idealWidth (26)));
    found.setBounds (head.withTrimmedLeft (-4));
    missing.setBounds (l.removeFromTop (24).withTrimmedLeft (-4));
    note.setBounds (listBox.reduced (16, 14).removeFromBottom (44).withTrimmedLeft (-4));
    state.setBounds (listBox.reduced (16, 14).removeFromBottom (68).removeFromTop (22).withTrimmedLeft (-4));
    auto rr = rolesBox.reduced (16, 0).withTrimmedTop (50);
    for (auto* role : roles)
    {
        auto line = rr.removeFromTop (28);
        role->label.setBounds (line.removeFromLeft (100).withTrimmedLeft (-4));
        role->plugin.setBounds (line);
        rr.removeFromTop (8);
    }
}
} // namespace aicore::settings
