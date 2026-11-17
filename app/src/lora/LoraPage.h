// AICore-C — AI > AUDIO LORA · VOICE / SAMPLES (the old AICORE's AudioTrainPage + audio_workspace.Collections):
// COLLECTION at the left (choose, name, type, most samples, description, epochs, Create LoRA), SAMPLES in the
// middle (the table with caption, exact words, BPM, language, source group; Add samples…, ⋯). The collections
// are the same files as the old app's (<training folder>\<slug>-<id>\collection.json, audio-collection-1); the
// dataset and the training run in AICore's audio worker (prepare-dataset, train, load-adapter).
#pragma once

#include "../sample/SamplePage.h"
#include "../ui/Page.h"
#include "../ui/Panels.h"
#include "../ui/Worker.h"

namespace aicore::lora
{
/** audio_workspace.Collections, the same JSON. Every call returns an error text, empty when it worked. */
struct Collections
{
    explicit Collections (juce::File root);
    juce::Array<juce::var> list() const;
    juce::File directory (const juce::String& id) const;
    juce::var get (const juce::String& id) const;
    juce::String create (const juce::String& name, const juce::String& kind, int limit, const juce::String& caption, juce::String& newId);
    juce::String add (const juce::String& id, const juce::StringArray& paths);
    juce::String update (const juce::String& id, const juce::var& changes);
    juce::String rename (const juce::String& id, const juce::String& name, int limit, const juce::String& caption);
    juce::String remove (const juce::String& id, const juce::StringArray& sampleIds);
    static bool write (const juce::File&, const juce::var&);
    juce::File root;
};

class LoraPage : public ui::Page, private juce::TableListBoxModel
{
public:
    LoraPage();
    void refresh();
    void addAudio (const juce::StringArray& paths); // from STEMS' "Send the processed track to training"

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    int getNumRows() override;
    void paintRowBackground (juce::Graphics&, int, int, int, bool) override;
    void paintCell (juce::Graphics&, int, int, int, int, bool) override {}
    juce::Component* refreshComponentForCell (int row, int column, bool selected, juce::Component* existing) override;
    void selectedRowsChanged (int) override {}

    Collections store() const;
    void select();
    void createCollection();
    void renameCollection();
    void removeSample();
    void addFiles();
    bool saveLabels();
    void prepare (bool train);
    void loadAdapter();
    void options();
    void done (const juce::var&);
    void fail (const juce::String& m) { state.setText (m, juce::dontSendNotification); }
    void startTask (const juce::String& kind, juce::var fields);

    juce::ComboBox collections, kind, limit;
    juce::TextEditor name, caption;
    juce::Label nameLabel, kindLabel, limitLabel, captionLabel, state;
    ui::BarSpin epochs { 1, 500, 10, 0 };
    ui::AButton go { "Create LoRA" }, addButton { juce::String::fromUTF8 ("Add samples\xe2\x80\xa6") }, stopButton { "Stop" };
    sample::IconButton dots { sample::IconButton::dots };
    juce::TableListBox table { {}, this };
    juce::Rectangle<int> leftBox, centreBox;

    juce::String current;
    juce::Array<juce::var> items; // the chosen collection's samples, edited in place
    juce::StringArray ids;        // collection ids in the combo order
    juce::StringArray pendingAudio;
    juce::String dataset;
    bool pendingTrain = false;
    ui::Worker worker;
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace aicore::lora
