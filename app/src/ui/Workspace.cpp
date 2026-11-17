#include "Workspace.h"
#include "I18n.h"

namespace aicore::ui
{
namespace
{
constexpr int headerH = 52, sectionH = 48;
}

void TransportButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    if (! isEnabled())
        g.setColour (col::disabledFill);
    else if (down)
        g.setColour (col::pressed);
    if (! isEnabled() || down)
        g.fillRoundedRectangle (r, 2.0f);
    else
    {
        g.setGradientFill (over ? juce::ColourGradient (col::onTop, 0, r.getY(), col::onBottom, 0, r.getBottom(), false)
                                : juce::ColourGradient (col::buttonTop, 0, r.getY(), col::buttonBottom, 0, r.getBottom(), false));
        g.fillRoundedRectangle (r, 2.0f);
    }
    g.setColour (! isEnabled() ? col::disabledEdge : over ? col::onEdge : col::edge);
    g.drawRoundedRectangle (r, 2.0f, 1.0f);
    const juce::Colour ink = ! isEnabled() ? col::disabledText : over || down ? col::onText : col::selectedText;
    g.setColour (ink);
    const auto c = r.getCentre();
    juce::Path p;
    switch (kind)
    {
        case start:
            g.fillRect (c.x - 6.0f, c.y - 6.0f, 2.0f, 12.0f);
            p.addTriangle (c.x + 6.0f, c.y - 6.0f, c.x + 6.0f, c.y + 6.0f, c.x - 3.0f, c.y);
            break;
        case play:
            p.addTriangle (c.x - 4.5f, c.y - 7.0f, c.x - 4.5f, c.y + 7.0f, c.x + 7.0f, c.y);
            break;
        case pause:
            g.fillRect (c.x - 5.0f, c.y - 6.0f, 3.5f, 12.0f);
            g.fillRect (c.x + 1.5f, c.y - 6.0f, 3.5f, 12.0f);
            break;
        case stop:
            g.fillRect (c.x - 5.5f, c.y - 5.5f, 11.0f, 11.0f);
            break;
    }
    g.fillPath (p);
}

Workspace::Workspace()
{
    areas = { { "EDIT", { "TRACKS", "SAMPLE", "STEMS", "VOICE", "MIDI" } },
              { "AI", { juce::String::fromUTF8 ("ACE \xc2\xb7 FLAMINGO"), juce::String::fromUTF8 ("AUDIO LORA \xc2\xb7 VOICE / SAMPLES"),
                        "TEMPLATE RENDERS", "DATABASE" } },
              { "SETTINGS", { "FOLDERS", "MODELS", "KEYBINDS", "PLUGINS" } } };
    for (auto* b : { &editButton, &aiButton, &settingsButton })
        addAndMakeVisible (b);
    editButton.onClick = [this] { showArea (0); };
    aiButton.onClick = [this] { showArea (1); };
    settingsButton.onClick = [this] { showArea (2); };
    for (auto* b : { &startButton, &playButton, &stopButton })
        addAndMakeVisible (b);
    startButton.setTooltip ("To the start (Home)");
    playButton.setTooltip ("Play / pause (Space)");
    stopButton.setTooltip ("Stop: back to where it started");
    startButton.onClick = [this] { if (auto* p = currentPage()) p->goStart(); };
    playButton.onClick = [this] { if (auto* p = currentPage()) p->togglePlay(); };
    stopButton.onClick = [this] { if (auto* p = currentPage()) p->stop(); };
    clockLabel.setFont (uiFont (19.0f));
    clockLabel.setJustificationType (juce::Justification::centred);
    clockLabel.setColour (juce::Label::textColourId, col::text);
    clockLabel.setColour (juce::Label::backgroundColourId, col::field);
    clockLabel.setColour (juce::Label::outlineColourId, col::outline);
    addAndMakeVisible (clockLabel);

    pages[{ 0, 0 }] = &tracksPage;
    pages[{ 0, 1 }] = &samplePage;
    pages[{ 0, 2 }] = &stemsPage;
    pages[{ 0, 3 }] = &voicePage;
    pages[{ 0, 4 }] = &midiPage;
    pages[{ 1, 0 }] = &aiPage;
    pages[{ 1, 1 }] = &loraPage;
    pages[{ 1, 2 }] = &rendersPage;
    pages[{ 1, 3 }] = &databasePage;
    pages[{ 2, 0 }] = &foldersPage;
    pages[{ 2, 1 }] = &modelsPage;
    pages[{ 2, 2 }] = &keybindsPage;
    pages[{ 2, 3 }] = &pluginsPage;
    for (auto* sp : { &stemsPage, &voicePage })
        sp->onTrainingAudio = [this] (const juce::String& path) {
            showArea (1);
            showSection (1, 1);
            loraPage.addAudio ({ path });
        };
    aiPage.onOpenInTracks = [this] (const juce::StringArray& paths) {
        showArea (0);
        showSection (0, 0);
        tracksPage.addFiles (paths);
    };
    for (auto* sp : { &stemsPage, &voicePage })
    {
        sp->onEditAsSample = [this] (const juce::String& path) {
            showArea (0);
            showSection (0, 1);
            samplePage.openFile (path);
        };
        sp->onOpenInTracks = [this] (const juce::StringArray& paths) {
            showArea (0);
            showSection (0, 0);
            tracksPage.addFiles (paths);
        };
    }
    for (auto& [key, page] : pages)
    {
        addChildComponent (page);
        if (auto* t = page->sectionTools())
            addChildComponent (t);
        if (auto* t = page->sectionRight())
            addChildComponent (t);
        page->onTransportChanged = [this] { syncTransport(); };
    }
    showArea (0);
    startTimerHz (20);
}

Workspace::~Workspace() { stopTimer(); }

juce::String Workspace::currentName() const
{
    return areas[(size_t) area].name + " > " + areas[(size_t) area].sections[areas[(size_t) area].current];
}

void Workspace::showArea (int a)
{
    area = juce::jlimit (0, (int) areas.size() - 1, a);
    editButton.setToggleState (area == 0, juce::dontSendNotification);
    aiButton.setToggleState (area == 1, juce::dontSendNotification);
    settingsButton.setToggleState (area == 2, juce::dontSendNotification);
    sectionButtons.clear();
    auto& ar = areas[(size_t) area];
    for (int i = 0; i < ar.sections.size(); ++i)
    {
        auto* b = sectionButtons.add (new AButton (ar.sections[i], [this, i] { showSection (area, i); }));
        addAndMakeVisible (b);
    }
    showSection (area, ar.current);
}

void Workspace::showSection (int a, int s)
{
    auto& ar = areas[(size_t) a];
    ar.current = s;
    for (int i = 0; i < sectionButtons.size(); ++i)
        sectionButtons[i]->setToggleState (i == s, juce::dontSendNotification);
    auto* shown = currentPage();
    for (auto& [key, page] : pages)
    {
        const bool on = page == shown;
        if (! on && page->isPlaying())
            page->stop(); // one page plays at a time, as the old app
        page->setVisible (on);
        if (auto* t = page->sectionTools())
            t->setVisible (on);
        if (auto* t = page->sectionRight())
            t->setVisible (on);
    }
    placeholder = shown != nullptr ? juce::String() : ar.sections[s] + " is not ported to the native app yet.";
    syncTransport();
    resized();
    repaint();
    if (shown != nullptr)
    {
        I18n::translateTree (*shown); // the hints of the fields, the tooltips and the table headers
        if (auto* t = shown->sectionTools())
            I18n::translateTree (*t);
        shown->shown();
        shown->grabKeyboardFocus();
    }
}

Page* Workspace::currentPage() const
{
    auto it = pages.find ({ area, areas[(size_t) area].current });
    return it == pages.end() ? nullptr : it->second;
}

void Workspace::syncTransport()
{
    auto* p = currentPage();
    const bool on = p != nullptr && p->hasTransport();
    for (juce::Component* b : { (juce::Component*) &startButton, (juce::Component*) &playButton, (juce::Component*) &stopButton })
        b->setEnabled (on);
    playButton.setKind (on && p->isPlaying() ? TransportButton::pause : TransportButton::play);
    clockLabel.setText (on ? p->clock() : juce::String ("0:00.000 / 0:00.000"), juce::dontSendNotification);
}

void Workspace::timerCallback()
{
    if (auto* p = currentPage(); p != nullptr && p->isPlaying())
        syncTransport();
}

void Workspace::paint (juce::Graphics& g)
{
    g.fillAll (col::background);
    auto header = getLocalBounds().removeFromTop (headerH).toFloat();
    g.setGradientFill (juce::ColourGradient (col::headerTop, 0, header.getY(), col::headerBottom, 0, header.getBottom(), false));
    g.fillRect (header);
    g.setColour (col::headerLine);
    g.fillRect (header.getX(), header.getBottom() - 1, header.getWidth(), 1.0f);
    drawLogo (g, { 14.0f, 13.0f, 26.0f, 26.0f }, col::muted, col::muted);
    g.setColour (col::text);
    auto font = uiFont (16.0f, true).withExtraKerningFactor (0.19f);
    g.setFont (font);
    g.drawText ("AICORE", 46, 0, 120, headerH, juce::Justification::centredLeft);

    auto section = getLocalBounds().withTrimmedTop (headerH).removeFromTop (sectionH).toFloat();
    g.setGradientFill (juce::ColourGradient (col::sectionTop, 0, section.getY(), col::sectionBottom, 0, section.getBottom(), false));
    g.fillRect (section);
    g.setColour (col::outline);
    g.fillRect (section.getX(), section.getBottom() - 1, section.getWidth(), 1.0f);

    if (placeholder.isNotEmpty())
    {
        auto page = getLocalBounds().withTrimmedTop (headerH + sectionH).reduced (12).toFloat();
        g.setColour (col::card);
        g.fillRoundedRectangle (page, 2.0f);
        g.setColour (col::outline);
        g.drawRoundedRectangle (page.reduced (0.5f), 2.0f, 1.0f);
        g.setColour (col::muted);
        g.setFont (uiFont (15.0f));
        g.drawText (placeholder, page, juce::Justification::centred);
    }
}

void Workspace::resized()
{
    auto r = getLocalBounds();
    auto header = r.removeFromTop (headerH).reduced (14, 8);
    // three zones: logo at the left, EDIT / AI in the exact centre, transport + SETTINGS at the right
    const int cw = editButton.idealWidth (36) + 6 + aiButton.idealWidth (36);
    auto centre = header.withSizeKeepingCentre (cw, header.getHeight());
    editButton.setBounds (centre.removeFromLeft (editButton.idealWidth (36)));
    centre.removeFromLeft (6);
    aiButton.setBounds (centre);
    auto right = header.withTrimmedLeft (header.getWidth() / 2 + cw / 2 + 20);
    settingsButton.setBounds (right.removeFromRight (settingsButton.idealWidth (36)));
    right.removeFromRight (40);
    clockLabel.setBounds (right.removeFromRight (196));
    right.removeFromRight (10);
    stopButton.setBounds (right.removeFromRight (46));
    right.removeFromRight (6);
    playButton.setBounds (right.removeFromRight (46));
    right.removeFromRight (6);
    startButton.setBounds (right.removeFromRight (46));

    auto section = r.removeFromTop (sectionH).reduced (14, 9);
    for (auto* b : sectionButtons)
    {
        b->setBounds (section.removeFromLeft (b->idealWidth (30)));
        section.removeFromLeft (6);
    }
    if (auto* p = currentPage())
    {
        section.removeFromLeft (22);
        if (auto* right = dynamic_cast<AButton*> (p->sectionRight()))
            right->setBounds (section.removeFromRight (right->idealWidth (28)));
        if (auto* tools = p->sectionTools())
            tools->setBounds (section);
    }
    for (auto& [key, page] : pages)
        page->setBounds (r.reduced (10, 0));
}
} // namespace aicore::ui
