// AICore-C — SETTINGS (the old AICORE's AudioSettingsPage, settings_gui.ModelSetup + ModelsPage, KeybindsPage),
// and SETTINGS > PLUGINS (asked by Diogo 2026-11-16: "nas definições metemos procurar vst, plugins, e associar
// cada um"): the plugins of AICore's list found by aicore-render scan, and which plugin plays each role.
#pragma once

#include "../sample/SamplePage.h"
#include "../ui/Keybinds.h"
#include "../ui/Page.h"
#include "../ui/Worker.h"

namespace aicore::settings
{
/** A label, a field and its folder / file button (path_row). */
struct PathRow
{
    juce::Label label;
    juce::TextEditor field;
    sample::IconButton pick { sample::IconButton::folder };
    bool folder = true;
};

class FoldersPage : public ui::Page
{
public:
    FoldersPage();
    void load();
    void save();
    void paint (juce::Graphics&) override;
    void resized() override;
    std::function<void()> onSaved;

private:
    juce::Label languageLabel, languageNote, urlLabel, runtimeLabel, state;
    juce::ComboBox language, runtime;
    PathRow training, workspace, samples, python;
    juce::TextEditor url;
    ui::AButton saveButton { "Save settings" };
    std::unique_ptr<juce::FileChooser> chooser;
};

class ModelsPage : public ui::Page, private juce::Timer
{
public:
    ModelsPage();
    ~ModelsPage() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

private:
    void timerCallback() override;
    juce::File tokenFile() const;
    void showToken();
    void link();
    void unlink();
    void checkAccount();
    void saveFolder();
    void startTask (const juce::String& kind, juce::var fields = {});
    void modelDone (const juce::var&);
    void sources();
    void startWorker();
    void stopWorker();

    juce::Label accountLabel, account, tokenLabel, folderLabel, fromLabel, mirrorLabel, setupState, localTitle, workerLabel, workerNote, state;
    juce::TextEditor token, folder, mirror;
    juce::ComboBox from;
    ui::AButton check { "Check" }, linkButton { "Link" }, unlinkButton { "Unlink" }, change { "Change" }, save { "Save" },
        sourcesButton { juce::String::fromUTF8 ("Download sources\xe2\x80\xa6") }, workerStart { "Start worker" }, workerStop { "Stop worker" }, stopButton { "Stop" };
    sample::IconButton dots { sample::IconButton::dots };
    struct Model
    {
        juce::String key, title, install, probe;
        juce::Label state;
        juce::OwnedArray<ui::AButton> buttons;
        juce::Rectangle<int> box;
    };
    juce::OwnedArray<Model> models;
    juce::ComboBox demucsModel;
    juce::Viewport modelsView;
    juce::Component modelsBody;
    ui::Worker worker, service;
    juce::String modelKind;
    std::unique_ptr<juce::ChildProcess> trainer;
    juce::File trainerLog;
    int headingY2 = 0;
    std::unique_ptr<juce::FileChooser> chooser;
};

class KeybindsPage : public ui::Page
{
public:
    KeybindsPage();
    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void save();
    void filter();
    struct Row
    {
        juce::String name;
        juce::Label what, where;
        juce::TextEditor key;
        ui::AButton change { "Change" };
        bool shown = true;
    };
    juce::OwnedArray<Row> rows;
    juce::TextEditor search;
    ui::AButton reset { "Reset all" };
    juce::Label state;
    juce::String changing;
};

class PluginsPage : public ui::Page
{
public:
    PluginsPage();
    void refresh();
    void scan();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::var run (const juce::StringArray& args);
    struct Role
    {
        juce::String role;
        juce::Label label;
        juce::ComboBox plugin;
        juce::StringArray files;
    };
    juce::OwnedArray<Role> roles;
    juce::Label found, missing, state, note;
    ui::AButton scanButton { "Find plugins" };
    juce::Array<juce::var> plugins;
    juce::Rectangle<int> listBox, rolesBox;
};
} // namespace aicore::settings
