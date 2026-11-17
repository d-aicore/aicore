// AICore-C — the old AICORE window (gui.py StudioWindow): the top bar (logo AICORE, EDIT / AI in the centre,
// the transport and the clock, SETTINGS at the right), the section row of the chosen area, and its page.
#pragma once

#include "AicoreTheme.h"
#include "../tracks/TracksPage.h"
#include "../sample/SamplePage.h"
#include "../stems/StemsPage.h"
#include "../midi/MidiPage.h"
#include "../ai/AiPage.h"
#include "../lora/LoraPage.h"
#include "../renders/TemplateRendersPage.h"
#include "../database/DatabasePage.h"
#include "../settings/SettingsPages.h"
#include "Page.h"

#include <map>

namespace aicore::ui
{
class TransportButton : public juce::Button
{
public:
    enum Kind { start, play, pause, stop };
    explicit TransportButton (Kind k) : juce::Button ({}), kind (k) { setWantsKeyboardFocus (false); }
    void setKind (Kind k) { kind = k; repaint(); }
    void paintButton (juce::Graphics&, bool over, bool down) override;

private:
    Kind kind;
};

class Workspace : public juce::Component, private juce::Timer
{
public:
    Workspace();
    ~Workspace() override;

    void showArea (int area);              // 0 EDIT, 1 AI, 2 SETTINGS
    void showSection (int area, int section);
    juce::String currentName() const;

    tracks::TracksPage tracksPage;
    sample::SamplePage samplePage;
    stems::StemsPage stemsPage { false }, voicePage { true };
    midi::MidiPage midiPage;
    ai::AiPage aiPage;
    lora::LoraPage loraPage;
    renders::TemplateRendersPage rendersPage;
    database::DatabasePage databasePage;
    settings::FoldersPage foldersPage;
    settings::ModelsPage modelsPage;
    settings::KeybindsPage keybindsPage;
    settings::PluginsPage pluginsPage;
    /** The page shown, or null (a section not ported yet). */
    Page* currentPage() const;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void syncTransport();

    struct Area
    {
        juce::String name;
        juce::StringArray sections;
        int current = 0;
    };
    std::vector<Area> areas;
    int area = 0;

    AButton editButton { "EDIT" }, aiButton { "AI" }, settingsButton { "SETTINGS" };
    TransportButton startButton { TransportButton::start }, playButton { TransportButton::play }, stopButton { TransportButton::stop };
    juce::Label clockLabel;
    juce::OwnedArray<AButton> sectionButtons;
    juce::String placeholder;
    std::map<std::pair<int, int>, Page*> pages; // (area, section) -> page
};
} // namespace aicore::ui
