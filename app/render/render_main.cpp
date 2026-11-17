// aicore-render — S5: render a .mid through one VST3 instrument, offline, no Core open.
//
// Contract aicore.service/1 (S1b transport, profile "render"): one JSON request line on stdin,
// one JSON object per line on stdout (start -> progress* -> done | error), logs on stderr,
// exit 0 only after done, 2 after error. Host = juce_audio_processors_headless (VST3 only):
// the native app's own host, no Python, no GPL in our code (PLAN S5 recommendation).
//
//   aicore-render hello
//   aicore-render job < request.json
//
// Input .mid must be PPQ 960 (S1b): anything else is UNSUPPORTED_FORMAT, like the Python reader.
// Output: <outputDir>/render.wav (float32 stereo) + report.json + manifest.json, each written to
// <outputDir>/.staging/<taskId>/<name>.part and renamed in; an error publishes nothing.

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors_headless/juce_audio_processors_headless.h>
#include <juce_cryptography/juce_cryptography.h>

#include "aicore/version.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>

#if JUCE_WINDOWS
 #include <fcntl.h>
 #include <io.h>
 #include <windows.h>
#endif

using namespace juce;

namespace
{
const char* kContract = "aicore.service/1";
const char* kServiceName = "aicore-render";
constexpr int kPpq = 960;
constexpr int64 kMaxMidiBytes = 8 * 1024 * 1024;
constexpr int64 kMaxWavBytes = 128 * 1024 * 1024;
constexpr double kMaxSeconds = 600.0;
const int warmupMs = [] { auto v = SystemStats::getEnvironmentVariable ("AICORE_RENDER_WARMUP_MS", "1500").getIntValue(); return jlimit (0, 30000, v); }();

FILE* protoOut = stdout;

String utcStamp()
{
    std::time_t now = std::time (nullptr);
    std::tm g {};
   #if JUCE_WINDOWS
    gmtime_s (&g, &now);
   #else
    gmtime_r (&now, &g);
   #endif
    char buf[32];
    std::strftime (buf, sizeof (buf), "%Y-%m-%dT%H:%M:%SZ", &g);
    return String (buf);
}

void emit (const var& doc)
{
    auto line = JSON::toString (doc, JSON::FormatOptions{}.withSpacing (JSON::Spacing::none)
                                                          .withEncoding (JSON::Encoding::ascii));
    std::fputs (line.toRawUTF8(), protoOut);
    std::fputc ('\n', protoOut);
    std::fflush (protoOut);
}

void logLine (const String& s)
{
    std::cerr << "[aicore-render] " << s.toStdString() << std::endl;
}

/** stdout = protocol lines only (CONTRACT.md §2.2). Plugins print to stdout, so fd 1 goes to stderr
    and protocol lines go to a private duplicate of the real stdout. */
void protectStdout()
{
#if JUCE_WINDOWS
    std::fflush (stdout);
    const int fd = _dup (_fileno (stdout));
    _dup2 (_fileno (stderr), _fileno (stdout));
    _setmode (fd, _O_BINARY);
    protoOut = _fdopen (fd, "wb");
#endif
}

void lowerPriority()
{
#if JUCE_WINDOWS
    SetPriorityClass (GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#endif
}

struct ContractError
{
    String code, message;
    var details;
};

[[noreturn]] void fail (const String& code, const String& message, var details = {})
{
    throw ContractError { code, message.substring (0, 500), details };
}

DynamicObject::Ptr obj() { return new DynamicObject(); }

String sha256Of (const File& f)
{
    FileInputStream in (f);
    if (! in.openedOk())
        fail ("BACKEND_FAILED", "cannot read " + f.getFullPathName());
    return SHA256 (in).toHexString();
}

bool isAbsolutePath (const String& p)
{
    return File::isAbsolutePath (p) && ! p.containsAnyOf ("\r\n\t");
}

var errorDoc (const ContractError& e, const String& rid)
{
    auto err = obj();
    err->setProperty ("code", e.code);
    err->setProperty ("message", e.message.isEmpty() ? e.code : e.message);
    err->setProperty ("requestId", rid);
    err->setProperty ("retryable", e.code == "WORKER_UNAVAILABLE");
    if (e.details.isObject())
        err->setProperty ("details", e.details);
    return var (err.get());
}

//==============================================================================
struct Effect
{
    String plugin, stateBase64;
};

const StringArray& instrumentRoles() { static const StringArray r { "bass", "lead", "pad", "pluck", "riser", "drums" }; return r; }
const StringArray& effectRoles() { static const StringArray r { "reverb", "delay", "filter", "sidechain", "drive" }; return r; }
String rolesPath;

/** AICore's own settings folder (never the Core's, owner 2026-11-16: "pode ter a mesma lista mas nao le
    atraves do core"): %APPDATA%\AICore-C, or AICORE_SETTINGS_DIR. Holds plugins.json (this app's own
    scan) and roles.json (plugin per role, chosen in Definicoes). Same shapes as the Core's files, so a
    list can be compared by eye, but nothing is read from the Core. */
File settingsDir()
{
    auto env = SystemStats::getEnvironmentVariable ("AICORE_SETTINGS_DIR", {});
    if (env.isNotEmpty())
        return File (env);
    return File::getSpecialLocation (File::userApplicationDataDirectory).getChildFile ("AICore-C");
}

File pluginListFile() { return settingsDir().getChildFile ("plugins.json"); }
File rolesFile() { return settingsDir().getChildFile ("roles.json"); }

var loadJsonFile (const File& f, const String& what)
{
    if (! f.existsAsFile())
        fail ("BROKEN_REFERENCE", what + " not found: " + f.getFullPathName() + " (Definicoes > Procurar plugins)");
    var doc;
    if (JSON::parse (f.loadFileAsString(), doc).failed() || ! doc.isObject())
        fail ("UNSUPPORTED_FORMAT", what + " is not readable JSON: " + f.getFullPathName());
    return doc;
}

void writeJsonAtomically (const File& f, const var& doc)
{
    f.getParentDirectory().createDirectory();
    auto tmp = f.getSiblingFile (f.getFileName() + ".part");
    if (! tmp.replaceWithText (JSON::toString (doc, JSON::FormatOptions{}.withEncoding (JSON::Encoding::ascii)) + "\n")
        || ! tmp.moveFileTo (f))
        fail ("BACKEND_FAILED", "cannot write " + f.getFullPathName());
}

/** roles.json: {"version":1,"roles":{id:{"plugin":{"id","file"}}}}. Returns the .vst3 for the role or fails typed. */
String pluginForRole (const String& role)
{
    rolesPath = rolesFile().getFullPathName();
    auto doc = loadJsonFile (rolesFile(), "AICore role list");
    auto file = doc["roles"][Identifier (role)]["plugin"]["file"].toString();
    if (file.isEmpty())
        fail ("UNSUPPORTED_OPERATION", "no plugin is chosen for role '" + role + "' (Definicoes > Papeis)");
    return file;
}

String catalogPath, catalogSha; // AICore's own plugin list this job was checked against

/** Returns the entry of `path` in AICore's own plugin list (from `aicore-render scan`), or fails typed. */
var findInPluginList (const String& path)
{
    auto cat = pluginListFile();
    catalogPath = cat.getFullPathName();
    auto doc = loadJsonFile (cat, "AICore plugin list");
    catalogSha = sha256Of (cat);
    const auto want = File (path).getFullPathName();
    if (auto* q = doc["quarantine"].getArray())
        for (auto& v : *q)
            if (File (v["file"].toString()).getFullPathName().equalsIgnoreCase (want))
                fail ("UNSUPPORTED_OPERATION", "plugin is quarantined (failed the scan): " + File (path).getFileName());
    if (auto* pl = doc["plugins"].getArray())
        for (auto& e : *pl)
            if (File (e["file"].toString()).getFullPathName().equalsIgnoreCase (want))
                return e;
    auto d = obj();
    d->setProperty ("plugin", path);
    d->setProperty ("list", catalogPath);
    fail ("UNSUPPORTED_OPERATION", "plugin is not in the AICore plugin list: " + File (path).getFileName()
                                   + " (Definicoes > Procurar plugins)", var (d.get()));
}

void checkInPluginList (const String& path) { findInPluginList (path); }

struct Request
{
    String requestId, outputDir, midiPath, audioPath, plugin, pluginState, pluginStateBase64;
    std::vector<Effect> effects;
    String role;                 // instrument role resolved through AICore's roles.json
    bool sampler = false;        // engine "sampler": AICore's own sampler, params.kit
    bool sfizz = false;          // engine "sfizz": sfizz_render plays params.sfz (an SFZ kit), the dry WAV then goes through the effects
    String sfz;
    std::vector<std::pair<int, String>> kit;
    StringArray tracks;          // only these MIDI tracks (by track name); empty = all
    StringArray effectRoleIds;   // effect roles, appended after explicit effects
    var params;
    double sampleRate = 44100, tailSeconds = 2, gainDb = 0, lengthSeconds = -1;
    int blockSize = 512;
    bool synthetic = false;
    var inputs;
};

double num (const var& v, const char* what)
{
    if (! (v.isInt() || v.isInt64() || v.isDouble()) || v.isBool())
        fail ("SCHEMA_INVALID", String ("params.") + what + " must be a number");
    return (double) v;
}

Request validate (const var& doc)
{
    if (! doc.isObject())
        fail ("SCHEMA_INVALID", "request line must be a JSON object");
    auto* o = doc.getDynamicObject();
    static const StringArray allowedTop { "schemaVersion", "requestId", "profile", "operation", "params",
                                          "inputs", "outputDir", "synthetic" };
    for (auto& p : o->getProperties())
        if (! allowedTop.contains (p.name.toString()))
            fail ("SCHEMA_INVALID", "unknown field '" + p.name.toString() + "' (additionalProperties: false)");
    for (auto k : { "schemaVersion", "requestId", "profile", "operation", "params", "inputs", "outputDir" })
        if (! o->hasProperty (k))
            fail ("SCHEMA_INVALID", String ("missing required field ") + k);
    if (o->getProperty ("schemaVersion").toString() != "1.0")
        fail ("UNSUPPORTED_SCHEMA", "only schemaVersion 1.0 is supported");
    Request r;
    r.requestId = o->getProperty ("requestId").toString();
    if (r.requestId.isEmpty() || r.requestId.length() > 128
        || ! r.requestId.containsOnly ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._:-"))
        fail ("SCHEMA_INVALID", "requestId does not match the id pattern");
    if (o->getProperty ("profile").toString() != "render" || o->getProperty ("operation").toString() != "render")
        fail ("UNSUPPORTED_OPERATION", o->getProperty ("profile").toString() + "/"
                                         + o->getProperty ("operation").toString() + " not offered by aicore-render");
    r.synthetic = (bool) o->getProperty ("synthetic");
    r.outputDir = o->getProperty ("outputDir").toString();
    if (! isAbsolutePath (r.outputDir))
        fail ("SCHEMA_INVALID", "outputDir must be an absolute path");

    r.params = o->getProperty ("params");
    if (! r.params.isObject())
        fail ("SCHEMA_INVALID", "params must be an object");
    auto* p = r.params.getDynamicObject();
    static const StringArray allowedParams { "plugin", "role", "effectRoles", "engine", "preset", "kit", "sfz", "tracks", "pluginState", "pluginStateBase64", "effects", "sampleRate",
                                             "blockSize", "tailSeconds", "lengthSeconds", "gainDb" };
    for (auto& kv : p->getProperties())
        if (! allowedParams.contains (kv.name.toString()))
            fail ("SCHEMA_INVALID", "unknown params field '" + kv.name.toString() + "'");
    const String engine = p->hasProperty ("engine") ? p->getProperty ("engine").toString() : String ("vst3");
    if (engine != "vst3" && engine != "sampler" && engine != "sfizz")
        fail ("UNSUPPORTED_OPERATION", "engine '" + engine + "' is not rendered here (vita: services/vita)");
    r.sampler = engine == "sampler";
    r.sfizz = engine == "sfizz";
    if (r.sfizz)
    {
        // sfizz (owner 2026-11-16: drums and samples through SFZ kits; he authorised running the downloaded sfizz_render)
        r.sfz = p->getProperty ("sfz").toString();
        if (! isAbsolutePath (r.sfz) || ! r.sfz.endsWithIgnoreCase (".sfz"))
            fail ("SCHEMA_INVALID", "engine sfizz needs params.sfz, an absolute path to a .sfz kit");
        if (p->hasProperty ("plugin") || p->hasProperty ("role") || p->hasProperty ("kit") || p->hasProperty ("tracks"))
            fail ("SCHEMA_INVALID", "engine sfizz takes params.sfz (and effects), not plugin/role/kit/tracks");
    }
    else if (p->hasProperty ("sfz"))
        fail ("SCHEMA_INVALID", "params.sfz belongs to engine sfizz");
    if (r.sampler)
    {
        // AICore's own sampler (owner: drums from tagged samples; never the Core's sampler):
        // params.kit = [{"note": 0-127, "sample": absolute .wav}], one sample per note.
        auto kit = p->getProperty ("kit");
        if (! kit.isArray() || kit.size() < 1 || kit.size() > 128)
            fail ("SCHEMA_INVALID", "engine sampler needs params.kit with 1-128 entries");
        Array<int> seen;
        for (auto& k : *kit.getArray())
        {
            if (! k.isObject() || ! (k["note"].isInt() || k["note"].isInt64()) || (int) k["note"] < 0 || (int) k["note"] > 127
                || ! isAbsolutePath (k["sample"].toString()) || ! k["sample"].toString().endsWithIgnoreCase (".wav"))
                fail ("SCHEMA_INVALID", "each kit entry needs note 0-127 and an absolute .wav sample");
            if (seen.contains ((int) k["note"]))
                fail ("SCHEMA_INVALID", "kit has two samples for note " + String ((int) k["note"]));
            seen.add ((int) k["note"]);
            r.kit.push_back ({ (int) k["note"], k["sample"].toString() });
        }
        if (p->hasProperty ("plugin") || p->hasProperty ("role"))
            fail ("SCHEMA_INVALID", "engine sampler takes params.kit, not plugin/role");
    }
    else if (p->hasProperty ("kit"))
        fail ("SCHEMA_INVALID", "params.kit belongs to engine sampler");
    if (p->hasProperty ("preset"))
        fail ("SCHEMA_INVALID", "params.preset belongs to engine vita");
    if (p->hasProperty ("tracks"))
    {
        auto tv = p->getProperty ("tracks");
        if (! tv.isArray() || tv.size() < 1 || tv.size() > 64)
            fail ("SCHEMA_INVALID", "params.tracks must be a list of 1-64 track names");
        for (auto& v : *tv.getArray())
            if (v.toString().isEmpty())
                fail ("SCHEMA_INVALID", "params.tracks has an empty name");
            else
                r.tracks.add (v.toString());
    }
    for (auto k : { "sampleRate", "blockSize", "tailSeconds" })
        if (! p->hasProperty (k))
            fail ("SCHEMA_INVALID", String ("missing params.") + k);
    if (p->hasProperty ("plugin") && p->hasProperty ("role"))
        fail ("SCHEMA_INVALID", "params.plugin and params.role are exclusive");
    if (p->hasProperty ("role"))
    {
        r.role = p->getProperty ("role").toString();
        if (! instrumentRoles().contains (r.role))
            fail ("SCHEMA_INVALID", "params.role must be one of " + instrumentRoles().joinIntoString (", "));
    }
    else if (p->hasProperty ("plugin"))
    {
        r.plugin = p->getProperty ("plugin").toString();
        if (! isAbsolutePath (r.plugin) || ! r.plugin.endsWithIgnoreCase (".vst3"))
            fail ("SCHEMA_INVALID", "params.plugin must be an absolute path to a .vst3");
    }
    if (p->hasProperty ("effectRoles"))
    {
        auto er = p->getProperty ("effectRoles");
        if (! er.isArray() || er.size() > 5)
            fail ("SCHEMA_INVALID", "params.effectRoles must be an array of at most 5");
        for (auto& v : *er.getArray())
        {
            if (! effectRoles().contains (v.toString()) || r.effectRoleIds.contains (v.toString()))
                fail ("SCHEMA_INVALID", "params.effectRoles items must be distinct, one of " + effectRoles().joinIntoString (", "));
            r.effectRoleIds.add (v.toString());
        }
    }
    r.sampleRate = num (p->getProperty ("sampleRate"), "sampleRate");
    if (r.sampleRate != 44100 && r.sampleRate != 48000 && r.sampleRate != 96000)
        fail ("SCHEMA_INVALID", "params.sampleRate must be 44100, 48000 or 96000");
    auto bs = p->getProperty ("blockSize");
    if (! (bs.isInt() || bs.isInt64()) || (int) bs < 32 || (int) bs > 4096)
        fail ("SCHEMA_INVALID", "params.blockSize must be an integer 32..4096");
    r.blockSize = (int) bs;
    r.tailSeconds = num (p->getProperty ("tailSeconds"), "tailSeconds");
    if (r.tailSeconds < 0 || r.tailSeconds > 30)
        fail ("SCHEMA_INVALID", "params.tailSeconds must be 0..30");
    if (p->hasProperty ("gainDb"))
    {
        r.gainDb = num (p->getProperty ("gainDb"), "gainDb");
        if (r.gainDb < -60 || r.gainDb > 12)
            fail ("SCHEMA_INVALID", "params.gainDb must be -60..12");
    }
    if (p->hasProperty ("lengthSeconds") && ! p->getProperty ("lengthSeconds").isVoid())
    {
        r.lengthSeconds = num (p->getProperty ("lengthSeconds"), "lengthSeconds");
        if (r.lengthSeconds <= 0 || r.lengthSeconds > kMaxSeconds)
            fail ("SCHEMA_INVALID", "params.lengthSeconds must be in (0, 600]");
    }
    if (p->hasProperty ("pluginStateBase64"))
    {
        r.pluginStateBase64 = p->getProperty ("pluginStateBase64").toString();
        if (r.pluginStateBase64.isEmpty())
            fail ("SCHEMA_INVALID", "params.pluginStateBase64 must not be empty");
        if (p->hasProperty ("pluginState"))
            fail ("SCHEMA_INVALID", "params.pluginState and params.pluginStateBase64 are exclusive");
    }
    if (p->hasProperty ("effects"))
    {
        auto fxv = p->getProperty ("effects");
        if (! fxv.isArray() || fxv.size() > 8)
            fail ("SCHEMA_INVALID", "params.effects must be an array of at most 8");
        for (auto& e : *fxv.getArray())
        {
            if (! e.isObject())
                fail ("SCHEMA_INVALID", "each effect must be an object");
            for (auto& kv : e.getDynamicObject()->getProperties())
                if (kv.name.toString() != "plugin" && kv.name.toString() != "pluginStateBase64")
                    fail ("SCHEMA_INVALID", "unknown effect field '" + kv.name.toString() + "'");
            Effect fx { e["plugin"].toString(), e["pluginStateBase64"].toString() };
            if (! isAbsolutePath (fx.plugin) || ! fx.plugin.endsWithIgnoreCase (".vst3"))
                fail ("SCHEMA_INVALID", "each effect.plugin must be an absolute path to a .vst3");
            r.effects.push_back (fx);
        }
    }
    if (p->hasProperty ("pluginState"))
    {
        r.pluginState = p->getProperty ("pluginState").toString();
        if (! isAbsolutePath (r.pluginState))
            fail ("SCHEMA_INVALID", "params.pluginState must be an absolute path");
    }

    r.inputs = o->getProperty ("inputs");
    if (! r.inputs.isArray() || r.inputs.size() < 1)
        fail ("SCHEMA_INVALID", "inputs must be a non-empty array");
    int midis = 0, audios = 0;
    for (auto& in : *r.inputs.getArray())
    {
        if (! in.isObject() || ! isAbsolutePath (in["path"].toString()) || in["role"].toString().isEmpty())
            fail ("SCHEMA_INVALID", "each input needs role + absolute path");
        if (in["role"].toString() == "midi")
        {
            ++midis;
            r.midiPath = in["path"].toString();
        }
        else if (in["role"].toString() == "audio")
        {
            ++audios;
            r.audioPath = in["path"].toString();
        }
    }
    if (midis + audios != 1)
        fail ("SCHEMA_INVALID", "profile render needs exactly one input: role midi (instrument) or role audio (effects only)");
    const bool audioMode = audios == 1;
    if (! audioMode && ! r.sampler && ! r.sfizz && ! p->hasProperty ("plugin") && ! p->hasProperty ("role"))
        fail ("SCHEMA_INVALID", "a midi input needs params.plugin or params.role (the instrument)");
    if (audioMode && (p->hasProperty ("plugin") || p->hasProperty ("role") || r.sampler || r.sfizz))
        fail ("SCHEMA_INVALID", "an audio input takes effects only (no plugin/role instrument)");
    if (audioMode && r.effects.empty() && r.effectRoleIds.isEmpty())
        fail ("SCHEMA_INVALID", "an audio input needs params.effects or params.effectRoles");

    if (r.sfizz && ! File (r.sfz).existsAsFile())
        fail ("BROKEN_REFERENCE", "sfz kit does not exist: " + r.sfz);
    for (auto& [note, sample] : r.kit)
        if (! File (sample).existsAsFile())
            fail ("BROKEN_REFERENCE", "kit sample does not exist (note " + String (note) + "): " + sample);

    // References and declared hashes.
    if (! File (r.outputDir).isDirectory())
        fail ("BROKEN_REFERENCE", "outputDir does not exist: " + r.outputDir);
    for (auto name : { "render.wav", "report.json", "manifest.json" })
        if (File (r.outputDir).getChildFile (name).exists())
            // contract §2.7: a resend goes to a new outputDir; publishing here would overwrite an earlier job
            fail ("OUTPUT_EXISTS", "outputDir already holds a published job (" + String (name) + "); use a new outputDir");
    for (auto& in : *r.inputs.getArray())
    {
        File f (in["path"].toString());
        if (! f.existsAsFile())
        {
            auto d = obj();
            d->setProperty ("role", in["role"]);
            d->setProperty ("path", f.getFullPathName());
            fail ("BROKEN_REFERENCE", "input does not exist: " + f.getFullPathName(), var (d.get()));
        }
        if (f.getSize() > (in["role"].toString() == "audio" ? kMaxWavBytes : kMaxMidiBytes))
            fail ("LIMIT_EXCEEDED", "input exceeds the profile cap: " + f.getFileName());
        if (in.hasProperty ("bytes") && (int64) in["bytes"] != f.getSize())
            fail ("HASH_MISMATCH", "declared bytes do not match " + f.getFileName());
        if (in.hasProperty ("sha256") && in["sha256"].toString() != sha256Of (f))
            fail ("HASH_MISMATCH", "sha256 mismatch for " + f.getFileName());
    }
    if (r.role.isNotEmpty())
        r.plugin = pluginForRole (r.role);
    for (auto& er : r.effectRoleIds)
        r.effects.push_back ({ pluginForRole (er), {} });
    for (auto& path : [&] { StringArray all; if (r.plugin.isNotEmpty()) all.add (r.plugin); for (auto& e : r.effects) all.add (e.plugin); return all; }())
    {
        if (! File (path).exists())
            fail ("BROKEN_REFERENCE", "plugin does not exist: " + path);
        checkInPluginList (path);
    }
    if (r.pluginState.isNotEmpty() && ! File (r.pluginState).existsAsFile())
        fail ("BROKEN_REFERENCE", "pluginState does not exist: " + r.pluginState);
    return r;
}

//==============================================================================
struct OfflinePlayHead final : public AudioPlayHead
{
    struct Segment { double seconds, ppq, bpm; };
    double bpm = 120, sampleRate = 44100;
    int64 sample = 0;
    std::vector<Segment> tempoMap; // from the MIDI's tempo events; empty = one constant bpm

    Segment at (double secs) const
    {
        Segment s { 0.0, 0.0, bpm };
        for (auto& t : tempoMap)
            if (t.seconds <= secs)
                s = t;
            else
                break;
        return s;
    }

    Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (true);
        const double secs = (double) sample / sampleRate;
        const auto seg = at (secs);
        const double ppq = seg.ppq + (secs - seg.seconds) * seg.bpm / 60.0;
        info.setBpm (seg.bpm);
        info.setTimeSignature (TimeSignature { 4, 4 });
        info.setTimeInSamples (sample);
        info.setTimeInSeconds (secs);
        info.setPpqPosition (ppq);
        info.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
        return info;
    }
};

/** Tempo segments {start seconds, start ppq, bpm} from tick-stamped tempo events at PPQ 960. */
std::vector<OfflinePlayHead::Segment> tempoSegments (const MidiMessageSequence& tempos, int ppq)
{
    std::vector<OfflinePlayHead::Segment> out;
    double lastTick = 0.0, seconds = 0.0, spq = 0.5;
    for (auto* e : tempos)
    {
        const double tick = e->message.getTimeStamp();
        seconds += (tick - lastTick) / ppq * spq;
        lastTick = tick;
        spq = e->message.getTempoSecondsPerQuarterNote();
        if (spq <= 0.0)
            continue;
        const OfflinePlayHead::Segment s { seconds, tick / ppq, 60.0 / spq };
        if (! out.empty() && std::abs (out.back().seconds - s.seconds) < 1e-12)
            out.back() = s;
        else
            out.push_back (s);
    }
    return out;
}

class Job
{
public:
    Job (Request reqIn, String taskIdIn) : req (std::move (reqIn)), taskId (std::move (taskIdIn))
    {
        outDir = File (req.outputDir);
        staging = outDir.getChildFile (".staging").getChildFile (taskId);
    }

    void progress (const String& phase, double fraction, const String& message = {})
    {
        fraction = jlimit (lastFraction, 1.0, fraction);
        lastFraction = fraction;
        auto d = obj();
        d->setProperty ("event", "progress");
        d->setProperty ("schemaVersion", "1.0");
        d->setProperty ("eventSeq", ++seq);
        d->setProperty ("taskId", taskId);
        d->setProperty ("phase", phase);
        d->setProperty ("fraction", std::round (fraction * 10000.0) / 10000.0);
        d->setProperty ("step", (int) std::round (fraction * 100.0));
        d->setProperty ("totalSteps", 100);
        d->setProperty ("at", utcStamp());
        if (message.isNotEmpty())
            d->setProperty ("message", message.substring (0, 200));
        emit (var (d.get()));
    }

    File publish (const String& name, const MemoryBlock& data)
    {
        staging.createDirectory();
        auto part = staging.getChildFile (name + ".part");
        {
            FileOutputStream out (part);
            if (! out.openedOk())
                fail ("BACKEND_FAILED", "cannot write " + part.getFullPathName());
            out.setPosition (0);
            out.truncate();
            const bool wrote = out.write (data.getData(), data.getSize());
            out.flush();
            if (! wrote || out.getStatus().failed())
                fail ("BACKEND_FAILED", "writing " + name + " failed: " + out.getStatus().getErrorMessage());
        }
        if (part.getSize() != (int64) data.getSize())
            fail ("BACKEND_FAILED", name + " is " + String (part.getSize()) + " bytes on disk, " + String ((int64) data.getSize()) + " expected");
        auto dest = outDir.getChildFile (name);
        if (! part.moveFileTo (dest))
            fail ("BACKEND_FAILED", "rename into outputDir failed for " + name);
        published.add (dest);
        return dest;
    }

    void cleanup()
    {
        staging.deleteRecursively();
        auto parent = staging.getParentDirectory();
        if (parent.getFileName() == ".staging" && parent.getNumberOfChildFiles (File::findFilesAndDirectories) == 0)
            parent.deleteFile();
    }

    void unpublish()
    {
        for (auto& f : published)
            f.deleteFile();
        published.clear();
    }

    var run()
    {
        const auto t0 = Time::getMillisecondCounterHiRes();
        staging.createDirectory();
        const bool fromAudio = req.audioPath.isNotEmpty();
        progress ("read", 0.0, fromAudio ? "a ler o WAV" : "a ler o MIDI");
        MidiMessageSequence events;
        std::vector<OfflinePlayHead::Segment> tempoMap;
        AudioBuffer<float> dry;
        int noteOns = 0;
        double bpm = 120.0, seconds = 0.0;
        if (fromAudio)
        {
            // WAV only (not whatever format the extension or header happens to fit), from the very bytes
            // whose sha256 went into the manifest (audit: hash and processing were two separate reads)
            auto bytes = readInputOnce (req.audioPath, kMaxWavBytes);
            if (bytes.getSize() < 12 || std::memcmp (bytes.getData(), "RIFF", 4) != 0
                || std::memcmp (static_cast<const char*> (bytes.getData()) + 8, "WAVE", 4) != 0)
                fail ("UNSUPPORTED_FORMAT", "audio input is not a RIFF/WAVE file: " + req.audioPath);
            WavAudioFormat wavFmt;
            std::unique_ptr<AudioFormatReader> reader (wavFmt.createReaderFor (new MemoryInputStream (bytes, false), true));
            if (reader == nullptr)
                fail ("UNSUPPORTED_FORMAT", "not a readable WAV: " + req.audioPath);
            if ((int) reader->sampleRate != (int) req.sampleRate)
                fail ("UNSUPPORTED_FORMAT", "WAV is " + String ((int) reader->sampleRate) + " Hz, request says " + String ((int) req.sampleRate));
            if (reader->numChannels < 1 || reader->numChannels > 2 || reader->lengthInSamples <= 0
                || (double) reader->lengthInSamples / reader->sampleRate > kMaxSeconds)
                fail ("UNSUPPORTED_FORMAT", "WAV must be 1-2 channels, 0-600 s");
            const int64 dataBytes = (int64) reader->lengthInSamples * reader->numChannels * (reader->bitsPerSample / 8);
            if (dataBytes > (int64) bytes.getSize())
                fail ("UNSUPPORTED_FORMAT", "WAV data chunk is truncated");
            dry.setSize (2, (int) reader->lengthInSamples);
            if (! reader->read (&dry, 0, (int) reader->lengthInSamples, 0, true, true))
                fail ("UNSUPPORTED_FORMAT", "WAV samples could not be read");
            if (reader->numChannels == 1)
                dry.copyFrom (1, 0, dry, 0, 0, dry.getNumSamples());
            seconds = req.lengthSeconds > 0 ? req.lengthSeconds : (double) dry.getNumSamples() / req.sampleRate + req.tailSeconds;
            progress ("read", 0.05, String (dry.getNumSamples() / req.sampleRate, 2) + " s de audio");
        }
        else
        {
            // ---- MIDI (PPQ 960 only) ----
            MidiFile mf;
            {
                auto bytes = readInputOnce (req.midiPath, kMaxMidiBytes);
                MemoryInputStream in (bytes, false);
                if (! mf.readFrom (in))
                    fail ("UNSUPPORTED_FORMAT", "not a readable Standard MIDI File");
            }
            const int tf = mf.getTimeFormat();
            if (tf <= 0)
                fail ("UNSUPPORTED_FORMAT", "SMPTE time division not allowed (PPQ required)");
            if (tf != kPpq)
                fail ("UNSUPPORTED_FORMAT", "PPQ " + String (tf) + " != 960 (S1b: Core templates are 960)");
            {
                MidiMessageSequence tempos;
                mf.findAllTempoEvents (tempos);
                if (tempos.getNumEvents() == 0)
                    fail ("UNSUPPORTED_FORMAT", "no tempo map (contract requires one)");
                tempos.sort();
                tempoMap = tempoSegments (tempos, kPpq);
                if (tempoMap.empty())
                    fail ("UNSUPPORTED_FORMAT", "tempo map has no valid tempo");
                bpm = tempoMap.front().bpm;
            }
            mf.convertTimestampTicksToSeconds();
            StringArray used;
            for (int t = 0; t < mf.getNumTracks(); ++t)
            {
                String name;
                for (auto* e : *mf.getTrack (t))
                    if (e->message.isTrackNameEvent())
                    {
                        name = e->message.getTextFromTextMetaEvent();
                        break;
                    }
                if (! req.tracks.isEmpty() && ! req.tracks.contains (name, true))
                {
                    // keep the tempo map, drop the notes of the tracks this request did not ask for
                    for (auto* e : *mf.getTrack (t))
                        if (e->message.isMetaEvent())
                            events.addEvent (e->message);
                    continue;
                }
                used.add (name);
                events.addSequence (*mf.getTrack (t), 0.0);
            }
            if (! req.tracks.isEmpty())
                for (auto& want : req.tracks)
                    if (! used.contains (want, true))
                        fail ("UNSUPPORTED_FORMAT", "MIDI has no track named '" + want + "'");
            events.updateMatchedPairs();
            double lastEvent = 0.0;
            for (auto* e : events)
            {
                if (e->message.isNoteOn())
                    ++noteOns;
                lastEvent = jmax (lastEvent, e->message.getTimeStamp());
            }
            if (noteOns == 0)
                fail ("UNSUPPORTED_FORMAT", "MIDI has no notes");
            seconds = req.lengthSeconds > 0 ? req.lengthSeconds : lastEvent + req.tailSeconds;
            if (seconds <= 0 || seconds > kMaxSeconds)
                fail ("LIMIT_EXCEEDED", "render length " + String (seconds, 2) + " s outside (0, 600]");
            progress ("read", 0.05, String (noteOns) + " notas, " + String (bpm, 2) + " BPM");
            if (req.sfizz)
            {
                // sfizz_render plays the whole MIDI through the kit; its WAV is the dry signal the effects then get
                auto exe = File (SystemStats::getEnvironmentVariable ("AICORE_SFIZZ_RENDER",
                                                                      "D:\\AICore\\plugins\\sfizz-1.2.3-win64-lib\\bin\\Release\\sfizz_render.exe"));
                if (! exe.existsAsFile())
                    fail ("BROKEN_REFERENCE", "sfizz_render was not found: " + exe.getFullPathName());
                auto staging = File (req.outputDir).getChildFile (".staging").getChildFile ("sfizz-" + req.requestId);
                staging.createDirectory();
                auto wavFile = staging.getChildFile ("dry.wav");
                ChildProcess child;
                StringArray cmd { exe.getFullPathName(), "--sfz", req.sfz, "--midi", req.midiPath, "--wav", wavFile.getFullPathName(),
                                  "--samplerate", String ((int) req.sampleRate), "--blocksize", String (req.blockSize) };
                if (! child.start (cmd, ChildProcess::wantStdErr) || ! child.waitForProcessToFinish (600000) || child.getExitCode() != 0
                    || ! wavFile.existsAsFile())
                {
                    staging.deleteRecursively();
                    fail ("BACKEND_FAILED", "sfizz_render did not render the kit");
                }
                AudioFormatManager afm;
                afm.registerBasicFormats();
                {
                    std::unique_ptr<AudioFormatReader> reader (afm.createReaderFor (wavFile));
                    if (reader == nullptr || reader->lengthInSamples <= 0)
                    {
                        staging.deleteRecursively();
                        fail ("BACKEND_FAILED", "sfizz_render wrote no readable audio");
                    }
                    if ((double) reader->lengthInSamples / req.sampleRate > kMaxSeconds)
                    {
                        staging.deleteRecursively();
                        fail ("LIMIT_EXCEEDED", "sfizz_render wrote more than 600 s");
                    }
                    dry.setSize (2, (int) reader->lengthInSamples);
                    const bool readOk = reader->read (&dry, 0, (int) reader->lengthInSamples, 0, true, true);
                    if (! readOk)
                    {
                        staging.deleteRecursively();
                        fail ("BACKEND_FAILED", "sfizz_render's WAV could not be read");
                    }
                    if (reader->numChannels == 1)
                        dry.copyFrom (1, 0, dry, 0, 0, dry.getNumSamples());
                }
                staging.deleteRecursively();
                if (req.lengthSeconds <= 0)
                    seconds = jmin (kMaxSeconds, jmax (seconds, (double) dry.getNumSamples() / req.sampleRate));
            }
        }

        // ---- plugins (only those in AICore's own scanned list) ----
        progress ("load", 0.06, fromAudio ? String ("a carregar os efeitos") : "a carregar " + File (req.plugin).getFileName());
        AudioPluginFormatManager fm;
        fm.addFormat (std::make_unique<VST3PluginFormatHeadless>());
        OfflinePlayHead head;
        head.bpm = bpm;
        head.sampleRate = req.sampleRate;
        head.tempoMap = tempoMap;

        auto load = [&] (const String& path, const String& stateB64, const String& stateFile, bool wantInstrument,
                         PluginDescription& descOut) -> std::unique_ptr<AudioPluginInstance>
        {
            OwnedArray<PluginDescription> types;
            fm.getFormat (0)->findAllTypesForFile (types, path);
            if (types.isEmpty())
                fail ("BACKEND_FAILED", "no VST3 plugin found in " + path);
            descOut = *types[0];
            for (auto* d : types)
                if (d->isInstrument == wantInstrument)
                {
                    descOut = *d;
                    break;
                }
            String loadError;
            auto inst = fm.createPluginInstance (descOut, req.sampleRate, req.blockSize, loadError);
            if (inst == nullptr)
                fail ("BACKEND_FAILED", descOut.name + " failed to load: " + loadError);
            // The Core's rule (engine/src/plugins/juce_host.cpp): an instrument is a VST3 of category
            // Instrument, or one that takes MIDI and no audio.
            const bool isInstrument = descOut.isInstrument || (inst->acceptsMidi() && inst->getTotalNumInputChannels() == 0);
            if (wantInstrument && ! isInstrument)
                fail ("UNSUPPORTED_OPERATION", descOut.name + " is not an instrument (category: " + descOut.category + ")");
            if (! wantInstrument && isInstrument)
                fail ("UNSUPPORTED_OPERATION", descOut.name + " is an instrument, not an effect");
            MemoryBlock state;
            if (stateB64.isNotEmpty())
            {
                MemoryOutputStream decoded (state, false);
                if (! Base64::convertFromBase64 (decoded, stateB64) || state.isEmpty())
                    fail ("SCHEMA_INVALID", "pluginStateBase64 of " + descOut.name + " is not valid base64");
            }
            else if (stateFile.isNotEmpty() && (! File (stateFile).loadFileAsData (state) || state.isEmpty()))
                fail ("BROKEN_REFERENCE", "pluginState unreadable: " + stateFile);
            if (! state.isEmpty())
                inst->setStateInformation (state.getData(), (int) state.getSize());
            inst->disableNonMainBuses();
            auto layout = inst->getBusesLayout(); // stereo, as the Core does
            if (! layout.outputBuses.isEmpty())
                layout.outputBuses.getReference (0) = AudioChannelSet::stereo();
            if (! layout.inputBuses.isEmpty() && ! wantInstrument)
                layout.inputBuses.getReference (0) = AudioChannelSet::stereo();
            if (inst->checkBusesLayoutSupported (layout))
                inst->setBusesLayout (layout);
            inst->setPlayHead (&head);
            inst->setNonRealtime (true);
            inst->prepareToPlay (req.sampleRate, req.blockSize);
            if (inst->getTotalNumOutputChannels() < 1)
                fail ("BACKEND_FAILED", descOut.name + " has no audio outputs");
            return inst;
        };

        PluginDescription desc;
        desc.name = "audio input";
        std::unique_ptr<AudioPluginInstance> plugin;
        Synthesiser sampler;
        if (req.sampler)
        {
            AudioFormatManager afm;
            afm.registerBasicFormats();
            for (int v = 0; v < 64; ++v)
                sampler.addVoice (new SamplerVoice());
            for (auto& [note, sample] : req.kit)
            {
                std::unique_ptr<AudioFormatReader> rd (afm.createReaderFor (File (sample)));
                if (rd == nullptr)
                    fail ("UNSUPPORTED_FORMAT", "kit sample is not a readable WAV: " + sample);
                BigInteger notes;
                notes.setBit (note);
                // one-shot drum hit: plays its whole length (up to 10 s), 1 ms attack, 20 ms release
                sampler.addSound (new SamplerSound (File (sample).getFileNameWithoutExtension(), *rd, notes, note,
                                                    0.001, 0.02, 10.0));
            }
            sampler.setCurrentPlaybackSampleRate (req.sampleRate);
            desc.name = "AICore sampler";
            desc.version = String ((int) req.kit.size()) + " samples";
        }
        else if (req.sfizz)
        {
            desc.name = "sfizz";
            desc.version = File (req.sfz).getFileName();
        }
        else if (! fromAudio)
            plugin = load (req.plugin, req.pluginStateBase64, req.pluginState, true, desc);
        std::vector<std::unique_ptr<AudioPluginInstance>> fx;
        Array<var> fxInfo;
        for (auto& e : req.effects)
        {
            PluginDescription d;
            fx.push_back (load (e.plugin, e.stateBase64, {}, false, d));
            auto info = obj();
            info->setProperty ("plugin", d.name);
            info->setProperty ("version", d.version);
            info->setProperty ("file", e.plugin);
            fxInfo.add (var (info.get()));
        }
        // Many plugins finish loading (samples, wavetables, licence checks) on the message thread
        // after instantiation; offline we must pump it, or the first seconds render as silence.
        if (plugin != nullptr || ! fx.empty()) // the own sampler needs no warm-up
            MessageManager::getInstance()->runDispatchLoopUntil (warmupMs);
        const int outCh = plugin != nullptr ? plugin->getTotalNumOutputChannels() : 2;
        progress ("load", 0.1, desc.name + " " + desc.version + " + " + String ((int) fx.size()) + " efeito(s)");

        // ---- render ----
        const int64 total = (int64) std::llround (seconds * req.sampleRate);
        // the output cap is checked on the projected size, before ~seconds*rate*8 bytes are allocated
        if (total * 2 * (int64) sizeof (float) + 4096 > kMaxWavBytes)
            fail ("LIMIT_EXCEEDED", "render of " + String (seconds, 1) + " s at " + String ((int) req.sampleRate)
                                    + " Hz would exceed the 128 MiB WAV cap");
        AudioBuffer<float> outBuf (2, (int) total);
        outBuf.clear();
        int bufCh = jmax (2, outCh, plugin != nullptr ? plugin->getTotalNumInputChannels() : 2);
        for (auto& f : fx)
            bufCh = jmax (bufCh, f->getTotalNumInputChannels(), f->getTotalNumOutputChannels());
        AudioBuffer<float> block (bufCh, req.blockSize);
        MidiBuffer midi, noMidi;
        int nextEvent = 0;
        const float gain = Decibels::decibelsToGain ((float) req.gainDb, -100.0f);
        double lastReport = 0.0;
        for (int64 pos = 0; pos < total; pos += req.blockSize)
        {
            const int n = (int) jmin ((int64) req.blockSize, total - pos);
            block.setSize (bufCh, n, false, false, true);
            block.clear();
            midi.clear();
            const double blockEnd = (double) (pos + n) / req.sampleRate;
            while (nextEvent < events.getNumEvents())
            {
                auto& m = events.getEventPointer (nextEvent)->message;
                if (m.getTimeStamp() >= blockEnd)
                    break;
                if (! m.isMetaEvent())
                {
                    const int offset = jlimit (0, n - 1, (int) std::llround (m.getTimeStamp() * req.sampleRate) - (int) pos);
                    midi.addEvent (m, offset);
                }
                ++nextEvent;
            }
            head.sample = pos;
            if (req.sampler)
                sampler.renderNextBlock (block, midi, 0, n);
            else if (plugin != nullptr)
                plugin->processBlock (block, midi);
            else
                for (int c = 0; c < 2; ++c)
                    if (pos < dry.getNumSamples())
                        block.copyFrom (c, 0, dry, c, (int) pos, (int) jmin ((int64) n, (int64) dry.getNumSamples() - pos));
            if (outCh == 1)
                block.copyFrom (1, 0, block, 0, 0, n);
            for (int c = 2; c < bufCh; ++c)
                block.clear (c, 0, n);
            for (auto& f : fx)
            {
                noMidi.clear();
                f->processBlock (block, noMidi);
            }
            for (int c = 0; c < 2; ++c)
                outBuf.copyFrom (c, (int) pos, block, c, 0, n);
            const double frac = (double) (pos + n) / (double) total;
            if (frac - lastReport >= 0.1 || pos + n >= total)
            {
                lastReport = frac;
                progress ("render", 0.1 + 0.8 * frac);
            }
        }
        if (plugin != nullptr)
            plugin->releaseResources();
        for (auto& f : fx)
            f->releaseResources();
        outBuf.applyGain (gain);
        for (int c = 0; c < outBuf.getNumChannels(); ++c)
        {
            const float* s = outBuf.getReadPointer (c);
            for (int i = 0; i < outBuf.getNumSamples(); ++i)
                if (! std::isfinite (s[i]))
                    fail ("BACKEND_FAILED", desc.name + " produced NaN/Inf samples (channel " + String (c) + ")");
        }
        const float peak = outBuf.getMagnitude (0, outBuf.getNumSamples());
        if (peak < 1.0e-6f) // notes went in, nothing came out: never publish that as success
        {
            auto d = obj();
            d->setProperty ("phase", "render");
            d->setProperty ("plugin", desc.name);
            d->setProperty ("notes", noteOns);
            d->setProperty ("warmupMs", warmupMs);
            d->setProperty ("hint", "default patch may be empty: pass the plugin state the Core saved (pluginStateBase64)");
            fail ("BACKEND_FAILED", desc.name + " rendered digital silence for " + String (noteOns) + " notes", var (d.get()));
        }
        const double rms = (outBuf.getRMSLevel (0, 0, outBuf.getNumSamples())
                            + outBuf.getRMSLevel (1, 0, outBuf.getNumSamples())) / 2.0;
        fx.clear();
        plugin.reset();

        // ---- write ----
        progress ("write", 0.92);
        MemoryBlock wavData;
        {
            std::unique_ptr<OutputStream> stream = std::make_unique<MemoryOutputStream> (wavData, false);
            WavAudioFormat wav;
            auto writer = wav.createWriterFor (stream, AudioFormatWriterOptions{}
                                                           .withSampleRate (req.sampleRate)
                                                           .withNumChannels (2)
                                                           .withBitsPerSample (32)
                                                           .withSampleFormat (AudioFormatWriterOptions::SampleFormat::floatingPoint));
            if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (outBuf, 0, outBuf.getNumSamples()))
                fail ("BACKEND_FAILED", "WAV writer failed");
        }
        if ((int64) wavData.getSize() > kMaxWavBytes)
            fail ("LIMIT_EXCEEDED", "render would exceed 128 MiB");
        auto wavFile = publish ("render.wav", wavData);

        auto wavFormat = obj();
        wavFormat->setProperty ("container", "wav");
        wavFormat->setProperty ("encoding", "float32");
        wavFormat->setProperty ("sampleRate", (int) req.sampleRate);
        wavFormat->setProperty ("channels", 2);
        wavFormat->setProperty ("durationSeconds", std::round (seconds * 1e6) / 1e6);
        wavFormat->setProperty ("bytes", (int64) wavData.getSize());

        Array<var> outputs;
        auto addOut = [&outputs] (const String& role, const File& f, const var& format)
        {
            auto o = obj();
            o->setProperty ("role", role);
            o->setProperty ("path", f.getFullPathName());
            o->setProperty ("bytes", f.getSize());
            o->setProperty ("sha256", sha256Of (f));
            o->setProperty ("format", format);
            outputs.add (var (o.get()));
        };
        addOut ("audio", wavFile, var (wavFormat.get()));

        auto metrics = obj();
        metrics->setProperty ("synthetic", req.synthetic);
        metrics->setProperty ("plugin", desc.name);
        metrics->setProperty ("pluginVersion", desc.version);
        metrics->setProperty ("pluginManufacturer", desc.manufacturerName);
        metrics->setProperty ("pluginFile", req.plugin);
        metrics->setProperty ("pluginState", req.pluginState.isNotEmpty() ? var (req.pluginState) : var());
        metrics->setProperty ("pluginStateFromBase64", req.pluginStateBase64.isNotEmpty());
        metrics->setProperty ("effects", fxInfo);
        metrics->setProperty ("pluginList", catalogPath);
        metrics->setProperty ("role", req.role.isNotEmpty() ? var (req.role) : var());
        metrics->setProperty ("effectRoles", req.effectRoleIds);
        metrics->setProperty ("roleList", rolesPath.isNotEmpty() ? var (rolesPath) : var());
        metrics->setProperty ("pluginListSha256", catalogSha);
        metrics->setProperty ("notes", noteOns);
        metrics->setProperty ("bpmUsed", bpm);
        metrics->setProperty ("ppq", kPpq);
        metrics->setProperty ("sampleRate", (int) req.sampleRate);
        metrics->setProperty ("blockSize", req.blockSize);
        metrics->setProperty ("seconds", seconds);
        metrics->setProperty ("peakDbfs", Decibels::gainToDecibels (peak, -200.0f));
        metrics->setProperty ("rmsDbfs", Decibels::gainToDecibels ((float) rms, -200.0f));
        metrics->setProperty ("silent", peak < 1.0e-6f);
        metrics->setProperty ("wallSeconds", (Time::getMillisecondCounterHiRes() - t0) / 1000.0);

        auto report = obj();
        report->setProperty ("schemaVersion", "1.0");
        report->setProperty ("taskId", taskId);
        report->setProperty ("metrics", var (metrics.get()));
        report->setProperty ("params", req.params);
        report->setProperty ("validation", "unreviewed");
        auto reportText = JSON::toString (var (report.get()), JSON::FormatOptions{}.withEncoding (JSON::Encoding::ascii));
        MemoryBlock reportData (reportText.toRawUTF8(), reportText.getNumBytesAsUTF8());
        auto reportFile = publish ("report.json", reportData);
        auto rf = obj();
        rf->setProperty ("container", "json");
        rf->setProperty ("bytes", (int64) reportData.getSize());
        addOut ("report", reportFile, var (rf.get()));

        auto service = obj();
        service->setProperty ("name", kServiceName);
        service->setProperty ("version", String (aicore::buildId()));
        Array<var> ins;
        for (auto& in : *req.inputs.getArray())
        {
            File f (in["path"].toString());
            auto i = obj();
            i->setProperty ("role", in["role"]);
            i->setProperty ("path", f.getFullPathName());
            const auto key = f.getFullPathName();
            i->setProperty ("bytes", inputBytes.contains (key) ? inputBytes[key] : f.getSize());
            i->setProperty ("sha256", inputSha.contains (key) ? inputSha[key] : sha256Of (f));
            ins.add (var (i.get()));
        }
        auto manifest = obj();
        manifest->setProperty ("schemaVersion", "1.0");
        manifest->setProperty ("contract", kContract);
        manifest->setProperty ("taskId", taskId);
        manifest->setProperty ("requestId", req.requestId);
        manifest->setProperty ("profile", "render");
        manifest->setProperty ("operation", "render");
        manifest->setProperty ("createdAt", utcStamp());
        manifest->setProperty ("service", var (service.get()));
        manifest->setProperty ("params", req.params);
        manifest->setProperty ("inputs", ins);
        manifest->setProperty ("outputs", outputs);
        manifest->setProperty ("metrics", var (metrics.get()));
        manifest->setProperty ("validation", "unreviewed");
        manifest->setProperty ("synthetic", req.synthetic);
        auto mText = JSON::toString (var (manifest.get()), JSON::FormatOptions{}.withEncoding (JSON::Encoding::ascii));
        MemoryBlock mData (mText.toRawUTF8(), mText.getNumBytesAsUTF8());
        auto manifestFile = publish ("manifest.json", mData);
        cleanup();
        progress ("write", 1.0, "manifesto publicado");

        auto result = obj();
        result->setProperty ("outputs", outputs);
        result->setProperty ("manifestPath", manifestFile.getFullPathName());
        result->setProperty ("metrics", var (metrics.get()));
        result->setProperty ("validation", "unreviewed");
        result->setProperty ("manifestSha256", SHA256 (mData).toHexString());
        result->setProperty ("manifestBytes", (int64) mData.getSize());
        return var (result.get());
    }

    /** Reads an input once; its sha256 is computed on these bytes, the ones that are then parsed. */
    MemoryBlock readInputOnce (const String& path, int64 cap)
    {
        File f (path);
        if (f.getSize() > cap)
            fail ("LIMIT_EXCEEDED", "input exceeds the profile cap: " + f.getFileName());
        MemoryBlock bytes;
        if (! f.loadFileAsData (bytes))
            fail ("BROKEN_REFERENCE", "input could not be read: " + f.getFullPathName());
        if ((int64) bytes.getSize() > cap)
            fail ("LIMIT_EXCEEDED", "input grew past the cap while it was read: " + f.getFileName());
        const auto sha = SHA256 (bytes).toHexString();
        for (auto& in : *req.inputs.getArray())
            if (File (in["path"].toString()) == f && in.hasProperty ("sha256") && in["sha256"].toString() != sha)
                fail ("HASH_MISMATCH", "the input changed after it was checked: " + f.getFileName());
        inputSha.set (f.getFullPathName(), sha);
        inputBytes.set (f.getFullPathName(), (int64) bytes.getSize());
        return bytes;
    }

    Request req;
    String taskId;
    File outDir, staging;
    Array<File> published;
    HashMap<String, String> inputSha;
    HashMap<String, int64> inputBytes;
    int seq = 0;
    double lastFraction = 0.0;
};

//==============================================================================
// Plugin list (AICore's own): probe / scan / roles. Used by the app's Definicoes page.

int cliError (const String& code, const String& message)
{
    auto e = obj();
    e->setProperty ("code", code);
    e->setProperty ("message", message.substring (0, 500));
    auto d = obj();
    d->setProperty ("error", var (e.get()));
    emit (var (d.get()));
    return 2;
}

/** One plugin file, in this (throw-away) process: what it holds, instantiated once with the Core's
    instrument rule. Run by `scan` as a child so a crashing plugin only kills the child. */
int doProbe (const String& path)
{
    AudioPluginFormatManager fm;
    fm.addFormat (std::make_unique<VST3PluginFormatHeadless>());
    OwnedArray<PluginDescription> types;
    fm.getFormat (0)->findAllTypesForFile (types, path);
    if (types.isEmpty())
        return cliError ("UNSUPPORTED_FORMAT", "no VST3 plugin in " + path);
    for (auto* d : types)
    {
        String err;
        auto inst = fm.createPluginInstance (*d, 44100.0, 512, err);
        if (inst == nullptr)
            return cliError ("BACKEND_FAILED", d->name + " failed to load: " + err);
        const bool instrument = d->isInstrument || (inst->acceptsMidi() && inst->getTotalNumInputChannels() == 0);
        auto o = obj();
        o->setProperty ("name", d->name);
        o->setProperty ("vendor", d->manufacturerName);
        o->setProperty ("version", d->version);
        o->setProperty ("category", d->category);
        o->setProperty ("isInstrument", instrument);
        o->setProperty ("uid", d->createIdentifierString());
        o->setProperty ("file", File (path).getFullPathName());
        o->setProperty ("outputs", inst->getTotalNumOutputChannels());
        emit (var (o.get()));
    }
    return 0;
}

void findVst3 (const File& folder, Array<File>& out, int depth = 0)
{
    if (depth > 4 || ! folder.isDirectory())
        return;
    for (const auto& entry : RangedDirectoryIterator (folder, false, "*", File::findFilesAndDirectories))
    {
        const auto f = entry.getFile();
        if (f.hasFileExtension (".vst3"))
            out.add (f); // a bundle is a folder named *.vst3: not looked into (as the Core does)
        else if (f.isDirectory())
            findVst3 (f, out, depth + 1);
    }
}

/** AICore's fixed list of supported plugins (supported-plugins.json next to the exe, or
    AICORE_SUPPORTED_PLUGINS). The scan only looks for these; the rest of the PC is ignored. */
var supportedList()
{
    auto env = SystemStats::getEnvironmentVariable ("AICORE_SUPPORTED_PLUGINS", {});
    File f = env.isNotEmpty() ? File (env)
                              : File::getSpecialLocation (File::currentExecutableFile).getSiblingFile ("supported-plugins.json");
    auto doc = loadJsonFile (f, "AICore supported-plugins list");
    if (! doc["plugins"].isArray())
        fail ("UNSUPPORTED_FORMAT", "supported-plugins list has no plugins array: " + f.getFullPathName());
    return doc;
}

var supportedEntryFor (const var& supported, const File& file)
{
    for (auto& s : *supported["plugins"].getArray())
        if (s["file"].toString().equalsIgnoreCase (file.getFileName()))
            return s;
    return {};
}

int doScan (const StringArray& extraFolders)
{
    var supported;
    try { supported = supportedList(); }
    catch (const ContractError& e) { return cliError (e.code, e.message); }
    StringArray folders { "C:\\Program Files\\Common Files\\VST3" };
    if (auto* sf = supported["folders"].getArray()) // where AICore keeps its own downloaded plugins
        for (auto& f : *sf)
            folders.addIfNotAlreadyThere (File (f.toString()).getFullPathName());
    var old;
    if (pluginListFile().existsAsFile() && JSON::parse (pluginListFile().loadFileAsString(), old).wasOk())
        if (auto* fs = old["folders"].getArray())
            for (auto& f : *fs)
                folders.addIfNotAlreadyThere (f.toString());
    for (auto& f : extraFolders)
        folders.addIfNotAlreadyThere (File (f).getFullPathName());
    Array<File> found, files;
    for (auto& f : folders)
        findVst3 (File (f), found);
    for (auto& f : found) // only the plugins AICore specifies; the owner's manual ones are ignored
        if (! supportedEntryFor (supported, f).isVoid())
            files.add (f);
    StringArray missing;
    for (auto& s : *supported["plugins"].getArray())
    {
        bool here = false;
        for (auto& f : files)
            here = here || f.getFileName().equalsIgnoreCase (s["file"].toString());
        if (! here)
            missing.add (s["name"].toString());
    }
    const auto self = File::getSpecialLocation (File::currentExecutableFile).getFullPathName();
    Array<var> plugins, quarantine;
    int i = 0;
    for (auto& f : files)
    {
        logLine ("scan " + String (++i) + " / " + String (files.size()) + ": " + f.getFileName());
        ChildProcess child;
        String problem;
        if (! child.start (StringArray { self, "probe", f.getFullPathName() }, ChildProcess::wantStdOut))
            problem = "could not start the probe";
        else
        {
            auto out = child.readAllProcessOutput();
            if (! child.waitForProcessToFinish (60000))
            {
                child.kill();
                problem = "probe did not finish in 60 s";
            }
            else
            {
                Array<var> types;
                for (auto& line : StringArray::fromLines (out))
                {
                    var d;
                    if (! line.trim().startsWithChar ('{') || JSON::parse (line, d).failed())
                        continue;
                    if (d.hasProperty ("error"))
                        problem = d["error"]["message"].toString();
                    else
                    {
                        if (auto* o = d.getDynamicObject())
                            o->setProperty ("roles", supportedEntryFor (supported, f)["roles"]);
                        types.add (d);
                    }
                }
                const auto exitCode = child.getExitCode();
                if (exitCode != 0 && problem.isEmpty())
                    problem = "probe exited " + String ((int) exitCode) + " after " + String (types.size()) + " description(s)";
                if (types.isEmpty() && problem.isEmpty())
                    problem = "probe printed no plugin description";
                if (problem.isEmpty()) // only a probe that finished cleanly puts plugins on the list
                    plugins.addArray (types);
            }
        }
        if (problem.isNotEmpty())
        {
            auto q = obj();
            q->setProperty ("file", f.getFullPathName());
            q->setProperty ("problem", problem);
            quarantine.add (var (q.get()));
        }
    }
    auto doc = obj();
    doc->setProperty ("version", 1);
    doc->setProperty ("scannedAt", utcStamp());
    doc->setProperty ("folders", folders);
    doc->setProperty ("plugins", plugins);
    doc->setProperty ("quarantine", quarantine);
    doc->setProperty ("missing", missing);
    try { writeJsonAtomically (pluginListFile(), var (doc.get())); }
    catch (const ContractError& e) { return cliError (e.code, e.message); }
    int instruments = 0;
    for (auto& p : plugins)
        instruments += (bool) p["isInstrument"] ? 1 : 0;
    auto s = obj();
    s->setProperty ("list", pluginListFile().getFullPathName());
    s->setProperty ("files", files.size());
    s->setProperty ("ignored", found.size() - files.size());
    s->setProperty ("missing", missing);
    s->setProperty ("plugins", plugins.size());
    s->setProperty ("instruments", instruments);
    s->setProperty ("effects", plugins.size() - instruments);
    s->setProperty ("quarantined", quarantine.size());
    emit (var (s.get()));
    return 0;
}

/** roles                      -> prints roles.json (empty map if none)
    roles set <role> <file>    -> the plugin must be in the list, instrument for bass/lead/pad/pluck/drums,
                                  effect for reverb/delay/filter/sidechain/drive
    roles clear <role> */
int doRoles (const StringArray& args)
{
    try
    {
        var doc;
        if (rolesFile().existsAsFile())
            doc = loadJsonFile (rolesFile(), "AICore role list");
        if (! doc.isObject() || ! doc["roles"].isObject())
        {
            auto d = obj();
            d->setProperty ("version", 1);
            d->setProperty ("roles", var (obj().get()));
            doc = var (d.get());
        }
        if (args.isEmpty())
        {
            emit (doc);
            return 0;
        }
        const auto verb = args[0];
        if (! ((verb == "set" && args.size() == 3) || (verb == "clear" && args.size() == 2)))
            return cliError ("SCHEMA_INVALID", "usage: roles | roles set <role> <plugin file> | roles clear <role>");
        const auto role = args[1];
        const bool instrumentRole = instrumentRoles().contains (role);
        if (! instrumentRole && ! effectRoles().contains (role))
            return cliError ("SCHEMA_INVALID", "unknown role '" + role + "'");
        auto* roles = doc["roles"].getDynamicObject();
        if (verb == "clear")
            roles->removeProperty (Identifier (role));
        else if (verb == "set" && args.size() == 3)
        {
            auto entry = findInPluginList (args[2]);
            bool allowed = false;
            if (auto* rs = entry["roles"].getArray())
                for (auto& r : *rs)
                    allowed = allowed || r.toString() == role;
            if (! allowed)
                return cliError ("UNSUPPORTED_OPERATION", entry["name"].toString() + " is not listed for role '" + role + "' in supported-plugins.json");
            if ((bool) entry["isInstrument"] != instrumentRole)
                return cliError ("UNSUPPORTED_OPERATION", entry["name"].toString() + " is " + ((bool) entry["isInstrument"] ? "an instrument" : "an effect")
                                                          + "; role '" + role + "' needs " + (instrumentRole ? "an instrument" : "an effect"));
            auto plugin = obj();
            plugin->setProperty ("id", entry["uid"]);
            plugin->setProperty ("file", entry["file"]);
            plugin->setProperty ("name", entry["name"]);
            auto r = obj();
            r->setProperty ("plugin", var (plugin.get()));
            r->setProperty ("params", var (obj().get()));
            roles->setProperty (Identifier (role), var (r.get()));
        }
        else
            return cliError ("SCHEMA_INVALID", "usage: roles | roles set <role> <file.vst3> | roles clear <role>");
        writeJsonAtomically (rolesFile(), doc);
        emit (doc);
        return 0;
    }
    catch (const ContractError& e)
    {
        return cliError (e.code, e.message);
    }
}

int doHello()
{
    auto service = obj();
    service->setProperty ("name", kServiceName);
    service->setProperty ("version", String (aicore::buildId()));
#if JUCE_WINDOWS
    service->setProperty ("pid", (int) GetCurrentProcessId());
#else
    service->setProperty ("pid", 0);
#endif
    auto profile = obj();
    profile->setProperty ("name", "render");
    profile->setProperty ("operations", Array<var> { "render" });
    auto limits = obj();
    limits->setProperty ("maxInputBytes", kMaxMidiBytes);
    limits->setProperty ("maxOutputBytes", kMaxWavBytes);
    limits->setProperty ("maxDurationSeconds", kMaxSeconds);
    limits->setProperty ("maxResponseBytes", 2 * 1024 * 1024);
    auto d = obj();
    d->setProperty ("schemaVersion", "1.0");
    d->setProperty ("contract", kContract);
    d->setProperty ("service", var (service.get()));
    d->setProperty ("profiles", Array<var> { var (profile.get()) });
    d->setProperty ("limits", var (limits.get()));
    d->setProperty ("ppq", kPpq);
    d->setProperty ("quantizeGrid", Array<var> { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "off" });
    d->setProperty ("authRequired", false);
    d->setProperty ("startedAt", utcStamp());
    emit (var (d.get()));
    return 0;
}

int doJob()
{
    const String taskId = "t-" + Uuid().toString().substring (0, 12);
    // one request line, read with the 2 MiB cap applied while reading (contract §2.5)
    std::string line;
    bool tooLong = false;
    for (int ch; (ch = std::cin.get()) != EOF && ch != '\n';)
    {
        if (line.size() > 2u * 1024u * 1024u)
        {
            tooLong = true;
            break;
        }
        line.push_back ((char) ch);
    }
    if (! line.empty() && line.back() == '\r')
        line.pop_back();
    String rid = "unknown";
    auto errLine = [&] (const ContractError& e)
    {
        auto d = obj();
        d->setProperty ("event", "error");
        d->setProperty ("schemaVersion", "1.0");
        d->setProperty ("contract", kContract);
        d->setProperty ("taskId", taskId);
        d->setProperty ("requestId", rid);
        d->setProperty ("error", errorDoc (e, rid));
        d->setProperty ("at", utcStamp());
        emit (var (d.get()));
        return 2;
    };
    if (String (line).trim().isEmpty())
        return errLine ({ "INVALID_JSON", "no request line on stdin", {} });
    if (tooLong || line.size() > 2u * 1024u * 1024u)
        return errLine ({ "LIMIT_EXCEEDED", "request line exceeds 2 MiB", {} });
    var doc;
    auto parsed = JSON::parse (String::fromUTF8 (line.c_str()), doc);
    if (parsed.failed())
        return errLine ({ "INVALID_JSON", "request line is not JSON: " + parsed.getErrorMessage(), {} });
    if (doc.isObject())
    {
        auto r = doc["requestId"].toString();
        if (r.isNotEmpty() && r.length() <= 128
            && r.containsOnly ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._:-"))
            rid = r;
    }
    Request req;
    try
    {
        req = validate (doc);
    }
    catch (const ContractError& e)
    {
        return errLine (e);
    }

    {
        auto service = obj();
        service->setProperty ("name", kServiceName);
        service->setProperty ("version", String (aicore::buildId()));
        auto d = obj();
        d->setProperty ("event", "start");
        d->setProperty ("schemaVersion", "1.0");
        d->setProperty ("contract", kContract);
        d->setProperty ("taskId", taskId);
        d->setProperty ("requestId", rid);
        d->setProperty ("profile", "render");
        d->setProperty ("operation", "render");
        d->setProperty ("service", var (service.get()));
        d->setProperty ("synthetic", req.synthetic);
        d->setProperty ("at", utcStamp());
        emit (var (d.get()));
    }
    Job job (req, taskId);
    try
    {
        auto result = job.run();
        auto d = obj();
        d->setProperty ("event", "done");
        d->setProperty ("schemaVersion", "1.0");
        d->setProperty ("contract", kContract);
        d->setProperty ("taskId", taskId);
        d->setProperty ("requestId", rid);
        d->setProperty ("result", result);
        d->setProperty ("synthetic", req.synthetic);
        d->setProperty ("at", utcStamp());
        emit (var (d.get()));
        return 0;
    }
    catch (const ContractError& e)
    {
        job.unpublish();
        job.cleanup();
        return errLine (e);
    }
    catch (const std::bad_alloc&)
    {
        job.unpublish();
        job.cleanup();
        return errLine ({ "OUT_OF_MEMORY", "allocation failed (no retry, no offload)", {} });
    }
    catch (const std::exception& e)
    {
        job.unpublish();
        job.cleanup();
        return errLine ({ "BACKEND_FAILED", String ("exception: ") + e.what(), {} });
    }
}
} // namespace

int main (int argc, char* argv[])
{
    protectStdout();
    lowerPriority();
    const String cmd = argc > 1 ? String (argv[1]) : String();
    StringArray rest;
    for (int i = 2; i < argc; ++i)
        rest.add (String::fromUTF8 (argv[i]));
    if (! StringArray { "hello", "job", "scan", "probe", "roles" }.contains (cmd) || (cmd == "probe" && rest.size() != 1))
    {
        logLine ("usage: aicore-render hello | job < request.json | scan [folder...] | probe <file.vst3> | roles [set <role> <file> | clear <role>]");
        return 64;
    }
    MessageManager::getInstance(); // VST3 hosting needs a message thread; this thread is it
    int rc = cmd == "hello" ? doHello()
           : cmd == "job"   ? doJob()
           : cmd == "scan"  ? doScan (rest)
           : cmd == "probe" ? doProbe (rest[0])
                            : doRoles (rest);
    DeletedAtShutdown::deleteAll();
    MessageManager::deleteInstance();
    return rc;
}
