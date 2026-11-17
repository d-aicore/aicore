// AICore-C — AI > DATABASE (the old AICORE's examples_gui.py): the template examples database. The examples
// library, Create / update database, add a template and its music (verdict, sections, tracks, why), and the table.
// The work is AICore's exemplos.py, run by services/database/aicore_db_tool.py; the catalogue is AICore's own
// templates folder, never the Core's.
#pragma once

#include "../sample/SamplePage.h"
#include "../ui/Page.h"

namespace aicore::database
{
class DatabasePage : public ui::Page, private juce::TableListBoxModel
{
public:
    DatabasePage();
    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    int getNumRows() override { return rows.size(); }
    void paintRowBackground (juce::Graphics&, int, int w, int h, bool) override;
    void paintCell (juce::Graphics&, int row, int column, int w, int h, bool) override;
    juce::var tool (const juce::StringArray& args);
    void updateDatabase();
    void add();

    struct Row
    {
        juce::Label label;
        juce::Component* field = nullptr;
        std::unique_ptr<juce::Button> button;
    };
    juce::Label title, addTitle, state;
    juce::TextEditor library, templateField, audio, sections, tracks, why;
    juce::ComboBox verdict;
    Row libraryRow, templateRow, audioRow, verdictRow, sectionsRow, tracksRow, whyRow;
    ui::AButton updateButton { "Create / update database" }, addButton { "Add to the examples" };
    juce::TableListBox table { {}, this };
    juce::Array<juce::var> rows;
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace aicore::database
