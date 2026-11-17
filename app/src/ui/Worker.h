// AICore-C — the audio worker (the old AICORE's Runner in audio_studio_gui.py): one job at a time in AICore's own
// Python (D:\AICore\python, `-m aicore_studio.audio_tasks request.json`): separate, export, enhance, ace-generate,
// train, template-renders... Every stdout line is a JSON event (progress, log, done, error). The request goes to
// <projects or training folder>\jobs\<id>\request.json; Stop asks the job to end (cancel.request).
// The worker keeps its data in AICore-C's own folders (AICORE_*_DATA_DIR), never in the Core's.
#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

namespace aicore::ui
{
class Worker : private juce::Thread
{
public:
    Worker() : Thread ("aicore-worker") {}
    ~Worker() override;

    /** kind + its fields (a DynamicObject var). Fails (returns the reason) when a job runs. */
    juce::String start (const juce::String& kind, const juce::var& fields);
    /** The same protocol from one of AICore's own scripts (services\...): python -I <script> request.json. */
    juce::String startScript (const juce::File& script, const juce::var& fields);
    void stop();
    bool running() const { return isThreadRunning(); }
    juce::File outFolder() const { return out; }

    std::function<void (const juce::var& event)> onEvent;  // progress / log / provider
    std::function<void (const juce::var& result)> onDone;
    std::function<void (const juce::String& message)> onError;
    std::function<void()> onFinished;

    /** The settings dict every job carries (the old load_settings keys, AICore-C's values). */
    static juce::var settings();
    /** The environment that keeps the worker in AICore's folders. */
    static juce::StringPairArray environment();
    static juce::File python();

private:
    void run() override;
    juce::String begin (const juce::String& kind, const juce::var& fields, const juce::File& script);
    juce::String commandLine;
    juce::File out;
};
} // namespace aicore::ui
