#include "AppSettings.h"

namespace aicore::ui
{
namespace
{
juce::var load()
{
    auto f = AppSettings::file();
    auto v = f.existsAsFile() ? juce::JSON::parse (f) : juce::var();
    return v.isObject() ? v : juce::var (new juce::DynamicObject());
}
} // namespace

juce::File AppSettings::home()
{
    auto env = juce::SystemStats::getEnvironmentVariable ("AICORE_HOME", {}).trim();
    return juce::File (env.isNotEmpty() ? env : juce::String ("D:\\AICore"));
}

juce::File AppSettings::file()
{
    auto env = juce::SystemStats::getEnvironmentVariable ("AICORE_SETTINGS_DIR", {}).trim();
    auto dir = env.isNotEmpty() ? juce::File (env)
                                : juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("AICore-C");
    return dir.getChildFile ("Studio").getChildFile ("audio-workspace.json");
}

juce::String AppSettings::get (const juce::String& key)
{
    auto v = load()[juce::Identifier (key)].toString().trim();
    if (v.isNotEmpty())
        return v;
    const auto h = home();
    if (key == "workspaceRoot") return h.getChildFile ("projects").getFullPathName();
    if (key == "trainingRoot") return h.getChildFile ("training").getFullPathName();
    if (key == "modelsRoot") return h.getChildFile ("models").getFullPathName();
    if (key == "aceUrl") return "http://127.0.0.1:8001";
    if (key == "demucsPython") return h.getChildFile ("models").getChildFile ("stems-env").getChildFile ("Scripts").getChildFile ("python.exe").getFullPathName();
    if (key == "coreTemplatesRoot") return h.getChildFile ("templates").getFullPathName(); // AICore's own, never the Core's
    if (key == "modelSource") return "huggingface";
    if (key == "aceRuntime") return "native";
    if (key == "wslDistribution") return "Ubuntu";
    if (key == "aceRepository") return "https://github.com/ace-step/ACE-Step-1.5.git";
    if (key == "aceRevision") return "ca1e85fe9430179831e6bc6be790c332190a3866";
    if (key == "aceWeightsRepository") return "ACE-Step/Ace-Step1.5";
    if (key == "aceWeightsRevision") return "main";
    if (key == "flamingoRepository") return "nvidia/music-flamingo-hf";
    if (key == "flamingoRevision") return "main";
    if (key == "modelHubUrl") return "https://huggingface.co";
    if (key == "demucsNamespace") return "adefossez";
    return {};
}

juce::StringArray AppSettings::keys()
{
    return { "trainingRoot", "workspaceRoot", "modelsRoot", "sampleRoot", "modelSource", "modelSourceUrl", "coreTemplatesRoot",
             "demucsPython", "aceUrl", "aceRuntime", "wslDistribution", "aceRepository", "aceRevision", "aceWeightsRepository",
             "aceWeightsRevision", "flamingoRepository", "flamingoRevision", "modelHubUrl", "demucsNamespace" };
}

juce::var AppSettings::getVar (const juce::String& key)
{
    return load()[juce::Identifier (key)];
}

void AppSettings::setVar (const juce::String& key, const juce::var& value)
{
    auto v = load();
    v.getDynamicObject()->setProperty (juce::Identifier (key), value);
    auto f = file();
    f.getParentDirectory().createDirectory();
    f.replaceWithText (juce::JSON::toString (v, false) + "\n");
}

void AppSettings::set (const juce::String& key, const juce::String& value)
{
    auto v = load();
    v.getDynamicObject()->setProperty (juce::Identifier (key), value);
    auto f = file();
    f.getParentDirectory().createDirectory();
    f.replaceWithText (juce::JSON::toString (v, false) + "\n");
}

juce::File AppSettings::samplesRoot()
{
    auto s = get ("sampleRoot");
    if (s.isNotEmpty())
    {
        juce::File p (s);
        if (p.hasFileExtension ("json"))
            p = p.getParentDirectory();
        if (p.isDirectory())
            return p;
    }
    auto folder = juce::File (get ("workspaceRoot")).getChildFile ("Samples");
    folder.createDirectory();
    return folder.isDirectory() ? folder : juce::File();
}
} // namespace aicore::ui
