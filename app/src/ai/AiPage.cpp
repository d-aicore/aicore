#include "AiPage.h"
#include "../ui/I18n.h"

#include "../ui/AudioOut.h"

namespace aicore::ai
{
using namespace aicore::ui;

namespace
{
juce::String clock2 (double s)
{
    s = juce::jmax (0.0, s);
    const int m = (int) (s / 60);
    return juce::String (m) + ":" + juce::String (s - m * 60, 2).paddedLeft ('0', 5);
}
} // namespace

//==============================================================================
FilePlayer::~FilePlayer()
{
    transport.stop();
    AudioOut::get().detach (&transport);
    transport.setSource (nullptr);
}

bool FilePlayer::load (const juce::File& f)
{
    if (file == f && reader != nullptr)
        return true;
    stop();
    transport.setSource (nullptr);
    reader.reset();
    if (formats.getNumKnownFormats() == 0)
        formats.registerBasicFormats();
    auto* r = formats.createReaderFor (f);
    if (r == nullptr)
        return false;
    const double rate = r->sampleRate;
    reader = std::make_unique<juce::AudioFormatReaderSource> (r, true);
    transport.setSource (reader.get(), 0, nullptr, rate);
    file = f;
    return true;
}

void FilePlayer::play (double from)
{
    AudioOut::get().attach (&transport);
    transport.setPosition (from);
    transport.start();
}

void FilePlayer::stop() { transport.stop(); }

//==============================================================================
PartWave::PartWave() { setMouseCursor (juce::MouseCursor::IBeamCursor); }

bool PartWave::load (const juce::File& f, juce::String& error)
{
    sample::Audio a;
    double rate = 0;
    if (! sample::load (f, a, rate, error))
        return false;
    const int n = 1600, len = a.getNumSamples(), step = juce::jmax (1, len / n), count = len / step;
    std::vector<std::pair<float, float>> raw ((size_t) count);
    float top = 0;
    for (int c = 0; c < count; ++c)
    {
        float pk = 0;
        double sq = 0;
        for (int i = c * step; i < (c + 1) * step; ++i)
        {
            const float l = a.getSample (0, i), r = a.getSample (1, i);
            pk = juce::jmax (pk, std::abs (l), std::abs (r));
            sq += 0.5 * ((double) l * l + (double) r * r);
        }
        raw[(size_t) c] = { pk, juce::jmin (pk, (float) std::sqrt (sq / step) * 1.41f) };
        top = juce::jmax (top, pk);
    }
    peaks.clear();
    if (top > 0)
        for (auto& [p, r] : raw)
            peaks.push_back ({ p / top, r / top });
    seconds = len / rate;
    setPart (seconds / 3, juce::jmin (seconds, 2 * seconds / 3));
    return true;
}

void PartWave::setPart (double a, double b)
{
    start = juce::jlimit (0.0, seconds, juce::jmin (a, b));
    end = juce::jlimit (0.0, seconds, juce::jmax (a, b));
    repaint();
}

void PartWave::mouseDown (const juce::MouseEvent& e)
{
    if (seconds > 0 && ! whole)
    {
        anchor = toSeconds ((float) e.x);
        setPart (anchor, anchor);
    }
}

void PartWave::mouseDrag (const juce::MouseEvent& e)
{
    if (anchor >= 0)
        setPart (anchor, toSeconds ((float) e.x));
}

void PartWave::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (col::field);
    g.fillRoundedRectangle (r, 2.0f);
    g.setColour (col::outline);
    g.drawRoundedRectangle (r, 2.0f, 1.0f);
    if (peaks.empty())
    {
        g.setColour (col::dim);
        g.setFont (uiFont (15.0f));
        g.drawText (aicore::ui::tr ("Import music to choose the part to change"), r, juce::Justification::centred);
        return;
    }
    auto clip = r.reduced (3);
    const juce::Colour base (0xff8a9bb0);
    juce::ColourGradient grad (base, 0, clip.getY(), base, 0, clip.getBottom(), false);
    grad.addColour (0.5, base.brighter (0.18f));
    g.setGradientFill (grad);
    g.fillRoundedRectangle (clip, 2.0f);
    g.setColour (base.darker (0.5f));
    g.drawRoundedRectangle (clip, 2.0f, 1.2f);
    const float mid = clip.getCentreY(), half = clip.getHeight() / 2 - 3;
    const int width = juce::jmax (1, (int) clip.getWidth()), n = (int) peaks.size();
    for (int i = 0; i < width; ++i)
    {
        const int a = (int) ((double) i / width * n), b = juce::jlimit (a + 1, n, (int) ((double) (i + 1) / width * n));
        float hp = 0, hb = 0;
        for (int k = a; k < b; ++k)
        {
            hp = juce::jmax (hp, peaks[(size_t) k].first);
            hb = juce::jmax (hb, peaks[(size_t) k].second);
        }
        const float x = clip.getX() + i;
        g.setColour (juce::Colour::fromRGBA (255, 255, 255, 107));
        g.fillRect (x, mid - hp * half, 1.0f, 2 * hp * half);
        g.setColour (juce::Colour (0xfff4f7fa));
        g.fillRect (x, mid - hb * half, 1.0f, 2 * hb * half);
    }
    const double a = whole ? 0.0 : start, b = whole ? seconds : end;
    if (b > a)
    {
        const float x0 = clip.getX() + (float) (a / seconds) * clip.getWidth(), x1 = clip.getX() + (float) (b / seconds) * clip.getWidth();
        g.setColour (col::background.withAlpha (150 / 255.0f));
        g.fillRect (clip.getX(), clip.getY(), x0 - clip.getX(), clip.getHeight());
        g.fillRect (x1, clip.getY(), clip.getRight() - x1, clip.getHeight());
        g.setColour (col::muted);
        g.drawLine (x0, clip.getY(), x0, clip.getBottom(), 1.5f);
        g.drawLine (x1, clip.getY(), x1, clip.getBottom(), 1.5f);
        const auto text = clock2 (a) + juce::String::fromUTF8 (" \xe2\x80\x93 ") + clock2 (b);
        g.setFont (uiFont (14.0f));
        juce::Rectangle<float> box (x0 + 4, clip.getY() + 4, juce::GlyphArrangement::getStringWidth (uiFont (14.0f), text) + 14, 20);
        g.setColour (col::field);
        g.fillRoundedRectangle (box, 2.0f);
        g.setColour (col::outline);
        g.drawRoundedRectangle (box, 2.0f, 1.0f);
        g.setColour (col::text);
        g.drawText (text, box, juce::Justification::centred);
    }
}

//==============================================================================
AiPage::AiPage()
{
    setWantsKeyboardFocus (true); // the page keeps the focus: the fields show their hints, Space plays
    aceButton.onClick = [this] { showFlamingo (false); };
    flamingoButton.onClick = [this] { showFlamingo (true); };
    addAndMakeVisible (aceButton);
    addAndMakeVisible (flamingoButton);

    fromMusic.onClick = [this] { setStart (false); };
    fromScratch.onClick = [this] { setStart (true); };
    music.setText ("No music chosen", juce::dontSendNotification);
    music.setFont (uiFont (15.0f, true));
    music.setColour (juce::Label::textColourId, col::text);
    musicButton.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser> ("Import music", juce::File(), "*.wav;*.flac;*.mp3;*.ogg;*.aiff;*.aif");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc) {
            if (fc.getResult() != juce::File())
                openMusic (fc.getResult().getFullPathName());
        });
    };
    repaintButton.onClick = [this] { setCover (false); };
    coverButton.onClick = [this] { setCover (true); };
    listenPart.onClick = [this] { playPart(); };
    for (auto [l, t] : { std::pair<juce::Label*, const char*> { &promptLabel, "Prompt" }, { &bpmLabel, "BPM" }, { &secondsLabel, "Length" },
                         { &strengthLabel, "Strength" }, { &noResults, "The takes show here." }, { &state, "Ready." } })
    {
        l->setText (t, juce::dontSendNotification);
        l->setFont (uiFont (15.0f));
        l->setColour (juce::Label::textColourId, col::text);
        l->setJustificationType (juce::Justification::topLeft);
    }
    prompt.setMultiLine (true, true);
    prompt.setReturnKeyStartsNewLine (true);
    prompt.setFont (uiFont (15.0f));
    prompt.setIndents (14, 14);
    prompt.setTextToShowWhenEmpty ("What ACE should make, e.g. hi-tech psy, fast staccato leads, rolling bass", col::dim);
    strength.setTooltip ("How much ACE may change the music (Cover)");
    generateButton.onClick = [this] { generate(); };
    stopButton.onClick = [this] { worker.stop(); };
    for (juce::Component* c : { (juce::Component*) &fromMusic, (juce::Component*) &fromScratch, (juce::Component*) &music, (juce::Component*) &musicButton,
                                (juce::Component*) &wave, (juce::Component*) &repaintButton, (juce::Component*) &coverButton, (juce::Component*) &listenPart,
                                (juce::Component*) &promptLabel, (juce::Component*) &bpmLabel, (juce::Component*) &secondsLabel, (juce::Component*) &strengthLabel,
                                (juce::Component*) &noResults, (juce::Component*) &state, (juce::Component*) &prompt, (juce::Component*) &bpm,
                                (juce::Component*) &length, (juce::Component*) &strength, (juce::Component*) &generateButton })
        addAndMakeVisible (c);
    addChildComponent (stopButton);
    worker.onError = [this] (const juce::String& m) { fail (m); };
    worker.onEvent = [this] (const juce::var& v0) {
        auto v = v0["event"].toString() == "provider" ? v0["value"] : v0;
        if (v["event"].toString() == "progress")
        {
            auto text = v["stage"].toString().isNotEmpty() ? v["stage"].toString() : juce::String ("Running");
            if (v["fraction"].isDouble() && (double) v["fraction"] > 0)
                text += juce::String::fromUTF8 (" \xc2\xb7 ") + juce::String (juce::roundToInt ((double) v["fraction"] * 100)) + "%";
            state.setText (text, juce::dontSendNotification);
        }
    };
    worker.onDone = [this] (const juce::var& r) { done (r); };
    worker.onFinished = [this] { stopButton.setVisible (false); resized(); };

    // FLAMINGO's screen
    flamingoState.setText ("Ready. Choose the intro excerpt to start.", juce::dontSendNotification);
    chooseAudio.setText (juce::String::fromUTF8 ("1 \xc2\xb7 Choose audio"), juce::dontSendNotification);
    referenceLabel.setText ("Reference A", juce::dontSendNotification);
    for (auto* l : { &flamingoState, &chooseAudio, &referenceLabel })
    {
        l->setFont (uiFont (15.0f));
        l->setColour (juce::Label::textColourId, col::text);
    }
    flamingoState.setColour (juce::Label::textColourId, col::muted);
    reference.setFont (uiFont (15.0f));
    reference.setIndents (8, 5);
    reference.setTextToShowWhenEmpty ("MP3, WAV or FLAC", col::dim);
    goal.addItemList ({ "Prepare template / ACE caption", "Make an intro of this kind", "Understand synths and effects", "Analyse KBBB and elastic bass" }, 1);
    goal.setSelectedItemIndex (0, juce::dontSendNotification);
    goal.setColour (juce::ComboBox::backgroundColourId, col::field);
    goal.setColour (juce::ComboBox::textColourId, col::text);
    goal.setColour (juce::ComboBox::outlineColourId, col::outline);
    chooseReference.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser> ("Choose reference", juce::File(), "*.mp3;*.wav;*.flac");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc) {
            if (fc.getResult() != juce::File())
                reference.setText (fc.getResult().getFullPathName());
        });
    };
    analyse.onClick = [this] { flamingoState.setText ("Flamingo is paused for now: the listener comes back later.", juce::dontSendNotification); };
    for (juce::Component* c : { (juce::Component*) &flamingoDots, (juce::Component*) &flamingoState, (juce::Component*) &chooseAudio,
                                (juce::Component*) &referenceLabel, (juce::Component*) &reference, (juce::Component*) &chooseReference,
                                (juce::Component*) &analyse, (juce::Component*) &goal })
        addChildComponent (c);
    showFlamingo (false);
    setStart (false);
}

AiPage::~AiPage() { stopTimer(); }

void AiPage::showFlamingo (bool on)
{
    flamingo = on;
    aceButton.setToggleState (! on, juce::dontSendNotification);
    flamingoButton.setToggleState (on, juce::dontSendNotification);
    for (juce::Component* c : { (juce::Component*) &flamingoDots, (juce::Component*) &flamingoState, (juce::Component*) &chooseAudio,
                                (juce::Component*) &referenceLabel, (juce::Component*) &reference, (juce::Component*) &chooseReference,
                                (juce::Component*) &analyse, (juce::Component*) &goal })
        c->setVisible (on);
    for (juce::Component* c : { (juce::Component*) &fromMusic, (juce::Component*) &fromScratch, (juce::Component*) &promptLabel, (juce::Component*) &bpmLabel,
                                (juce::Component*) &state, (juce::Component*) &prompt, (juce::Component*) &bpm, (juce::Component*) &generateButton,
                                (juce::Component*) &noResults })
        c->setVisible (! on);
    if (! on)
        setStart (scratch);
    else
    {
        for (juce::Component* c : { (juce::Component*) &music, (juce::Component*) &musicButton, (juce::Component*) &wave, (juce::Component*) &repaintButton,
                                    (juce::Component*) &coverButton, (juce::Component*) &listenPart, (juce::Component*) &length, (juce::Component*) &secondsLabel,
                                    (juce::Component*) &strength, (juce::Component*) &strengthLabel })
            c->setVisible (false);
        for (auto& t : takes)
            for (auto* b : { t->play.get(), t->openEdit.get(), t->save.get() })
                b->setVisible (false);
        stop();
    }
    resized();
    repaint();
}

void AiPage::setStart (bool s)
{
    scratch = s;
    fromMusic.setToggleState (! s, juce::dontSendNotification);
    fromScratch.setToggleState (s, juce::dontSendNotification);
    for (juce::Component* c : { (juce::Component*) &music, (juce::Component*) &musicButton, (juce::Component*) &wave, (juce::Component*) &repaintButton,
                                (juce::Component*) &coverButton, (juce::Component*) &listenPart })
        c->setVisible (! s && ! flamingo);
    length.setVisible (s && ! flamingo);
    secondsLabel.setVisible (s && ! flamingo);
    noResults.setVisible (takes.empty() && ! flamingo);
    for (auto& t : takes)
        for (auto* b : { t->play.get(), t->openEdit.get(), t->save.get() })
            b->setVisible (! flamingo);
    setCover (cover);
}

void AiPage::setCover (bool c)
{
    cover = c;
    repaintButton.setToggleState (! c, juce::dontSendNotification);
    coverButton.setToggleState (c, juce::dontSendNotification);
    const bool showStrength = c && ! scratch && ! flamingo;
    strength.setVisible (showStrength);
    strengthLabel.setVisible (showStrength);
    wave.whole = c;
    wave.repaint();
    resized();
    repaint();
}

void AiPage::openMusic (const juce::String& path)
{
    juce::String error;
    if (! wave.load (juce::File (path), error))
    {
        fail (error);
        return;
    }
    source = path;
    music.setText (juce::File (path).getFileName(), juce::dontSendNotification);
    musicButton.setButtonText ("Change");
    setStart (false);
}

void AiPage::generate()
{
    const juce::String mode = scratch ? "text2music" : cover ? "cover" : "repaint";
    const auto text = prompt.getText().trim();
    if (text.isEmpty())
    {
        fail ("Write what ACE should make.");
        return;
    }
    auto* o = new juce::DynamicObject();
    o->setProperty ("mode", mode);
    o->setProperty ("prompt", text);
    o->setProperty ("instrumental", true);
    if (bpm.value() > 0)
        o->setProperty ("bpm", (int) bpm.value());
    if (mode == "text2music")
        o->setProperty ("seconds", length.value());
    else
    {
        if (source.isEmpty())
        {
            fail ("Import the music first.");
            return;
        }
        o->setProperty ("source", source);
        if (mode == "repaint")
        {
            if (wave.end - wave.start < 0.5)
            {
                fail ("Drag on the wave to choose the part to change.");
                return;
            }
            o->setProperty ("start", wave.start);
            o->setProperty ("end", wave.end);
        }
        else
            o->setProperty ("strength", strength.value());
    }
    o->setProperty ("seed", 42 + (int) takes.size());
    const auto err = worker.start ("ace-generate", juce::var (o));
    if (err.isNotEmpty())
    {
        fail (err);
        return;
    }
    state.setText (juce::String::fromUTF8 ("Running\xe2\x80\xa6"), juce::dontSendNotification);
    stopButton.setVisible (true);
    resized();
}

void AiPage::done (const juce::var& result)
{
    if (! result["candidate"].isString())
        return;
    auto t = std::make_unique<Take>();
    t->result = result;
    const int i = (int) takes.size();
    t->play = std::make_unique<AButton> (juce::String::fromUTF8 ("\xe2\x96\xb6"));
    t->openEdit = std::make_unique<AButton> ("Open in EDIT");
    t->save = std::make_unique<AButton> (juce::String::fromUTF8 ("Save\xe2\x80\xa6"));
    t->play->onClick = [this, i] { playTake (i); };
    t->openEdit->onClick = [this, i] {
        if (onOpenInTracks)
            onOpenInTracks ({ takes[(size_t) i]->result["candidate"].toString() });
    };
    t->save->onClick = [this, i] {
        chooser = std::make_unique<juce::FileChooser> ("Save take", juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("ACE take " + juce::String (i + 1) + ".wav"), "*.wav");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this, i] (const juce::FileChooser& fc) {
            auto f = fc.getResult();
            if (f == juce::File())
                return;
            if (juce::File (takes[(size_t) i]->result["candidate"].toString()).copyFileTo (f))
                state.setText ("Saved: " + f.getFullPathName(), juce::dontSendNotification);
            else
                fail ("Could not save: " + f.getFullPathName());
        });
    };
    for (auto* b : { t->play.get(), t->openEdit.get(), t->save.get() })
        addAndMakeVisible (b);
    takes.push_back (std::move (t));
    noResults.setVisible (false);
    state.setText ("Take ready.", juce::dontSendNotification);
    resized();
    repaint();
}

void AiPage::playTake (int i)
{
    if (playingTake == i && player.playing())
    {
        player.stop();
        playingTake = -1;
        return;
    }
    if (player.load (juce::File (takes[(size_t) i]->result["candidate"].toString())))
    {
        player.play();
        playingTake = i;
        startTimer (100);
    }
}

void AiPage::playPart()
{
    if (source.isEmpty())
        return;
    if (player.load (juce::File (source)))
    {
        player.play (wave.whole ? 0.0 : wave.start);
        playingTake = -2;
        startTimer (100);
    }
}

void AiPage::togglePlay()
{
    if (flamingo)
        return;
    if (player.playing())
        stop();
    else if (! takes.empty())
        playTake ((int) takes.size() - 1);
    else
        playPart();
    if (onTransportChanged) onTransportChanged();
}

void AiPage::stop()
{
    player.stop();
    playingTake = -1;
    if (onTransportChanged) onTransportChanged();
}

void AiPage::goStart()
{
    if (player.playing())
        player.play (0);
}

juce::String AiPage::clock() const
{
    if (player.file == juce::File())
        return "0:00.000 / 0:00.000";
    return clock2 (player.seconds()) + "0 / " + clock2 (player.length()) + "0";
}

void AiPage::timerCallback()
{
    if (! player.playing())
    {
        stopTimer();
        if (onTransportChanged) onTransportChanged();
        return;
    }
    if (playingTake == -2 && ! wave.whole && player.seconds() >= wave.end)
        stop();
    if (onTransportChanged) onTransportChanged();
}

void AiPage::visibilityChanged()
{
    if (! isVisible())
        stop();
}

void AiPage::paint (juce::Graphics& g)
{
    if (flamingo)
    {
        drawPanel (g, askBox);
        drawPanel (g, setupBox);
        g.setColour (col::title);
        g.setFont (uiFont (15.0f, true).withExtraKerningFactor (0.1f));
        g.drawText (aicore::ui::tr ("ASK FLAMINGO"), askBox.getX() + 20, askBox.getY() + 12, 300, 26, juce::Justification::centredLeft);
        auto bar = juce::Rectangle<int> (askBox.getX() + 16, askBox.getY() + 50, askBox.getWidth() - 32, 6);
        g.setColour (col::grid);
        g.fillRect (bar);
        auto st = flamingoState.getBounds().expanded (0, 0).toFloat();
        g.setColour (col::card);
        g.fillRoundedRectangle (st.withLeft (st.getX() - 16), 2.0f);
        g.setColour (col::outline);
        g.drawRoundedRectangle (st.withLeft (st.getX() - 16), 2.0f, 1.0f);
        drawHeading (g, "Analysis setup", setupBox.getX() + 16, setupBox.getY() + 14, setupBox.getWidth() - 32);
        g.setColour (col::outline);
        g.drawRoundedRectangle (refBox.toFloat(), 2.0f, 1.0f);
        g.setColour (col::grid);
        g.fillRect (setupBox.getX() + 16, refBox.getBottom() + 10, setupBox.getWidth() - 32, 1);
        const int gy = refBox.getBottom() + 20;
        g.setColour (col::title);
        g.setFont (uiFont (15.0f, true).withExtraKerningFactor (0.1f));
        g.drawText ("2 " + juce::String::fromUTF8 ("\xc2\xb7 ") + aicore::ui::upper (aicore::ui::tr ("Goal")), setupBox.getX() + 20, gy, 200, 26, juce::Justification::centredLeft);
        g.setColour (col::accent);
        g.fillEllipse ((float) setupBox.getX() + 20 + juce::GlyphArrangement::getStringWidth (uiFont (15.0f, true).withExtraKerningFactor (0.1f), juce::String::fromUTF8 ("2 \xc2\xb7 GOAL")) + 12,
                       (float) gy + 10, 5, 5);
        g.setColour (col::grid);
        g.fillRect (setupBox.getX() + 16, gy + 32, setupBox.getWidth() - 32, 1);
        return;
    }
    drawPanel (g, leftBox);
    drawPanel (g, centreBox);
    const int lx = leftBox.getX() + 14, lw = leftBox.getWidth() - 28;
    drawHeading (g, "Start from", lx, leftBox.getY() + 12, lw);
    if (! scratch)
        drawHeading (g, "Music", lx, headingMusicY, lw, true);
    const int cx = centreBox.getX() + 16, cw = centreBox.getWidth() - 32;
    if (! scratch)
    {
        g.setColour (col::title);
        g.setFont (uiFont (15.0f, true).withExtraKerningFactor (0.1f));
        g.drawText (aicore::ui::tr ("PART TO CHANGE"), cx + 4, centreBox.getY() + 14, 400, 26, juce::Justification::centredLeft);
    }
    drawHeading (g, "Ask ACE", cx, headingAskY, cw, ! scratch);
    drawHeading (g, "Results", cx, headingResultsY, cw, true);
    for (auto& t : takes)
    {
        auto b = t->box.toFloat();
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfff6f8fa), 0, b.getY(), juce::Colour (0xffe9edf1), 0, b.getBottom(), false));
        g.fillRoundedRectangle (b, 2.0f);
        g.setColour (col::outline);
        g.drawRoundedRectangle (b.reduced (0.5f), 2.0f, 1.0f);
        const auto mode = t->result["mode"].toString();
        const auto what = mode == "repaint" ? "Repaint" : mode == "cover" ? "Cover" : mode == "text2music" ? "From scratch" : "";
        g.setColour (col::text);
        g.setFont (uiFont (15.0f, true));
        const int idx = (int) (&t - &takes[0]);
        g.drawText (aicore::ui::tr ("Take") + " " + juce::String (idx + 1) + juce::String::fromUTF8 (" \xc2\xb7 ") + aicore::ui::tr (what), t->play->getRight() + 10, t->box.getY(), 400, t->box.getHeight(),
                    juce::Justification::centredLeft);
    }
}

void AiPage::layoutTakes() {}

void AiPage::resized()
{
    auto r = getLocalBounds();
    auto row = r.removeFromTop (38).withTrimmedTop (4).withTrimmedLeft (4);
    aceButton.setBounds (row.removeFromLeft (juce::jmax (150, aceButton.idealWidth (28))).withHeight (30));
    row.removeFromLeft (6);
    flamingoButton.setBounds (row.removeFromLeft (juce::jmax (150, flamingoButton.idealWidth (28))).withHeight (30));
    r = r.reduced (4, 0).withTrimmedTop (8).withTrimmedBottom (4);

    if (flamingo)
    {
        setupBox = r.removeFromRight (338);
        r.removeFromRight (8);
        askBox = r;
        flamingoDots.setBounds (askBox.getRight() - 52, askBox.getY() + 12, 36, 26);
        flamingoState.setBounds (askBox.getX() + 32, askBox.getY() + 66, askBox.getWidth() - 48, 40);
        auto s = setupBox.withTrimmedTop (60).reduced (16, 0);
        chooseAudio.setBounds (s.removeFromTop (24).withTrimmedLeft (4));
        refBox = s.removeFromTop (130).withTrimmedTop (2);
        auto in = refBox.reduced (20, 18);
        referenceLabel.setBounds (in.removeFromTop (24).withTrimmedLeft (-4));
        in.removeFromTop (6);
        reference.setBounds (in.removeFromTop (28));
        in.removeFromTop (6);
        chooseReference.setBounds (in.removeFromTop (30));
        s.removeFromTop (66);
        goal.setBounds (s.removeFromTop (28));
        s.removeFromTop (10);
        analyse.setBounds (s.removeFromTop (30));
        return;
    }
    leftBox = r.removeFromLeft (300);
    r.removeFromLeft (8);
    centreBox = r;
    auto l = leftBox.reduced (14, 0).withTrimmedTop (54);
    fromMusic.setBounds (l.removeFromTop (30));
    l.removeFromTop (8);
    fromScratch.setBounds (l.removeFromTop (30));
    headingMusicY = l.getY() + 22;
    l.removeFromTop (22 + 40 + 16);
    auto mrow = l.removeFromTop (30);
    musicButton.setBounds (mrow.removeFromRight (musicButton.idealWidth (26)).reduced (0, 0).translated (-8, 0));
    music.setBounds (mrow.withTrimmedLeft (4));

    auto c = centreBox.reduced (16, 0).withTrimmedTop (14).withTrimmedBottom (14);
    if (! scratch)
    {
        c.removeFromTop (32);
        wave.setBounds (c.removeFromTop (120));
        c.removeFromTop (6);
        auto modes = c.removeFromTop (30);
        repaintButton.setBounds (modes.removeFromLeft (repaintButton.idealWidth (26)));
        modes.removeFromLeft (6);
        coverButton.setBounds (modes.removeFromLeft (coverButton.idealWidth (26)));
        listenPart.setBounds (modes.removeFromRight (listenPart.idealWidth (26)));
        c.removeFromTop (22);
    }
    headingAskY = c.getY();
    c.removeFromTop (40);
    auto p = c.removeFromTop (76);
    promptLabel.setBounds (p.removeFromLeft (60).withTrimmedTop (2));
    prompt.setBounds (p);
    c.removeFromTop (8);
    auto nums = c.removeFromTop (28);
    bpmLabel.setBounds (nums.removeFromLeft (60).withTrimmedTop (4));
    const int third = (nums.getWidth() - 2 * 70) / 3;
    bpm.setBounds (nums.removeFromLeft (third));
    nums.removeFromLeft (10);
    if (length.isVisible() || strength.isVisible())
    {
        auto& lab = length.isVisible() ? secondsLabel : strengthLabel;
        auto& spin = length.isVisible() ? (juce::Component&) length : (juce::Component&) strength;
        lab.setBounds (nums.removeFromLeft (60).withTrimmedTop (4));
        spin.setBounds (nums.removeFromLeft (third));
    }
    c.removeFromTop (8);
    generateButton.setBounds (c.removeFromTop (30));
    c.removeFromTop (22);
    headingResultsY = c.getY();
    c.removeFromTop (40);
    auto foot = c.removeFromBottom (24);
    if (stopButton.isVisible())
        stopButton.setBounds (foot.removeFromRight (stopButton.idealWidth (26)).withSizeKeepingCentre (stopButton.idealWidth (26), 26));
    state.setBounds (foot);
    noResults.setBounds (c.removeFromTop (24));
    int y = c.getY();
    for (auto& t : takes)
    {
        t->box = { c.getX(), y, c.getWidth(), 42 };
        auto b = t->box.reduced (10, 6);
        t->play->setBounds (b.removeFromLeft (44));
        t->save->setBounds (b.removeFromRight (t->save->idealWidth (26)));
        b.removeFromRight (6);
        t->openEdit->setBounds (b.removeFromRight (t->openEdit->idealWidth (26)));
        y += 48;
    }
}
} // namespace aicore::ai
