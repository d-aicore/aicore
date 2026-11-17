// AICore-C — one page of the window (a section of EDIT / AI / SETTINGS). The window's top bar drives the page
// that shows: its transport (start, play/pause, stop) and the clock; its tools sit in the section row.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace aicore::ui
{
class Page : public juce::Component
{
public:
    /** Tools at the left of the section row, after the section buttons (TRACKS: Import audio…, Split…). */
    virtual juce::Component* sectionTools() { return nullptr; }
    /** One tool at the right of the section row (TRACKS: Export the mix…). */
    virtual juce::Component* sectionRight() { return nullptr; }
    virtual bool hasTransport() const { return false; }
    virtual void togglePlay() {}
    virtual void stop() {}
    virtual void goStart() {}
    virtual bool isPlaying() const { return false; }
    virtual juce::String clock() const { return "0:00.000 / 0:00.000"; }
    /** Called when the page is shown (stop what plays elsewhere is the window's job). */
    virtual void shown() {}
    std::function<void()> onTransportChanged;
};
} // namespace aicore::ui
