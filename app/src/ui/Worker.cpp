#include "Worker.h"

#include "AppSettings.h"

namespace aicore::ui
{
Worker::~Worker()
{
    stop();
    stopThread (15000);
}

juce::File Worker::python()
{
    return AppSettings::home().getChildFile ("python").getChildFile ("Scripts").getChildFile ("python.exe");
}

juce::StringPairArray Worker::environment()
{
    juce::StringPairArray env;
    const auto data = AppSettings::file().getParentDirectory(); // %APPDATA%\AICore-C\Studio
    const auto base = data.getParentDirectory();
    env.set ("AICORE_HOME", AppSettings::home().getFullPathName());
    env.set ("AICORE_STUDIO_DATA_DIR", data.getFullPathName());
    env.set ("AICORE_MEMORY_DATA_DIR", base.getChildFile ("Memory").getFullPathName());
    env.set ("AICORE_WORKER_DATA_DIR", base.getChildFile ("Worker").getFullPathName());
    env.set ("PYTHONIOENCODING", "utf-8");
    return env;
}

juce::var Worker::settings()
{
    auto* o = new juce::DynamicObject();
    for (auto key : AppSettings::keys())
        o->setProperty (juce::Identifier (key), AppSettings::get (key));
    auto keys = AppSettings::getVar ("keybinds");
    o->setProperty ("keybinds", keys.isObject() ? keys : juce::var (new juce::DynamicObject()));
    o->setProperty ("language", AppSettings::get ("language").isNotEmpty() ? AppSettings::get ("language") : juce::String ("en"));
    return juce::var (o);
}

juce::String Worker::start (const juce::String& kind, const juce::var& fields) { return begin (kind, fields, {}); }

juce::String Worker::startScript (const juce::File& script, const juce::var& fields)
{
    if (! script.existsAsFile())
        return "Not found: " + script.getFullPathName();
    return begin (script.getFileNameWithoutExtension(), fields, script);
}

juce::String Worker::begin (const juce::String& kind, const juce::var& fields, const juce::File& script)
{
    if (running())
        return "Wait for the current job to finish.";
    if (! python().existsAsFile())
        return "AICore's Python was not found: " + python().getFullPathName();
    const auto id = juce::Uuid().toString().removeCharacters ("-");
    const auto root = AppSettings::get (kind == "train" ? "trainingRoot" : "workspaceRoot");
    out = juce::File (root).getChildFile ("jobs").getChildFile (id);
    if (! out.createDirectory())
        return "Could not make the job folder: " + out.getFullPathName();
    auto* job = new juce::DynamicObject();
    if (auto* f = fields.getDynamicObject())
        for (auto& p : f->getProperties())
            job->setProperty (p.name, p.value);
    job->setProperty ("id", id);
    job->setProperty ("kind", kind);
    job->setProperty ("settings", settings());
    job->setProperty ("out", out.getFullPathName());
    auto request = out.getChildFile ("request.json");
    if (! request.replaceWithText (juce::JSON::toString (juce::var (job), false)))
        return "Could not write the job request.";
    juce::String sets;
    auto env = environment();
    for (auto& k : env.getAllKeys())
        sets << "set \"" << k << "=" << env[k] << "\"&& ";
    commandLine = "cmd.exe /s /c \"" + sets + python().getFullPathName().quoted()
                  + (script.existsAsFile() ? " -I " + script.getFullPathName().quoted() + " " : juce::String (" -m aicore_studio.audio_tasks "))
                  + request.getFullPathName().quoted() + " 2> " + out.getChildFile ("worker.stderr.log").getFullPathName().quoted() + "\"";
    startThread();
    return {};
}

void Worker::stop()
{
    if (running() && out != juce::File())
        out.getChildFile ("cancel.request").create();
}

void Worker::run()
{
    juce::ChildProcess child;
    juce::var result;
    juce::String lastError;
    bool gotDone = false;
    auto post = [this] (std::function<void()> fn) { juce::MessageManager::callAsync (std::move (fn)); };
    if (! child.start (commandLine, juce::ChildProcess::wantStdOut))
    {
        post ([this] { if (onError) onError ("The audio worker could not run."); if (onFinished) onFinished(); });
        return;
    }
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
            if (line.isEmpty() || juce::JSON::parse (line, d).failed() || ! d.isObject())
                continue;
            const auto ev = d["event"].toString();
            if (ev == "done")
            {
                result = d["result"];
                gotDone = true;
            }
            else if (ev == "error")
            {
                lastError = d["message"].toString();
                post ([this, lastError] { if (onError) onError (lastError); });
            }
            else
                post ([this, d] { if (onEvent) onEvent (d); });
        }
    }
    child.waitForProcessToFinish (10000);
    const int code = (int) child.getExitCode();
    const auto tail = out.getChildFile ("worker.stderr.log").loadFileAsString().getLastCharacters (1500);
    post ([this, code, gotDone, result, lastError, tail] {
        if (tail.isNotEmpty() && onEvent)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("event", "log");
            o->setProperty ("text", tail);
            onEvent (juce::var (o));
        }
        if (code == 0 && gotDone)
        {
            if (onDone) onDone (result);
        }
        else if (lastError.isEmpty() && onError)
            onError ("The job did not succeed. The original is kept; see the state and log.");
        if (onFinished) onFinished();
    });
}
} // namespace aicore::ui
