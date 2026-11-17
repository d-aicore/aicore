#include "ServiceJob.h"

namespace aicore::ui
{
juce::File servicesRoot()
{
    auto env = juce::SystemStats::getEnvironmentVariable ("AICORE_SERVICES_DIR", {});
    if (env.isNotEmpty())
        return juce::File (env);
    for (auto d = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
         d.getParentDirectory() != d; d = d.getParentDirectory())
        if (d.getChildFile ("services").getChildFile ("midi").getChildFile ("aicore_midi_service.py").existsAsFile())
            return d;
    return juce::File ("D:\\AICore\\development\\0.1.0\\Baseline");
}

juce::File newJobFolder (const juce::String& name)
{
    auto base = juce::SystemStats::getEnvironmentVariable ("AICORE_JOBS_DIR", "D:\\AICore\\projects\\aicore-c");
    auto safe = juce::File::createLegalFileName (name).replaceCharacter (' ', '-');
    auto dir = juce::File (base).getChildFile (safe + "-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S"));
    dir.createDirectory();
    return dir;
}

bool ServiceJob::start (const juce::StringArray& command, const juce::var& request, const juce::File& workDir,
                        const juce::StringPairArray& env, Callbacks cb)
{
    if (isThreadRunning())
        return false;
    workDir.createDirectory();
    auto req = workDir.getChildFile ("request.json");
    if (! req.replaceWithText (juce::JSON::toString (request, true) + "\n"))
        return false;
    logFile = workDir.getChildFile ("service.stderr.log");
    juce::String sets;
    for (auto& k : env.getAllKeys())
        sets << "set \"" << k << "=" << env[k] << "\"&& ";
    juce::StringArray quoted;
    for (auto& c : command)
        quoted.add (c.quoted());
    // cmd /s /c "<sets> "prog" args < "req" 2> "log""
    commandLine = "cmd.exe /s /c \"" + sets + quoted.joinIntoString (" ") + " < " + req.getFullPathName().quoted()
                  + " 2> " + logFile.getFullPathName().quoted() + "\"";
    callbacks = std::move (cb);
    startThread();
    return true;
}

void ServiceJob::run()
{
    juce::ChildProcess child;
    juce::var terminal;
    int code = -1;
    if (child.start (commandLine, juce::ChildProcess::wantStdOut))
    {
        juce::String pending;
        char buf[4096];
        for (;;)
        {
            const int n = child.readProcessOutput (buf, sizeof (buf));
            if (n <= 0)
            {
                if (! child.isRunning())
                    break;
                wait (20);
                continue;
            }
            pending += juce::String::fromUTF8 (buf, n);
            int nl;
            while ((nl = pending.indexOfChar ('\n')) >= 0)
            {
                const auto line = pending.substring (0, nl).trim();
                pending = pending.substring (nl + 1);
                juce::var d;
                if (line.isEmpty() || juce::JSON::parse (line, d).failed())
                    continue;
                const auto ev = d["event"].toString();
                if (ev == "progress" && callbacks.progress)
                    juce::MessageManager::callAsync ([cb = callbacks.progress, d] {
                        cb (d["phase"].toString(), (double) d["fraction"], d["message"].toString());
                    });
                else if (ev == "done" || ev == "error")
                    terminal = d;
            }
        }
        child.waitForProcessToFinish (10000);
        code = (int) child.getExitCode();
    }
    const auto tail = logFile.loadFileAsString().getLastCharacters (600);
    if (callbacks.finished)
        juce::MessageManager::callAsync ([cb = callbacks.finished, terminal, code, tail] { cb (terminal, code, tail); });
}
} // namespace aicore::ui
