#include "SampleEdit.h"

#include "../ui/AppSettings.h"

#include <juce_cryptography/juce_cryptography.h>

#include <cmath>

namespace aicore::sample
{
const Role kRoles[19] = { { "kick", "Kick" },   { "bass", "Bass" },     { "kickbass", "Kick bass" }, { "hat", "Hat" },
                          { "openhat", "Open hat" }, { "snare", "Snare" }, { "clap", "Clap" },       { "perc", "Perc" },
                          { "tom", "Tom" },     { "crash", "Crash" },   { "impact", "Impact" },      { "riser", "Riser" },
                          { "fx", "FX" },       { "vox", "Vox" },       { "pad", "Pad" },            { "synth", "Synth" },
                          { "orchestral", "Orchestral" }, { "fill", "Fill" }, { "other", "Other" } };

juce::String roleFolder (const juce::String& key)
{
    for (auto& r : kRoles)
        if (key == r.key)
            return r.folder;
    return {};
}

bool load (const juce::File& file, Audio& out, double& rate, juce::String& error)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (file));
    if (r == nullptr)
    {
        error = "This file could not be opened.";
        return false;
    }
    if (r->lengthInSamples * 2 * 4 > (juce::int64) 512 * 1024 * 1024 || r->lengthInSamples > std::numeric_limits<int>::max())
    {
        error = "This sound is too long to edit; cut or convert a copy first.";
        return false;
    }
    if (r->lengthInSamples / r->sampleRate > 20 * 60)
    {
        error = "Longer than 20 minutes: cut a part first.";
        return false;
    }
    out.setSize (2, (int) r->lengthInSamples);
    r->read (&out, 0, (int) r->lengthInSamples, 0, true, true);
    if (r->numChannels == 1)
        out.copyFrom (1, 0, out, 0, 0, out.getNumSamples());
    rate = r->sampleRate;
    return true;
}

std::pair<int, int> clampRange (const Audio& a, int start, int end)
{
    const int n = a.getNumSamples();
    int x = juce::jmin (start, end), y = juce::jmax (start, end);
    x = juce::jlimit (0, n, x);
    y = juce::jlimit (0, n, y);
    return y - x < 1 ? std::pair<int, int> { 0, n } : std::pair<int, int> { x, y };
}

static float monoAt (const Audio& a, int i) { return 0.5f * (a.getSample (0, i) + a.getSample (1, i)); }

int zeroCross (const Audio& a, int frame, double rate, double windowMs)
{
    const int n = a.getNumSamples();
    if (n < 2)
        return juce::jlimit (0, n, frame);
    const int w = juce::jmax (1, (int) (rate * windowMs / 1000));
    const int lo = juce::jmax (0, frame - w), hi = juce::jmin (n - 1, frame + w);
    if (hi - lo < 1)
        return juce::jlimit (0, n, frame);
    int best = -1, bestDist = std::numeric_limits<int>::max();
    int quiet = lo;
    float quietV = std::abs (monoAt (a, lo));
    for (int i = lo; i < hi; ++i)
    {
        const float v0 = monoAt (a, i), v1 = monoAt (a, i + 1);
        if (std::signbit (v0) != std::signbit (v1))
        {
            const int d = std::abs (i - frame);
            if (d < bestDist)
            {
                bestDist = d;
                best = i + 1;
            }
        }
        if (std::abs (v1) < quietV)
        {
            quietV = std::abs (v1);
            quiet = i + 1;
        }
    }
    return best >= 0 ? best : quiet;
}

Audio range (const Audio& a, int start, int end)
{
    Audio out (2, juce::jmax (0, end - start));
    for (int ch = 0; ch < 2; ++ch)
        if (end > start)
            out.copyFrom (ch, 0, a, ch, start, end - start);
    return out;
}

Audio keep (const Audio& a, int start, int end)
{
    auto [x, y] = clampRange (a, start, end);
    return range (a, x, y);
}

Audio cut (const Audio& a, int start, int end)
{
    auto [x, y] = clampRange (a, start, end);
    const int n = a.getNumSamples();
    if (x == 0 && y == n)
        return Audio (2, 0);
    const int head = x, tail = n - y, xf = juce::jmin (head, tail, 128);
    Audio out (2, head + tail - xf);
    for (int ch = 0; ch < 2; ++ch)
    {
        if (head - xf > 0)
            out.copyFrom (ch, 0, a, ch, 0, head - xf);
        for (int i = 0; i < xf; ++i)
        {
            const float r = xf > 1 ? (float) i / (float) (xf - 1) : 1.0f;
            out.setSample (ch, head - xf + i, a.getSample (ch, head - xf + i) * (1 - r) + a.getSample (ch, y + i) * r);
        }
        if (tail - xf > 0)
            out.copyFrom (ch, head, a, ch, y + xf, tail - xf);
    }
    return out;
}

Audio gainDb (const Audio& a, int start, int end, double db)
{
    auto [x, y] = clampRange (a, start, end);
    Audio out (a);
    for (int ch = 0; ch < 2; ++ch)
        out.applyGain (ch, x, y - x, (float) std::pow (10.0, db / 20));
    return out;
}

Audio normalize (const Audio& a, int start, int end, double peakDb)
{
    auto [x, y] = clampRange (a, start, end);
    float peak = 0;
    for (int ch = 0; ch < 2; ++ch)
        peak = juce::jmax (peak, a.getMagnitude (ch, x, y - x));
    if (peak < 1e-6f)
        return Audio (a);
    return gainDb (a, x, y, peakDb - 20 * std::log10 ((double) peak));
}

Audio reverse (const Audio& a, int start, int end)
{
    auto [x, y] = clampRange (a, start, end);
    Audio out (a);
    out.reverse (x, y - x);
    return out;
}

Audio silence (const Audio& a, int start, int end)
{
    auto [x, y] = clampRange (a, start, end);
    Audio out (a);
    out.clear (x, y - x);
    return out;
}

Audio trimSilence (const Audio& a, double rate, double thresholdDb)
{
    const int n = a.getNumSamples();
    const float th = (float) std::pow (10.0, thresholdDb / 20);
    int first = -1, last = -1;
    for (int i = 0; i < n; ++i)
        if (std::abs (a.getSample (0, i)) > th || std::abs (a.getSample (1, i)) > th)
        {
            if (first < 0)
                first = i;
            last = i;
        }
    if (first < 0)
        return Audio (a);
    return range (a, first, juce::jmin (n, last + (int) (rate * 0.01)));
}

Audio declick (const Audio& a, double rate, double ms)
{
    Audio out (a);
    const int total = out.getNumSamples();
    const int n = juce::jmin (total / 2, juce::jmax (1, (int) (rate * ms / 1000)));
    if (n > 0)
    {
        out.applyGainRamp (0, n, 0.0f, 1.0f);
        out.applyGainRamp (total - n, n, 1.0f, 0.0f);
    }
    return out;
}

Audio insert (const Audio& a, int at, const Audio& clip)
{
    at = juce::jlimit (0, a.getNumSamples(), at);
    Audio out (2, a.getNumSamples() + clip.getNumSamples());
    for (int ch = 0; ch < 2; ++ch)
    {
        if (at > 0)
            out.copyFrom (ch, 0, a, ch, 0, at);
        if (clip.getNumSamples() > 0)
            out.copyFrom (ch, at, clip, juce::jmin (ch, clip.getNumChannels() - 1), 0, clip.getNumSamples());
        if (a.getNumSamples() - at > 0)
            out.copyFrom (ch, at + clip.getNumSamples(), a, ch, at, a.getNumSamples() - at);
    }
    return out;
}

std::vector<float> fadeCurve (int n, const juce::String& curve)
{
    std::vector<float> v ((size_t) juce::jmax (0, n));
    for (int i = 0; i < n; ++i)
    {
        const double x = n > 1 ? (double) i / (n - 1) : 0.0; // numpy linspace(0, 1, n)
        v[(size_t) i] = (float) (curve == "linear" ? x : curve == "exponential" ? x * x * x : 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * x));
    }
    return v;
}

std::pair<int, int> fadeLengths (int frames, int fadeIn, int fadeOut)
{
    const int fi = juce::jlimit (0, frames, fadeIn);
    const int fo = juce::jlimit (0, frames - fi, fadeOut);
    return { fi, fo };
}

Audio applyFades (const Audio& a, int fadeIn, int fadeOut, const juce::String& curve)
{
    Audio out (a);
    const int n = out.getNumSamples();
    auto [fi, fo] = fadeLengths (n, fadeIn, fadeOut);
    if (fi > 0)
    {
        auto c = fadeCurve (fi, curve);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < fi; ++i)
                out.setSample (ch, i, out.getSample (ch, i) * c[(size_t) i]);
    }
    if (fo > 0)
    {
        auto c = fadeCurve (fo, curve);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < fo; ++i)
                out.setSample (ch, n - fo + i, out.getSample (ch, n - fo + i) * c[(size_t) (fo - 1 - i)]);
    }
    return out;
}

int barsFrames (double bpm, double bars, double rate, juce::String& error)
{
    if (bpm < 40 || bpm > 999)
    {
        error = "BPM between 40 and 999.";
        return -1;
    }
    return (int) std::round (bars * 4 * 60.0 / bpm * rate);
}

juce::String cleanName (const juce::String& text)
{
    juce::String out;
    for (auto c : text)
        out << (juce::String ("<>:\"/\\|?*").containsChar (c) || c < 32 ? juce::juce_wchar (' ') : c);
    while (out.contains ("  "))
        out = out.replace ("  ", " ");
    out = out.trim();
    while (out.endsWithChar ('.') || out.endsWithChar (' '))
        out = out.dropLastCharacters (1);
    while (out.startsWithChar ('.') || out.startsWithChar (' '))
        out = out.substring (1);
    return out.substring (0, 80);
}

juce::String fileName (const juce::String& name, const juce::String& role, const juce::String& kind, double bpm, const juce::String& key)
{
    const auto roleWord = roleFolder (role);
    juce::StringArray words;
    words.add (cleanName (name).isNotEmpty() ? cleanName (name) : (roleWord.isNotEmpty() ? roleWord : juce::String ("Sample")));
    const auto low = words[0].toLowerCase();
    if (roleWord.isNotEmpty() && ! low.contains (roleWord.toLowerCase()))
        words.insert (0, roleWord);
    if (kind == "loop")
    {
        if (! low.contains ("loop"))
            words.add ("Loop");
        bool hasBpm = false;
        juce::StringArray toks;
        toks.addTokens (low, " ", "");
        for (int i = 0; i < toks.size(); ++i)
        {
            auto t = toks[i];
            if ((t.endsWith ("bpm") && t.dropLastCharacters (3).trim().containsOnly ("0123456789") && t.length() > 4)
                || (t == "bpm" && i > 0 && toks[i - 1].containsOnly ("0123456789")))
                hasBpm = true;
        }
        if (bpm > 0 && ! hasBpm)
            words.add ((std::abs (bpm - std::round (bpm)) < 1e-9 ? juce::String ((int) std::round (bpm)) : juce::String (bpm)) + " BPM");
    }
    juce::StringArray first;
    first.addTokens (words[0], " ", "");
    if (key.isNotEmpty() && ! first.contains (key))
        words.add (key);
    auto out = cleanName (words.joinIntoString (" "));
    return out.isNotEmpty() ? out : juce::String ("Sample");
}

static Audio resampled (const Audio& a, double rate, int newRate)
{
    if (newRate <= 0 || (int) rate == newRate)
        return Audio (a);
    const double ratio = rate / newRate;
    const int outN = (int) std::ceil (a.getNumSamples() / ratio);
    Audio out (a.getNumChannels(), outN);
    for (int ch = 0; ch < a.getNumChannels(); ++ch)
    {
        juce::WindowedSincInterpolator interp;
        interp.process (ratio, a.getReadPointer (ch), out.getWritePointer (ch), outN, a.getNumSamples(), 0);
    }
    return out;
}

bool exportAudio (const Audio& in, double rate, const juce::File& path, const ExportChoice& choice, ExportResult& res, juce::String& error)
{
    const auto fmt = choice.format;
    if (fmt != "wav" && fmt != "flac" && fmt != "mp3")
    {
        error = "Unknown format: " + fmt;
        return false;
    }
    if (in.getNumSamples() == 0)
    {
        error = "There is no audio: nothing was saved.";
        return false;
    }
    Audio audio (in);
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        for (auto* p = audio.getWritePointer (ch), *e = p + audio.getNumSamples(); p < e; ++p)
            if (! std::isfinite (*p))
                *p = 0;
    if (choice.mono)
    {
        Audio m (1, audio.getNumSamples());
        m.copyFrom (0, 0, audio.getReadPointer (0), audio.getNumSamples(), 0.5f);
        m.addFrom (0, 0, audio, 1, 0, audio.getNumSamples(), 0.5f);
        audio = std::move (m);
    }
    const int outRate = choice.rate > 0 ? choice.rate : (int) rate;
    audio = resampled (audio, rate, outRate);
    juce::String bits = fmt == "mp3" ? juce::String() : choice.bits;
    if (fmt == "wav" && ! juce::StringArray { "16", "24", "32f" }.contains (bits))
        bits = "24";
    if (fmt == "flac" && ! juce::StringArray { "16", "24" }.contains (bits))
        bits = "24";
    if (bits != "32f")
    {
        float peak = 0;
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            peak = juce::jmax (peak, audio.getMagnitude (ch, 0, audio.getNumSamples()));
        if (peak > 0.999f)
            audio.applyGain (0.999f / peak);
    }
    if (bits == "16")
    {
        juce::Random rng (0);
        const float lsb = 1.0f / 32768.0f;
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            for (int i = 0; i < audio.getNumSamples(); ++i)
                audio.setSample (ch, i, juce::jlimit (-1.0f, 32767.0f / 32768.0f,
                                                      audio.getSample (ch, i) + (rng.nextFloat() - rng.nextFloat()) * lsb));
    }
    const auto ext = "." + fmt;
    auto target = path.getFileExtension().equalsIgnoreCase (ext) ? path : path.getSiblingFile (path.getFileName() + ext);
    target.getParentDirectory().createDirectory();

    auto writeWith = [&] (juce::AudioFormat& format, const juce::File& file, int bitDepth) {
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto opts = juce::AudioFormatWriterOptions {}.withSampleRate (outRate).withNumChannels (audio.getNumChannels()).withBitsPerSample (bitDepth);
        if (bitDepth == 32)
            opts = opts.withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        auto w = format.createWriterFor (stream, opts);
        return w != nullptr && w->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    };
    bool ok = false;
    if (fmt == "wav")
    {
        juce::WavAudioFormat wav;
        ok = writeWith (wav, target, bits == "16" ? 16 : bits == "24" ? 24 : 32);
    }
    else if (fmt == "flac")
    {
        juce::FlacAudioFormat flac;
        ok = writeWith (flac, target, bits == "16" ? 16 : 24);
    }
    else
    {
        // MP3: JUCE has no encoder; AICore's own Python (soundfile / libsndfile) writes it from a float WAV
        auto tmp = target.getSiblingFile ("." + target.getFileNameWithoutExtension() + ".aicore-mp3.wav");
        juce::WavAudioFormat wav;
        if (writeWith (wav, tmp, 32))
        {
            auto py = ui::AppSettings::home().getChildFile ("python\\Scripts\\python.exe");
            juce::String script = "import sys,soundfile as sf\n"
                                  "d,r=sf.read(sys.argv[1],dtype='float32')\n"
                                  "try:\n sf.write(sys.argv[2],d,r,format='MP3',subtype='MPEG_LAYER_III',compression_level=0.0,bitrate_mode='CONSTANT')\n"
                                  "except TypeError:\n sf.write(sys.argv[2],d,r,format='MP3',subtype='MPEG_LAYER_III')\n";
            juce::ChildProcess child;
            if (py.existsAsFile() && child.start (juce::StringArray { py.getFullPathName(), "-I", "-c", script, tmp.getFullPathName(), target.getFullPathName() }))
            {
                child.waitForProcessToFinish (120000);
                ok = child.getExitCode() == 0 && target.existsAsFile();
                if (! ok)
                    error = "MP3: " + child.readAllProcessOutput().trim();
            }
            else
                error = "The MP3 encoder (AICore's Python) was not found. Choose WAV or FLAC.";
        }
        tmp.deleteFile();
    }
    if (! ok)
    {
        if (error.isEmpty())
            error = "Could not write: " + target.getFullPathName();
        return false;
    }
    res = { target, audio.getNumSamples() / (double) outRate, outRate, audio.getNumChannels(), fmt, bits };
    return true;
}

bool saveSample (const Audio& in, double rate, const juce::File& root, const juce::String& name, const juce::String& role,
                 const juce::String& kind, double bpm, const juce::String& key, const juce::String& source, SaveResult& res,
                 juce::String& error)
{
    if (roleFolder (role).isEmpty())
        return (error = "Choose what the sample is (kick, bass, ...)."), false;
    if (kind != "one-shot" && kind != "loop")
        return (error = "A sample is a one-shot or a loop."), false;
    if (in.getNumSamples() < rate * 0.005)
        return (error = "The selection is too short to be a sample."), false;
    if (root == juce::File() || ! root.isDirectory())
        return (error = "The samples folder was not found. Choose it in Settings > Folders > Samples folder."), false;
    auto audio = declick (in, rate);
    float peak = 0;
    for (int ch = 0; ch < 2; ++ch)
        peak = juce::jmax (peak, audio.getMagnitude (ch, 0, audio.getNumSamples()));
    if (peak > 0.999f)
        audio.applyGain (0.999f / peak);
    bpm = kind == "loop" ? bpm : 0.0;
    auto folder = root.getChildFile (kEditFolder).getChildFile (roleFolder (role)).getChildFile (kind == "loop" ? "Loops" : "One-shots");
    folder.createDirectory();
    const auto base = fileName (name, role, kind, bpm, key);
    auto target = folder.getChildFile (base + ".wav");
    for (int n = 2; target.exists(); ++n)
        target = folder.getChildFile (base + " " + juce::String (n) + ".wav");
    auto tmp = target.getSiblingFile ("." + target.getFileName() + ".tmp.wav");
    {
        tmp.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (tmp);
        juce::WavAudioFormat wav;
        auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}.withSampleRate (rate).withNumChannels (2).withBitsPerSample (24));
        if (w == nullptr || ! w->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples()))
            return (error = "Could not write: " + target.getFullPathName()), false;
    }
    if (! tmp.moveFileTo (target))
        return (error = "Could not write: " + target.getFullPathName()), false;
    juce::FileInputStream in2 (target);
    res = { target, juce::SHA256 (in2).toHexString(), audio.getNumSamples() / rate };

    auto* fields = new juce::DynamicObject();
    fields->setProperty ("role", role);
    fields->setProperty ("kind", kind);
    fields->setProperty ("bpm", bpm > 0 ? (int) std::round (bpm) : 0);
    fields->setProperty ("tags", juce::Array<juce::var> { "aicore-edit" });
    if (key.isNotEmpty())
        fields->setProperty ("key", key);
    if (cleanName (name).isNotEmpty())
        fields->setProperty ("name", cleanName (name));
    auto* rec = new juce::DynamicObject();
    rec->setProperty ("file", target.getFullPathName());
    rec->setProperty ("sha256", res.sha256);
    rec->setProperty ("seconds", res.seconds);
    rec->setProperty ("fields", juce::var (fields));
    rec->setProperty ("source", source);
    rec->setProperty ("at", (juce::int64) (juce::Time::currentTimeMillis() / 1000));
    root.getChildFile (kEditFolder).getChildFile ("edits.jsonl").appendText (juce::JSON::toString (juce::var (rec), true) + "\n", false, false, "\n");
    return true;
}
} // namespace aicore::sample
