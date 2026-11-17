// AICore-C — the window's keys (the old AICORE's settings_gui.KEYBINDS): name, what it does, where, default.
// SETTINGS > KEYBINDS changes them; they are kept in the settings ("keybinds": only the ones that differ from the
// default, as the old app) and every page asks matches() instead of a fixed key.
#pragma once

#include "AppSettings.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace aicore::ui
{
struct Keybind { const char* name; const char* what; const char* where; const char* key; };
inline const Keybind kKeybinds[] = {
    { "toggle_play", "Play / Stop", "All", "Space" },        { "undo", "Undo", "All", "Ctrl+Z" },
    { "redo", "Redo", "All", "Ctrl+Shift+Z" },               { "cut", "Cut", "EDIT", "Ctrl+X" },
    { "copy", "Copy", "EDIT", "Ctrl+C" },                    { "paste", "Paste", "EDIT", "Ctrl+V" },
    { "delete_selection", "Delete", "EDIT", "Del" },         { "select_all", "Select all", "EDIT", "Ctrl+A" },
    { "import_audio", "Import audio", "EDIT", "Ctrl+O" },    { "export", "Export (Tracks: split)", "EDIT", "Ctrl+E" },
    { "go_start", "Back to start", "Tracks", "Home" },       { "split", "Split at the playhead", "Tracks", "S" },
    { "duplicate", "Duplicate", "Tracks", "Ctrl+D" },        { "add_track", "New track", "Tracks", "Ctrl+T" } };

struct Keys
{
    /** The key text chosen for name (Qt's portable text, as the old app saves it), else the default. */
    static juce::String keyFor (const juce::String& name)
    {
        auto chosen = AppSettings::getVar ("keybinds");
        if (chosen.hasProperty (juce::Identifier (name)) && chosen[juce::Identifier (name)].toString().isNotEmpty())
            return chosen[juce::Identifier (name)].toString();
        for (auto& k : kKeybinds)
            if (name == k.name)
                return k.key;
        return {};
    }
    /** "Ctrl+Shift+Z" -> a JUCE KeyPress. */
    static juce::KeyPress toKeyPress (const juce::String& text)
    {
        auto parts = juce::StringArray::fromTokens (text, "+", "");
        parts.trim();
        int mods = 0;
        juce::String key;
        for (auto& p : parts)
        {
            const auto l = p.toLowerCase();
            if (l == "ctrl") mods |= juce::ModifierKeys::commandModifier;
            else if (l == "shift") mods |= juce::ModifierKeys::shiftModifier;
            else if (l == "alt") mods |= juce::ModifierKeys::altModifier;
            else key = p;
        }
        const auto lk = key.toLowerCase();
        int code = 0;
        if (lk == "space") code = juce::KeyPress::spaceKey;
        else if (lk == "del" || lk == "delete") code = juce::KeyPress::deleteKey;
        else if (lk == "home") code = juce::KeyPress::homeKey;
        else if (lk == "end") code = juce::KeyPress::endKey;
        else if (lk == "backspace") code = juce::KeyPress::backspaceKey;
        else if (lk == "return" || lk == "enter") code = juce::KeyPress::returnKey;
        else if (lk == "esc" || lk == "escape") code = juce::KeyPress::escapeKey;
        else if (lk == "left") code = juce::KeyPress::leftKey;
        else if (lk == "right") code = juce::KeyPress::rightKey;
        else if (lk == "up") code = juce::KeyPress::upKey;
        else if (lk == "down") code = juce::KeyPress::downKey;
        else if (lk.startsWith ("f") && lk.length() > 1 && lk.substring (1).containsOnly ("0123456789"))
            code = juce::KeyPress::F1Key + lk.substring (1).getIntValue() - 1;
        else if (key.length() == 1)
            code = (int) key.toUpperCase()[0];
        return code == 0 ? juce::KeyPress() : juce::KeyPress (code, juce::ModifierKeys (mods), 0);
    }
    /** The key text of a key press ("Ctrl+Shift+Z"), as the old app writes it. */
    static juce::String toText (const juce::KeyPress& k)
    {
        juce::String s;
        const auto m = k.getModifiers();
        if (m.isCommandDown()) s << "Ctrl+";
        if (m.isAltDown()) s << "Alt+";
        if (m.isShiftDown()) s << "Shift+";
        const int c = k.getKeyCode();
        if (c == juce::KeyPress::spaceKey) s << "Space";
        else if (c == juce::KeyPress::deleteKey) s << "Del";
        else if (c == juce::KeyPress::homeKey) s << "Home";
        else if (c == juce::KeyPress::endKey) s << "End";
        else if (c == juce::KeyPress::backspaceKey) s << "Backspace";
        else if (c == juce::KeyPress::returnKey) s << "Return";
        else if (c >= juce::KeyPress::F1Key && c <= juce::KeyPress::F12Key) s << "F" << juce::String (c - juce::KeyPress::F1Key + 1);
        else if (c > 32 && c < 127) s << juce::String::charToString ((juce::juce_wchar) juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) c));
        else return {};
        return s;
    }
    static bool matches (const juce::KeyPress& k, const juce::String& name)
    {
        const auto want = toKeyPress (keyFor (name));
        if (! want.isValid())
            return false;
        const auto a = k.getModifiers(), b = want.getModifiers();
        const bool sameMods = a.isCommandDown() == b.isCommandDown() && a.isShiftDown() == b.isShiftDown() && a.isAltDown() == b.isAltDown();
        const int kc = k.getKeyCode() >= 'a' && k.getKeyCode() <= 'z' ? k.getKeyCode() - 32 : k.getKeyCode();
        return sameMods && kc == want.getKeyCode();
    }
};
} // namespace aicore::ui
