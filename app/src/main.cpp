// AICore-C — app nativa: a copy of the old AICORE (D:\AICore\app, PySide6), screen by screen (owner 2026-11-16:
// "tem de ficar como a versao anterior"). One console-subsystem exe: headless CLI (--version / --self-test /
// --grab <png>) and the JUCE window. Zero browser / zero webview.
//   --area <0|1|2> --section <n>   open EDIT / AI / SETTINGS at a section (for the grab gate)
//   --import <wav> [--import ...]  EDIT > TRACKS: add audio files (each on its own track)
//   --select-first                 choose the first clip (shows the fade squares and the clip fields)
//   --chain master|<n>             open the CHAIN dock on the master or on track n
//   --sample <file> [--sample-select s0 s1] [--sample-fades in out] [--sample-loop] [--sample-master]
//   --grab-delay <ms>              wait before --grab (default 1200)
#include <juce_gui_basics/juce_gui_basics.h>

#include "aicore/selftest.h"
#include "aicore/version.h"
#include "ui/Workspace.h"
#include "ui/AudioOut.h"
#include "ui/Worker.h"
#include "lora/LoraPage.h"
#include "sample/SampleEdit.h"

#include <cmath>

#include <cstdio>
#include <memory>

namespace aicore { void detachOwnConsole(); } // ConsoleDetach.cpp

namespace
{
class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow()
        : DocumentWindow ("AICORE Studio  " + juce::String (aicore::buildId()), aicore::ui::col::background, DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        workspace = new aicore::ui::Workspace();
        setContentOwned (workspace, true);
        setResizable (true, false);
        setResizeLimits (1000, 640, 10000, 10000);
        centreWithSize (1440, 860);
        setVisible (true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    aicore::ui::Workspace* workspace = nullptr;
};

int runSampleTest (const juce::File& wav, const juce::File& out)
{
    using namespace aicore::sample;
    Audio a;
    double rate = 0;
    juce::String error;
    int fails = 0;
    auto check = [&] (bool ok, const juce::String& what) {
        std::printf ("%s: %s\n", ok ? "ok" : "FAIL", what.toRawUTF8());
        fails += ok ? 0 : 1;
    };
    check (load (wav, a, rate, error), "load " + wav.getFileName() + " " + error);
    if (fails)
        return 1;
    const int n = a.getNumSamples(), r = (int) rate;
    check (keep (a, r, 3 * r).getNumSamples() == 2 * r, "keep 1..3 s = 2 s");
    check (cut (a, r, 3 * r).getNumSamples() == n - 2 * r - 128, "cut 1..3 s joins with a 128-frame crossfade");
    auto nm = normalize (a, 0, n);
    const float pk = juce::jmax (nm.getMagnitude (0, 0, n), nm.getMagnitude (1, 0, n));
    check (std::abs (20 * std::log10 (pk) + 1) < 0.01, "normalize peak -1 dB");
    auto fd = applyFades (keep (a, 0, r), r / 2, r / 4, "smooth");
    check (fd.getSample (0, 0) == 0.0f && fd.getSample (0, r - 1) == 0.0f, "fades start and end at 0");
    check (fileName ("", "kick", "loop", 145, "F#m") == "Kick Loop 145 BPM F#m", "file name " + fileName ("", "kick", "loop", 145, "F#m"));
    out.createDirectory();
    for (auto [fmt, bits] : { std::pair<const char*, const char*> { "wav", "24" }, { "wav", "16" }, { "wav", "32f" }, { "flac", "24" }, { "mp3", "" } })
    {
        ExportChoice c;
        c.format = fmt;
        c.bits = bits;
        ExportResult res;
        juce::String err;
        const bool ok = exportAudio (keep (a, 0, 2 * r), rate, out.getChildFile (juce::String ("export-") + fmt + bits), c, res, err);
        check (ok && res.file.existsAsFile() && res.file.getSize() > 1000, juce::String ("export ") + fmt + " " + bits + " " + err);
    }
    {
        ExportChoice c;
        c.rate = 48000;
        c.mono = true;
        ExportResult res;
        juce::String err;
        check (exportAudio (keep (a, 0, r), rate, out.getChildFile ("export-48k-mono"), c, res, err) && res.channels == 1 && res.rate == 48000,
               "export 48 kHz mono " + err);
    }
    SaveResult s1, s2;
    juce::String e1, e2;
    check (saveSample (keep (a, 0, r), rate, out, "", "kick", "one-shot", 0, "", wav.getFullPathName(), s1, e1), "save one-shot " + e1);
    check (saveSample (keep (a, 0, r), rate, out, "", "kick", "one-shot", 0, "", wav.getFullPathName(), s2, e2) && s2.file != s1.file,
           "save again never overwrites: " + s2.file.getFileName());
    check (out.getChildFile ("AICORE edits/edits.jsonl").existsAsFile(), "edits.jsonl");
    return fails == 0 ? 0 : 1;
}

class AICoreApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "AICORE Studio"; }
    const juce::String getApplicationVersion() override { return aicore::buildId(); }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& commandLine) override
    {
        const auto args = juce::StringArray::fromTokens (commandLine, true);
        if (args.contains ("--version"))
        {
            std::printf ("%s\n", aicore::buildId().c_str());
            std::fflush (stdout);
            quitSoon (0);
            return;
        }
        // --sample-test <wav> <out folder>: the SAMPLE edits, export (WAV/FLAC/MP3) and save, headless
        if (const int st = args.indexOf ("--sample-test"); st >= 0 && st + 2 < args.size())
        {
            const int rc = runSampleTest (juce::File (args[st + 1].unquoted()), juce::File (args[st + 2].unquoted()));
            quitSoon (rc);
            return;
        }
        // --worker-run <kind> <fields.json>: one audio-worker job, headless; prints its events (the gate of the pages'
        // jobs: separate, export, ...)
        if (const int wr = args.indexOf ("--worker-run"); wr >= 0 && wr + 2 < args.size())
        {
            worker = std::make_unique<aicore::ui::Worker>();
            worker->onEvent = [] (const juce::var& v) { std::printf ("event: %s\n", juce::JSON::toString (v, true).toRawUTF8()); std::fflush (stdout); };
            worker->onError = [] (const juce::String& m) { std::printf ("error: %s\n", m.toRawUTF8()); std::fflush (stdout); };
            worker->onDone = [this] (const juce::var& r) { workerOk = true; std::printf ("done: %s\n", juce::JSON::toString (r, true).toRawUTF8()); std::fflush (stdout); };
            worker->onFinished = [this] { quitSoon (workerOk ? 0 : 1); };
            const auto err = worker->start (args[wr + 1], juce::JSON::parse (juce::File (args[wr + 2].unquoted())));
            if (err.isNotEmpty())
            {
                std::printf ("fail: %s\n", err.toRawUTF8());
                quitSoon (1);
            }
            return;
        }
        // --lora-test <training folder> <wav>: a collection made, a sample added, labels saved (the old app reads it)
        if (const int lt = args.indexOf ("--lora-test"); lt >= 0 && lt + 2 < args.size())
        {
            aicore::lora::Collections store (juce::File (args[lt + 1].unquoted()));
            juce::String id;
            auto e = store.create ("Test kicks", "samples", 10, "punchy psy kick", id);
            if (e.isEmpty())
                e = store.add (id, { args[lt + 2].unquoted() });
            if (e.isEmpty())
            {
                auto item = store.get (id);
                auto* ch = new juce::DynamicObject();
                auto* one = new juce::DynamicObject();
                one->setProperty ("bpm", 145.0);
                one->setProperty ("language", "en");
                ch->setProperty (juce::Identifier (item["items"][0]["id"].toString()), juce::var (one));
                e = store.update (id, juce::var (ch));
            }
            std::printf ("%s %s\n", e.isEmpty() ? "ok" : "fail", (e.isEmpty() ? store.directory (id).getFullPathName() : e).toRawUTF8());
            quitSoon (e.isEmpty() ? 0 : 1);
            return;
        }
        if (args.contains ("--self-test"))
        {
            const auto r = aicore::runSelfTest();
            std::printf ("%s: %s\n", r.ok ? "ok" : "fail", r.detail.c_str());
            std::fflush (stdout);
            quitSoon (r.ok ? 0 : 1);
            return;
        }

        if (const int g = args.indexOf ("--grab"); g >= 0 && (g + 1 >= args.size() || args[g + 1].startsWith ("--")))
        {
            // audit 2026-11-16: --grab without a path used to open the window and never quit
            std::printf ("fail: --grab needs a .png path\n");
            std::fflush (stdout);
            quitSoon (2);
            return;
        }
        if (! args.contains ("--grab"))
            aicore::detachOwnConsole(); // opened by double-click or a shortcut: only the window, no console
        laf = std::make_unique<aicore::ui::AicoreLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (laf.get());
        window = std::make_unique<MainWindow>();
        auto& ws = *window->workspace;

        juce::StringArray imports;
        for (int i = 0; i + 1 < args.size(); ++i)
            if (args[i] == "--import")
                imports.add (args[i + 1].unquoted());
        const bool selectFirst = args.contains ("--select-first");
        const int ci = args.indexOf ("--chain"); // --chain master | <track number from 1>
        juce::StringArray effects;
        for (int i = 0; i + 1 < args.size(); ++i)
            if (args[i] == "--chain-add")
                effects.add (args[i + 1]);
        const juce::String chainArg = ci >= 0 && ci + 1 < args.size() ? args[ci + 1] : juce::String();
        if (! imports.isEmpty() || chainArg.isNotEmpty())
            juce::Timer::callAfterDelay (100, [this, imports, selectFirst, chainArg, effects] {
                auto& tp = window->workspace->tracksPage;
                if (! imports.isEmpty())
                    tp.addFiles (imports, 0, 0.0);
                auto& s = tp.view.session;
                if (selectFirst && ! s.tracks.empty() && ! s.tracks[0].clips.empty())
                {
                    tp.view.selected = { s.tracks[0].clips[0].id };
                    if (tp.view.onSelectionChanged)
                        tp.view.onSelectionChanged();
                    tp.view.repaint();
                }
                if (chainArg.isNotEmpty())
                {
                    tp.openChain (chainArg == "master" ? -1 : chainArg.getIntValue() - 1);
                    for (auto& key : effects)
                        tp.chainPanel().addEffect (key); // --chain-add comp|limit|eq|hp|lufs
                }
            });
        // EDIT > SAMPLE for the grab: --sample <file> [--sample-select <s0> <s1>] [--sample-fades <in s> <out s>]
        // [--sample-loop] [--sample-master]
        if (const int smp = args.indexOf ("--sample"); smp >= 0 && smp + 1 < args.size())
        {
            auto& page = ws.samplePage;
            page.openFile (args[smp + 1].unquoted());
            const int sel = args.indexOf ("--sample-select"), fad = args.indexOf ("--sample-fades");
            const double s0 = sel >= 0 ? args[sel + 1].getDoubleValue() : -1, s1 = sel >= 0 ? args[sel + 2].getDoubleValue() : -1;
            const double f0 = fad >= 0 ? args[fad + 1].getDoubleValue() : 0, f1 = fad >= 0 ? args[fad + 2].getDoubleValue() : 0;
            const bool loopKind = args.contains ("--sample-loop"), masterOn = args.contains ("--sample-master");
            juce::Timer::callAfterDelay (1500, [&page, s0, s1, f0, f1, loopKind, masterOn] {
                auto* e = page.current();
                if (e == nullptr)
                    return;
                const double r = e->wave.rate();
                if (s1 > s0)
                    e->wave.setSelection ((int) (s0 * r), (int) (s1 * r));
                if (f0 > 0 || f1 > 0)
                    e->wave.setFades (f0, f1);
                if (loopKind)
                    e->setLoopKind();
                if (masterOn)
                    e->showMaster (true);
            });
        }
        // EDIT > STEMS / VOICE for the grab: --stems-import <wav> (repeatable) [--stems-voice] [--stems-mix] [--stems-master]
        {
            juce::StringArray stemFiles;
            for (int i = 0; i + 1 < args.size(); ++i)
                if (args[i] == "--stems-import")
                    stemFiles.add (args[i + 1].unquoted());
            if (! stemFiles.isEmpty())
            {
                auto& sp = args.contains ("--stems-voice") ? ws.voicePage : ws.stemsPage;
                sp.importFiles (stemFiles);
                sp.select (0);
                if (args.contains ("--stems-mix"))
                    sp.showView (1);
                if (args.contains ("--stems-master"))
                    sp.chooseMaster();
            }
        }
        if (const int am = args.indexOf ("--ace-music"); am >= 0 && am + 1 < args.size())
            ws.aiPage.openMusic (args[am + 1].unquoted());
        if (args.contains ("--ace-cover"))
            ws.aiPage.setCover (true);
        if (args.contains ("--ace-scratch"))
            ws.aiPage.setStart (true);
        if (args.contains ("--flamingo"))
            ws.aiPage.showFlamingo (true);
        if (const int mi = args.indexOf ("--midi"); mi >= 0 && mi + 1 < args.size())
            ws.midiPage.openFile (args[mi + 1].unquoted());
        const int ai = args.indexOf ("--area"), si = args.indexOf ("--section");
        if (ai >= 0 && ai + 1 < args.size())
        {
            ws.showArea (args[ai + 1].getIntValue());
            if (si >= 0 && si + 1 < args.size())
                ws.showSection (args[ai + 1].getIntValue(), args[si + 1].getIntValue());
        }

        const int grabIdx = args.indexOf ("--grab");
        if (grabIdx >= 0 && grabIdx + 1 < args.size())
        {
            grabPath = args[grabIdx + 1].unquoted();
            const int di = args.indexOf ("--grab-delay");
            const int delay = di >= 0 && di + 1 < args.size() ? args[di + 1].getIntValue() : 1200;
            juce::Timer::callAfterDelay (juce::jlimit (100, 600000, delay), [this] { captureAndQuit(); });
        }
    }

    std::unique_ptr<aicore::ui::Worker> worker;
    bool workerOk = false;

    void shutdown() override
    {
        worker.reset();
        window.reset();
        aicore::ui::AudioOut::get().close(); // before JUCE goes: the device and its thread
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        laf.reset();
    }
    void systemRequestedQuit() override { quit(); }

private:
    void quitSoon (int code)
    {
        setApplicationReturnValue (code);
        juce::Timer::callAfterDelay (30, [] { juce::JUCEApplication::getInstance()->quit(); });
    }

    void captureAndQuit()
    {
        if (window == nullptr || ! window->isShowing())
        {
            std::printf ("fail: janela nao visivel\n");
            std::fflush (stdout);
            quitSoon (1);
            return;
        }
        auto* content = window->getContentComponent();
        const auto snap = content->createComponentSnapshot (content->getLocalBounds());
        const juce::File out (grabPath);
        out.getParentDirectory().createDirectory();
        out.deleteFile();
        juce::FileOutputStream fos (out);
        juce::PNGImageFormat png;
        const bool wrote = fos.openedOk() && png.writeImageToStream (snap, fos);
        fos.flush();
        std::printf ("%s: grab %s %dx%d | %s\n", wrote ? "ok" : "fail", grabPath.toRawUTF8(), snap.getWidth(), snap.getHeight(),
                     window->workspace->currentName().toRawUTF8());
        std::fflush (stdout);
        quitSoon (wrote ? 0 : 1);
    }

    std::unique_ptr<aicore::ui::AicoreLookAndFeel> laf;
    std::unique_ptr<MainWindow> window;
    juce::String grabPath;
};
} // namespace

START_JUCE_APPLICATION (AICoreApp)
