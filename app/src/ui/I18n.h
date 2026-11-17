// AICore-C — the language of the app (the old AICORE's i18n.py): every text is written in English; with Portuguese
// chosen (SETTINGS > FOLDERS > Language, read once at start, as the old app), tr() answers from the old app's own
// table (i18n-pt.json next to the exe: the 876 texts of i18n.PT). A text missing there stays in English.
// Texts are translated where they are drawn (buttons, labels, menus, headers, tooltips, painted titles), so a
// message made at run time ("Opened: x") is translated by its known start.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace aicore::ui
{
struct I18n
{
    /** Reads the choice and the table (once). */
    static void init();
    static bool portuguese();
    static juce::String tr (const juce::String& english);
    /** The placeholders of the text fields under root (drawn by the field itself, not by the look and feel). */
    static void translateTree (juce::Component& root);
    /** Upper case that also knows the Portuguese letters (á, ã, ç...: Windows' C locale does not). */
    static juce::String upper (const juce::String&);
};

inline juce::String tr (const juce::String& english) { return I18n::tr (english); }
inline juce::String upper (const juce::String& s) { return I18n::upper (s); }
} // namespace aicore::ui
