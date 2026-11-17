// AICore-C — AI > TEMPLATE RENDERS (the old AICORE's template_renders_gui.py): the same screen, but the sound is
// rendered by AICore's own aicore-render (VST3), never by Core's engine (Diogo 2026-11-16). The work is
// services/render/aicore_template_renders.py in AICore's Python, run by the Worker; it writes audio.npy, notes.npy
// and catalogo.jsonl, as before.
#pragma once

#include "../sample/SamplePage.h"
#include "../tracks/ChainPanel.h"
#include "../ui/Page.h"
#include "../ui/Worker.h"

namespace aicore::renders
{
class TemplateRendersPage : public ui::Page
{
public:
    TemplateRendersPage();
    void start();
    void paint (juce::Graphics&) override;
    void resized() override;
    static juce::File renderer(); // aicore-render.exe next to the app

private:
    struct Row
    {
        juce::Label label;
        juce::TextEditor field;
        std::unique_ptr<juce::Button> button;
    };
    Row templates, mids, programs, out;
    ui::AButton go { "Render" }, stopButton { "Stop" };
    juce::Label title, state;
    tracks::Knob perTemplate { "Per template", "", 1, 1000, 0, 40 }, bars { "Bars", "", 1, 16, 0, 4 };
    ui::Worker worker;
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace aicore::renders
