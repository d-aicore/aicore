// AICore-C — the app's own settings (SETTINGS > FOLDERS / MODELS ...): %APPDATA%\AICore-C\Studio\audio-workspace.json
// (the audio worker reads the same file: AICORE_STUDIO_DATA_DIR points it there).
// The same keys and defaults as the old AICORE's audio-workspace.json (audio_workspace.load_settings), but its
// own file: AICore reads nothing of the Core's. Home = AICORE_HOME, else D:\AICore.
#pragma once

#include <juce_core/juce_core.h>

namespace aicore::ui
{
class AppSettings
{
public:
    static juce::File home();
    static juce::File file();
    /** A setting, or its default when it is not set (or empty). */
    static juce::String get (const juce::String& key);
    static void set (const juce::String& key, const juce::String& value);
    /** A setting that is not text (keybinds: an object), as stored. */
    static juce::var getVar (const juce::String& key);
    static void setVar (const juce::String& key, const juce::var& value);
    /** Every key of the old audio-workspace.json (but keybinds). */
    static juce::StringArray keys();
    /** The samples folder: Samples folder setting, else <projects folder>\Samples (made when missing). */
    static juce::File samplesRoot();
};
} // namespace aicore::ui
