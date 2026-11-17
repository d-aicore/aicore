// AICore-C — runs one contract job (aicore.service/1) in a child process and streams its lines.
//
// The request goes in a file and is fed to the service's stdin through `cmd /c "... < req.json"`
// (juce::ChildProcess has no stdin pipe). Every stdout line is one JSON event; progress and the
// terminal line (done | error) come back on the message thread.
#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <functional>

namespace aicore::ui
{
class ServiceJob : private juce::Thread
{
public:
    struct Callbacks
    {
        std::function<void (const juce::String& phase, double fraction, const juce::String& message)> progress;
        std::function<void (const juce::var& terminal, int exitCode, const juce::String& stderrTail)> finished;
    };

    ServiceJob() : Thread ("aicore-job") {}
    ~ServiceJob() override { stopThread (10000); }

    /** command = the program and its args (e.g. python.exe service.py job); env = extra KEY=VALUE pairs. */
    bool start (const juce::StringArray& command, const juce::var& request, const juce::File& workDir,
                const juce::StringPairArray& env, Callbacks cb);
    bool isRunning() const { return isThreadRunning(); }

private:
    void run() override;

    juce::String commandLine;
    juce::File logFile;
    Callbacks callbacks;
};

/** Folder where the services' Python scripts are: AICORE_SERVICES_DIR, the repo this exe was built
    from (walking up), else the Baseline checkout. */
juce::File servicesRoot();
/** A fresh output folder for a job: D:\AICore\projects\aicore-c\<name>-<yyyymmdd-hhmmss>. */
juce::File newJobFolder (const juce::String& name);
} // namespace aicore::ui
