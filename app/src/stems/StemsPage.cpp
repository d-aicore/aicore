#include "StemsPage.h"
#include "../ui/I18n.h"

#include "../ui/AppSettings.h"
#include "../ui/AudioOut.h"
#include "../ui/Keybinds.h"

#include <cmath>
#include <set>

namespace aicore::stems
{
using namespace aicore::ui;

namespace
{
constexpr float meterFloor = -60.0f, meterTop = 3.0f;
const juce::Colour clipText { 0xff1f2633 };

juce::Colour mixc (juce::Colour a, juce::Colour b, float t)
{
    return juce::Colour ((juce::uint8) juce::roundToInt (a.getRed() + (b.getRed() - a.getRed()) * t),
                         (juce::uint8) juce::roundToInt (a.getGreen() + (b.getGreen() - a.getGreen()) * t),
                         (juce::uint8) juce::roundToInt (a.getBlue() + (b.getBlue() - a.getBlue()) * t));
}

/** Qt's QColor::lighter / darker (HSV value times the factor; over the top, less saturation). */
juce::Colour qtLighter (juce::Colour c, float factor)
{
    float h, s, v;
    c.getHSB (h, s, v);
    v *= factor;
    if (v > 1.0f)
    {
        s = juce::jmax (0.0f, s - (v - 1.0f));
        v = 1.0f;
    }
    return juce::Colour::fromHSV (h, s, v, c.getFloatAlpha());
}

juce::String clockText (double seconds)
{
    seconds = juce::jmax (0.0, seconds);
    const int m = (int) (seconds / 60);
    return juce::String (m) + ":" + juce::String (seconds - m * 60, 3).paddedLeft ('0', 6);
}

double sv (const tracks::ChainSettings& s, const char* key, double def = 0)
{
    auto it = s.find (key);
    return it == s.end() ? def : it->second;
}

/** The insert list of a strip (trackview.py insert_states): lit when it changes the sound. */
std::vector<std::pair<juce::String, bool>> insertStates (const tracks::ChainSettings& s)
{
    return { { "EQ", sv (s, "lowDb") != 0 || sv (s, "midDb") != 0 || sv (s, "highDb") != 0 },
             { "High-pass", sv (s, "highpassHz") > 0 },
             { "Compressor", sv (s, "compress") != 0 },
             { "LUFS", sv (s, "targetLufs") != 0 },
             { "Limiter", sv (s, "limit") != 0 },
             { "Fade", sv (s, "fadeInS") > 0 || sv (s, "fadeOutS") > 0 } };
}

void drawToggle (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, bool on, juce::Colour onColour)
{
    g.setColour (on ? onColour : col::control);
    g.fillRoundedRectangle (r, 2.0f);
    if (! on)
    {
        g.setColour (col::edge);
        g.drawRoundedRectangle (r, 2.0f, 1.0f);
    }
    g.setColour (on ? col::onText : col::selectedText);
    g.setFont (uiFont (15.0f));
    g.drawText (text, r, juce::Justification::centred);
}

void drawMeterBar (juce::Graphics& g, juce::Rectangle<float> track, float db, bool vertical, float low, float high)
{
    auto frac = [&] (float d) { return juce::jlimit (0.0f, 1.0f, (d - low) / (high - low)); };
    g.setColour (col::grid);
    g.fillRoundedRectangle (track, 2.5f);
    const float level = frac (db);
    if (db <= low || level <= 0)
        return;
    juce::ColourGradient grad = vertical ? juce::ColourGradient (col::accent, 0, track.getBottom(), col::error, 0, track.getY(), false)
                                         : juce::ColourGradient (col::accent, track.getX(), 0, col::error, track.getRight(), 0, false);
    grad.clearColours();
    grad.addColour (0.0, col::accent);
    grad.addColour (frac (-6.0f), col::accent);
    grad.addColour (frac (-3.0f), col::muteOn);
    grad.addColour (frac (0.0f), col::error);
    g.setGradientFill (grad);
    auto lit = vertical ? track.withTop (track.getBottom() - level * track.getHeight()) : track.withWidth (level * track.getWidth());
    g.fillRoundedRectangle (lit, 2.5f);
}

class ExportTracksDialog : public juce::Component
{
public:
    ExportTracksDialog (const juce::StringArray& heard, const juce::String& name, const juce::File& folder, const sample::ExportChoice& last)
    {
        what.setText ("What you hear: " + heard.joinIntoString (", "), juce::dontSendNotification);
        what.setFont (uiFont (15.0f));
        addAndMakeVisible (what);
        nameField.setText (name);
        folderField.setText (folder.getFullPathName());
        for (auto* t : { &nameField, &folderField })
        {
            t->setFont (uiFont (15.0f));
            t->setIndents (8, 5);
            addAndMakeVisible (t);
        }
        pick.onClick = [this] {
            chooser = std::make_unique<juce::FileChooser> ("Choose folder", juce::File (folderField.getText()));
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc) {
                if (fc.getResult() != juce::File())
                    folderField.setText (fc.getResult().getFullPathName());
            });
        };
        addAndMakeVisible (pick);
        for (auto [l, t] : { std::pair<juce::Label*, const char*> { &nl, "Name" }, { &fl, "Folder" }, { &fol, "Format:" }, { &bl, "Bits:" }, { &rl, "Sample rate:" }, { &cl, "Channels:" } })
        {
            l->setText (t, juce::dontSendNotification);
            l->setFont (uiFont (15.0f));
            addAndMakeVisible (l);
        }
        format.addItemList ({ "WAV (*.wav)", "FLAC (*.flac)", "MP3 (*.mp3)" }, 1);
        rate.addItemList ({ "Same as the track", "44.1 kHz", "48 kHz", "88.2 kHz", "96 kHz" }, 1);
        channels.addItemList ({ "Stereo (as it is)", "Mono (left + right in one channel)" }, 1);
        for (auto* c : { &format, &bits, &rate, &channels })
        {
            c->setColour (juce::ComboBox::backgroundColourId, col::field);
            c->setColour (juce::ComboBox::textColourId, col::text);
            c->setColour (juce::ComboBox::outlineColourId, col::outline);
            addAndMakeVisible (c);
        }
        format.onChange = [this] { fillBits(); };
        format.setSelectedItemIndex (last.format == "flac" ? 1 : last.format == "mp3" ? 2 : 0, juce::dontSendNotification);
        fillBits (last.bits);
        const int rates[] = { 0, 44100, 48000, 88200, 96000 };
        rate.setSelectedItemIndex (0, juce::dontSendNotification);
        for (int i = 0; i < 5; ++i)
            if (rates[i] == last.rate)
                rate.setSelectedItemIndex (i, juce::dontSendNotification);
        channels.setSelectedItemIndex (last.mono ? 1 : 0, juce::dontSendNotification);
        ok.onClick = [this] { finish (true); };
        cancel.onClick = [this] { finish (false); };
        addAndMakeVisible (ok);
        addAndMakeVisible (cancel);
        setSize (520, 330);
    }
    std::function<void (juce::String name, juce::File folder, sample::ExportChoice)> onExport;

    void paint (juce::Graphics& g) override { g.fillAll (col::card); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        what.setBounds (r.removeFromTop (40));
        r.removeFromTop (6);
        auto row = [&] (juce::Label& l, juce::Component& c, juce::Component* extra = nullptr) {
            auto line = r.removeFromTop (28);
            l.setBounds (line.removeFromLeft (110));
            if (extra != nullptr)
            {
                extra->setBounds (line.removeFromRight (36));
                line.removeFromRight (6);
            }
            c.setBounds (line);
            r.removeFromTop (6);
        };
        row (nl, nameField);
        row (fl, folderField, &pick);
        row (fol, format);
        row (bl, bits);
        row (rl, rate);
        row (cl, channels);
        auto b = r.removeFromBottom (30);
        cancel.setBounds (b.removeFromRight (90));
        b.removeFromRight (8);
        ok.setBounds (b.removeFromRight (100));
    }

private:
    void fillBits (juce::String keep = {})
    {
        if (keep.isEmpty())
            keep = bitKeys[juce::jmax (0, bits.getSelectedItemIndex())];
        bits.clear (juce::dontSendNotification);
        bitKeys.clear();
        const int f = format.getSelectedItemIndex();
        juce::StringArray keys = f == 0 ? juce::StringArray { "16", "24", "32f" } : f == 1 ? juce::StringArray { "16", "24" } : juce::StringArray {};
        for (auto& k : keys)
        {
            bits.addItem (k == "16" ? "16-bit" : k == "24" ? "24-bit" : "32-bit float", bits.getNumItems() + 1);
            bitKeys.add (k);
        }
        if (keys.isEmpty())
        {
            bits.addItem ("320 kbps", 1);
            bitKeys.add ("");
        }
        bits.setSelectedItemIndex (juce::jmax (0, bitKeys.indexOf (keys.contains (keep) ? keep : juce::String ("24"))), juce::dontSendNotification);
        bits.setEnabled (bits.getNumItems() > 1);
    }
    void finish (bool accepted)
    {
        auto cb = onExport;
        sample::ExportChoice c;
        const int f = format.getSelectedItemIndex();
        c.format = f == 1 ? "flac" : f == 2 ? "mp3" : "wav";
        c.bits = bitKeys[juce::jmax (0, bits.getSelectedItemIndex())];
        const int rates[] = { 0, 44100, 48000, 88200, 96000 };
        c.rate = rates[juce::jlimit (0, 4, rate.getSelectedItemIndex())];
        c.mono = channels.getSelectedItemIndex() == 1;
        const auto n = nameField.getText().trim();
        const juce::File folder (folderField.getText().trim());
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState (0);
        if (accepted && cb)
            cb (n, folder, c);
    }
    juce::Label what, nl, fl, fol, bl, rl, cl;
    juce::TextEditor nameField, folderField;
    sample::IconButton pick { sample::IconButton::folder };
    juce::ComboBox format, bits, rate, channels;
    juce::StringArray bitKeys;
    AButton ok { "Export" }, cancel { "Cancel" };
    std::unique_ptr<juce::FileChooser> chooser;
};

void launch (juce::Component* content, const juce::String& title)
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (content);
    o.dialogTitle = title;
    o.dialogBackgroundColour = col::card;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

tracks::ChainSettings settingsFromVar (const juce::var& v)
{
    tracks::ChainSettings s;
    if (auto* o = v.getDynamicObject())
        for (auto& p : o->getProperties())
            s[p.name.toString().toStdString()] = p.value.isBool() ? ((bool) p.value ? 1.0 : 0.0) : (double) p.value;
    return s;
}

juce::var settingsVar (const tracks::ChainSettings& s)
{
    auto* o = new juce::DynamicObject();
    for (auto& [k, v] : s)
    {
        if (k == "compress" || k == "limit")
            o->setProperty (juce::Identifier (k), v != 0);
        else
            o->setProperty (juce::Identifier (k), v);
    }
    return juce::var (o);
}

tracks::ChainSettings trackDefaults()
{
    return { { "gainDb", 0 }, { "lowDb", 0 }, { "midDb", 0 }, { "highDb", 0 }, { "highpassHz", 0 }, { "fadeInS", 0 },
             { "fadeOutS", 0 }, { "targetLufs", 0 }, { "compress", 0 }, { "limit", 0 } };
}

tracks::ChainSettings masterDefaults()
{
    return { { "gainDb", 0 }, { "limit", 1 }, { "ceilingDb", -1 }, { "fadeInS", 0 }, { "fadeOutS", 0 }, { "lowDb", 0 },
             { "midDb", 0 }, { "highDb", 0 }, { "highpassHz", 0 }, { "compress", 0 }, { "targetLufs", 0 } };
}

/** Peaks per column (low, high, rms), read in blocks off the message thread (trackview.py _Peaks). */
class PeakJob : public juce::ThreadPoolJob
{
public:
    PeakJob (juce::String p, std::function<void (juce::String, std::shared_ptr<Peaks>)> cb) : ThreadPoolJob ("peaks"), path (std::move (p)), done (std::move (cb)) {}
    JobStatus runJob() override
    {
        auto peaks = std::make_shared<Peaks>();
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (juce::File (path)));
        if (r != nullptr && r->lengthInSamples > 0)
        {
            const juce::int64 frames = r->lengthInSamples;
            const int chans = (int) juce::jmin<unsigned int> (2, r->numChannels);
            const int columns = (int) juce::jmin<juce::int64> (4096, frames);
            peaks->numChannels = chans;
            peaks->seconds = frames / r->sampleRate;
            for (int ch = 0; ch < chans; ++ch)
                peaks->channels[ch].assign ((size_t) columns, { 0, 0, 0 });
            std::vector<double> sums ((size_t) columns * 2, 0.0);
            std::vector<bool> seen ((size_t) columns, false);
            juce::AudioBuffer<float> block (chans, 1 << 16);
            for (juce::int64 pos = 0; pos < frames && ! shouldExit(); pos += block.getNumSamples())
            {
                const int n = (int) juce::jmin<juce::int64> (block.getNumSamples(), frames - pos);
                r->read (&block, 0, n, pos, true, chans > 1);
                for (int i = 0; i < n; ++i)
                {
                    const int col = (int) ((pos + i) * columns / frames);
                    for (int ch = 0; ch < chans; ++ch)
                    {
                        const float v = block.getSample (ch, i);
                        auto& c = peaks->channels[ch][(size_t) col];
                        if (! seen[(size_t) col])
                            c = { v, v, 0 };
                        c[0] = juce::jmin (c[0], v);
                        c[1] = juce::jmax (c[1], v);
                        sums[(size_t) col * 2 + (size_t) ch] += (double) v * v;
                    }
                    seen[(size_t) col] = true;
                }
            }
            for (int c = 0; c < columns; ++c)
            {
                const double count = juce::jmax<juce::int64> (1, (juce::int64) ((c + 1) * frames / columns) - (juce::int64) (c * frames / columns));
                for (int ch = 0; ch < chans; ++ch)
                    peaks->channels[ch][(size_t) c][2] = (float) std::sqrt (sums[(size_t) c * 2 + (size_t) ch] / count);
            }
        }
        juce::MessageManager::callAsync ([cb = done, p = path, peaks] { cb (p, peaks); });
        return jobHasFinished;
    }

private:
    juce::String path;
    std::function<void (juce::String, std::shared_ptr<Peaks>)> done;
};
} // namespace

juce::Colour stemColour (const juce::String& name)
{
    static const std::pair<const char*, juce::uint32> roles[] = {
        { "kick", 0xffe39d96 }, { "bass sub 303 acid", 0xff6a9cf0 }, { "hat hats ride cymbal", 0xffe0c070 },
        { "clap snare", 0xffd9845a }, { "perc tom conga shaker drum", 0xffddb07c }, { "lead synth arp pluck other", 0xffa98bd0 },
        { "pad atmo drone ambient", 0xff6fb08f }, { "fx riser impact crash sweep noise", 0xffc47fa8 },
        { "vocal vocals voice chop", 0xffc98bb9 } };
    static const juce::uint32 others[] = { 0xff7fa9c6, 0xff8a8fc0, 0xffb9a56a, 0xff6f8fb0, 0xffd88080, 0xff7a9ab8 };
    const auto key = name.toLowerCase();
    for (auto& [words, colour] : roles)
        for (auto& w : juce::StringArray::fromTokens (words, " ", ""))
            if (key.contains (w))
                return juce::Colour (colour);
    int sum = 0;
    for (auto c : name)
        sum += (int) c;
    return juce::Colour (others[(size_t) sum % 6]);
}

//==============================================================================
Lanes::Lanes (StemsPage& p) : page (p)
{
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

int Lanes::laneHeight() const
{
    const int n = juce::jmax (1, (int) page.rows.size());
    return juce::jmax (rowMin, (getHeight() - rulerH - 2) / n);
}

juce::Rectangle<int> Lanes::partOf (int r, const juce::String& part) const
{
    auto lane = laneBox (r);
    const int cy = lane.getCentreY();
    int x = 150; // as the old head: M at 150 px, S, Listen, the volume bar, the meter
    if (part == "m") return { x, cy - 11, 26, 22 };
    x += 30;
    if (part == "s") return { x, cy - 11, 26, 22 };
    x += 32;
    if (part == "listen") return { x, cy - 11, 52, 22 };
    x += 52 + 10;
    if (part == "volume") return { x, cy - 11, 110, 22 };
    x += 116;
    if (part == "meter") return { x, cy - 7, 56, 14 };
    return {};
}

float Lanes::xOfSeconds (const juce::Rectangle<int>& clip, double s) const
{
    const double tl = page.timeline();
    const float width = (float) clip.getWidth() - (waveLeft - 1) - (waveRight - 1);
    return clip.getX() + waveLeft + (tl > 0 ? (float) (width * juce::jmin (s, tl) / tl) : 0.0f);
}

double Lanes::secondsOfX (const juce::Rectangle<int>& clip, float x) const
{
    const double tl = page.timeline();
    const float width = (float) clip.getWidth() - waveLeft - waveRight;
    if (width <= 0 || tl <= 0)
        return 0;
    return juce::jlimit (0.0, tl, (x - clip.getX() - waveLeft) / width * tl);
}

Lanes::Hit Lanes::hit (juce::Point<int> p) const
{
    if (p.y < rulerH)
        return { -1, "ruler" };
    const int r = (p.y - rulerH) / juce::jmax (1, laneHeight());
    if (r < 0 || r >= (int) page.rows.size())
        return {};
    for (auto part : { "m", "s", "listen", "volume" })
        if (partOf (r, part).contains (p))
            return { r, part };
    if (clipBox (r).contains (p))
        return { r, "clip" };
    return { r, "head" };
}

void Lanes::paint (juce::Graphics& g)
{
    g.fillAll (col::field);
    const double tl = page.timeline();
    // the ruler: seconds lined up with the clips
    if (tl > 0)
    {
        const int left = headW + 4 + waveLeft;
        const float width = (float) getWidth() - left - 4;
        double step = 300;
        for (double s : { 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0 })
            if (width / (tl / s) >= 70)
            {
                step = s;
                break;
            }
        g.setColour (col::outline);
        g.drawHorizontalLine (rulerH - 1, (float) left, (float) getWidth() - 4);
        g.setFont (uiFont (13.0f));
        for (int n = 0; n * step <= tl; ++n)
        {
            const float x = left + width * (float) (n * step / tl);
            g.setColour (col::outline);
            g.drawVerticalLine ((int) x, rulerH - 7.0f, (float) rulerH - 1);
            for (int k = 1; k < 4; ++k)
            {
                const float xm = x + width * (float) (step * k / 4 / tl);
                if (xm < getWidth() - 4)
                    g.drawVerticalLine ((int) xm, rulerH - 4.0f, (float) rulerH - 1);
            }
            const double sec = n * step;
            juce::String label;
            if (sec >= 60)
                label = juce::String ((int) (sec / 60)) + ":" + juce::String (std::fmod (sec, 60.0), 1).paddedLeft ('0', 4);
            else if (std::abs (sec - std::round (sec)) < 1e-9)
                label = juce::String ((int) std::round (sec)) + " s";
            else
                label = juce::String (sec, 1) + " s";
            g.setColour (col::muted);
            g.drawText (label, (int) x + 3, 0, 60, 16, juce::Justification::centredLeft, false);
        }
        const float x = left + width * (float) (juce::jmin (page.playhead, tl) / tl);
        juce::Path tri;
        tri.addTriangle (x - 5, 0, x + 5, 0, x, 8);
        g.setColour (col::text);
        g.fillPath (tri);
        g.drawLine (x, 0, x, (float) rulerH, 1.5f);
    }
    for (int r = 0; r < (int) page.rows.size(); ++r)
    {
        const auto& row = page.rows[(size_t) r];
        const auto colour = stemColour (row.name);
        const bool selected = r == page.current;
        // the head: a card, the colour as a strip at the left
        auto head = laneBox (r).withWidth (headW).toFloat().reduced (0.5f, 3.5f);
        g.setColour (col::card);
        g.fillRoundedRectangle (head, 2.0f);
        g.setColour (selected ? colour : col::grid);
        g.drawRoundedRectangle (head, 2.0f, selected ? 2.0f : 1.0f);
        {
            juce::Graphics::ScopedSaveState s (g);
            juce::Path clipPath;
            clipPath.addRoundedRectangle (head, 4.0f);
            g.reduceClipRegion (clipPath);
            g.setColour (colour);
            g.fillRect (head.withWidth (6.0f));
        }
        g.setColour (col::text);
        g.setFont (uiFont (15.0f, true));
        g.drawFittedText (row.name, juce::Rectangle<int> (14, laneBox (r).getY(), 150, laneBox (r).getHeight()), juce::Justification::centredLeft, 2);
        drawToggle (g, partOf (r, "m").toFloat(), "M", row.mute, col::muteOn);
        drawToggle (g, partOf (r, "s").toFloat(), "S", row.solo, col::accent);
        {
            auto lb = partOf (r, "listen").toFloat();
            const bool on = page.only == r && page.isPlaying();
            drawToggle (g, lb, "", on, col::hover);
            g.setColour (on ? col::onText : col::selectedText);
            g.setFont (uiFont (13.0f));
            g.drawText (aicore::ui::tr (on ? "Pause" : "Listen"), lb, juce::Justification::centred);
        }
        {
            auto vb = partOf (r, "volume").toFloat();
            const double gain = sv (row.settings, "gainDb");
            const float frac = (float) juce::jlimit (0.0, 1.0, (gain + 60) / 72.0);
            g.setColour (col::field);
            g.fillRoundedRectangle (vb, 2.0f);
            g.setColour (col::hover);
            g.fillRoundedRectangle (vb.withWidth (vb.getWidth() * frac), 2.0f);
            g.setColour (col::outline);
            g.drawRoundedRectangle (vb, 2.0f, 1.0f);
            g.setColour (col::text);
            g.setFont (uiFont (13.0f));
            g.drawText (juce::String (gain, 1), vb, juce::Justification::centred);
        }
        drawMeterBar (g, partOf (r, "meter").toFloat().withSizeKeepingCentre (56, 5), page.levelDb (r), false, meterFloor, meterTop);

        // the clip: Core's fillClip in the track's colour, the wave light in it
        auto box = clipBox (r).toFloat();
        const auto base = mixc (colour, juce::Colour (0xff4c5568), 0.22f);
        auto inside = qtLighter (base, 1.12f), middle = qtLighter (base, 1.35f).withAlpha (0.8f);
        if (row.mute)
        {
            inside = inside.withMultipliedAlpha (0.5f);
            middle = middle.withMultipliedAlpha (0.5f);
        }
        juce::ColourGradient grad (inside, 0, box.getY(), inside, 0, box.getBottom(), false);
        grad.addColour (0.5, middle);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (box.reduced (0.5f), 2.0f);
        g.setColour (qtLighter (base, 1.0f / 1.6f));
        g.drawRoundedRectangle (box.reduced (0.5f), 2.0f, 1.2f);
        auto pk = page.peaks.count (row.path) ? page.peaks.at (row.path) : nullptr;
        if (pk != nullptr && pk->numChannels > 0)
        {
            auto area = box.withTrimmedLeft (waveLeft - 1).withTrimmedTop (18).withTrimmedRight (2).withTrimmedBottom (3);
            if (tl > 0 && pk->seconds > 0)
                area.setWidth (area.getWidth() * (float) juce::jmin (1.0, pk->seconds / tl));
            const float band = area.getHeight() / pk->numChannels;
            const float alpha = row.mute ? 0.45f : 1.0f;
            for (int ch = 0; ch < pk->numChannels; ++ch)
            {
                auto wb = juce::Rectangle<float> (area.getX(), area.getY() + ch * band + (ch ? 4.0f : 0.0f), area.getWidth(), band - 6);
                const auto& cols = pk->channels[ch];
                const int n = (int) cols.size(), width = juce::jmax (1, (int) wb.getWidth());
                const float mid = wb.getCentreY(), half = wb.getHeight() / 2;
                juce::Path outline, body;
                std::vector<juce::Point<float>> tops, bottoms, rt, rb;
                for (int i = 0; i <= width; ++i)
                {
                    const int a = juce::jmin (n - 1, (int) ((double) i / width * n)), b = juce::jlimit (a + 1, n, (int) ((double) (i + 1) / width * n));
                    float lo = cols[(size_t) a][0], hi = cols[(size_t) a][1], rms = 0;
                    for (int k = a; k < b; ++k)
                    {
                        lo = juce::jmin (lo, cols[(size_t) k][0]);
                        hi = juce::jmax (hi, cols[(size_t) k][1]);
                        rms = juce::jmax (rms, cols[(size_t) k][2]);
                    }
                    const float x = wb.getX() + i;
                    tops.push_back ({ x, mid - juce::jmax (0.4f, hi * half) });
                    bottoms.push_back ({ x, mid - juce::jmin (-0.4f, lo * half) });
                    rt.push_back ({ x, mid - rms * half });
                    rb.push_back ({ x, mid + rms * half });
                }
                auto shape = [] (const std::vector<juce::Point<float>>& up, const std::vector<juce::Point<float>>& low) {
                    juce::Path p;
                    p.startNewSubPath (up[0]);
                    for (size_t i = 1; i < up.size(); ++i)
                        p.lineTo (up[i]);
                    for (size_t i = low.size(); i-- > 0;)
                        p.lineTo (low[i]);
                    p.closeSubPath();
                    return p;
                };
                g.setColour (juce::Colours::white.withAlpha (90 / 255.0f * alpha));
                g.drawHorizontalLine ((int) mid, wb.getX(), wb.getRight());
                g.setColour (juce::Colours::white.withAlpha (0.42f * alpha));
                g.fillPath (shape (tops, bottoms));
                g.setColour (juce::Colour (0xfff4f7fa).withAlpha (0.95f * alpha));
                g.fillPath (shape (rt, rb));
            }
        }
        if (tl > 0)
        {
            const float width = box.getWidth() - (waveLeft - 1) - (waveRight - 1);
            const double span = pk != nullptr && pk->seconds > 0 ? pk->seconds : tl;
            const double fi = sv (row.settings, "fadeInS"), fo = sv (row.settings, "fadeOutS");
            g.setColour (mixc (colour, clipText, 0.5f));
            const float wl = box.getX() + waveLeft;
            if (fi > 0)
                g.drawLine (wl, box.getBottom() - 2, wl + width * (float) (juce::jmin (fi, span) / tl), box.getY() + 18, 1.2f);
            if (fo > 0)
            {
                const float end = wl + width * (float) (span / tl);
                g.drawLine (end - width * (float) (juce::jmin (fo, span) / tl), box.getY() + 18, end, box.getBottom() - 2, 1.2f);
            }
            if (page.zone.row == r && page.zone.t1 > page.zone.t0)
            {
                const float za = wl + width * (float) (juce::jmin (page.zone.t0, tl) / tl), zb = wl + width * (float) (juce::jmin (page.zone.t1, tl) / tl);
                g.setColour (colour.withAlpha (0.28f));
                g.fillRect (za, box.getY(), zb - za, box.getHeight());
                g.setColour (colour.darker (0.4f));
                g.drawVerticalLine ((int) za, box.getY(), box.getBottom());
                g.drawVerticalLine ((int) zb, box.getY(), box.getBottom());
            }
            const float x = wl + width * (float) (juce::jmin (page.playhead, tl) / tl);
            g.setColour (col::text);
            g.drawLine (x, box.getY(), x, box.getBottom(), 1.5f);
        }
        g.setColour (colour.withAlpha (row.mute ? 0.35f : 0.85f));
        g.fillRect (box.getX() + 2, box.getY(), box.getWidth() - 4, 2.0f);
        if (selected)
        {
            g.setColour (colour);
            g.drawRoundedRectangle (box.reduced (1), 2.0f, 2.0f);
        }
        g.setColour (clipText);
        g.setFont (uiFont (13.0f, true));
        g.drawText (row.name, box.reduced (6, 3).withHeight (16), juce::Justification::topLeft, true);
    }
}

void Lanes::mouseMove (const juce::MouseEvent& e)
{
    auto h = hit (e.getPosition());
    setMouseCursor (h.part == "clip" ? juce::MouseCursor::IBeamCursor
                    : h.part == "ruler" || h.part == "m" || h.part == "s" || h.part == "listen" ? juce::MouseCursor::PointingHandCursor
                    : h.part == "volume" ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void Lanes::mouseDown (const juce::MouseEvent& e)
{
    auto h = hit (e.getPosition());
    pressRow = -1;
    draggingVolume = false;
    if (h.part == "ruler")
    {
        if (page.timeline() > 0)
        {
            const int left = headW + 4 + waveLeft;
            page.seek (juce::jlimit (0.0, page.timeline(), (e.x - left) / (double) (getWidth() - left - 4) * page.timeline()));
        }
        return;
    }
    if (h.row < 0)
        return;
    page.select (h.row);
    auto& row = page.rows[(size_t) h.row];
    if (h.part == "m")
    {
        row.mute = ! row.mute;
        page.changed();
    }
    else if (h.part == "s")
    {
        row.solo = ! row.solo;
        page.changed();
    }
    else if (h.part == "listen")
        page.listenStem (h.row);
    else if (h.part == "volume")
    {
        draggingVolume = true;
        pressRow = h.row;
        volumeStart = sv (row.settings, "gainDb");
    }
    else if (h.part == "clip" && page.timeline() > 0)
    {
        pressRow = h.row;
        pressX = (float) e.x;
        pressSeconds = secondsOfX (clipBox (h.row), (float) e.x);
        page.seek (pressSeconds);
    }
}

void Lanes::mouseDrag (const juce::MouseEvent& e)
{
    if (pressRow < 0)
        return;
    if (draggingVolume)
    {
        auto& row = page.rows[(size_t) pressRow];
        row.settings["gainDb"] = juce::jlimit (-60.0, 12.0, std::round ((volumeStart + e.getDistanceFromDragStartX() / 110.0 * 72.0) * 10) / 10);
        page.changed();
        return;
    }
    if (std::abs (e.x - pressX) < 4)
        return;
    const double t = secondsOfX (clipBox (pressRow), (float) e.x);
    page.setZone ({ pressRow, juce::jmin (t, pressSeconds), juce::jmax (t, pressSeconds) });
}

void Lanes::mouseUp (const juce::MouseEvent& e)
{
    if (pressRow >= 0 && ! draggingVolume && std::abs (e.x - pressX) < 4)
        page.setZone ({}); // a click: no zone
    if (draggingVolume)
        page.changed (false);
    pressRow = -1;
    draggingVolume = false;
}

void Lanes::mouseDoubleClick (const juce::MouseEvent& e)
{
    auto h = hit (e.getPosition());
    if (h.part == "volume")
    {
        page.rows[(size_t) h.row].settings["gainDb"] = 0;
        page.changed();
    }
}

//==============================================================================
Mixer::Mixer (StemsPage& p) : page (p)
{
    inserts.setInsertsMode ("INSERTS", false);
    inserts.onChanged = [this] (const tracks::ChainSettings& s) {
        auto apply = [&] (tracks::ChainSettings& target) {
            for (auto& [k, v] : s)
                target[k] = v;
        };
        if (page.masterChosen)
            apply (page.master);
        else if (page.current >= 0 && page.current < (int) page.rows.size())
            apply (page.rows[(size_t) page.current].settings);
        page.changed();
    };
    insertsView.setViewedComponent (&inserts, false);
    insertsView.setScrollBarsShown (true, false);
    addAndMakeVisible (insertsView);
}

void Mixer::refresh()
{
    if (page.masterChosen)
    {
        inserts.setInsertsMode (juce::String::fromUTF8 ("INSERTS \xc2\xb7 MASTER"), true);
        inserts.showSettings (page.master, true);
    }
    else if (page.current >= 0 && page.current < (int) page.rows.size())
    {
        const auto& row = page.rows[(size_t) page.current];
        inserts.setInsertsMode (juce::String::fromUTF8 ("INSERTS \xc2\xb7 ") + row.name.toUpperCase(), false);
        inserts.showSettings (row.settings, false);
    }
    inserts.setVisible (page.masterChosen || page.current >= 0);
    resized();
    repaint();
}

juce::Rectangle<int> Mixer::stripBox (int row) const
{
    auto r = getLocalBounds().reduced (6);
    if (row < 0)
        return { r.getRight() - insertsW - 10 - stripW, r.getY(), stripW, r.getHeight() };
    return { r.getX() + 6 + row * (stripW + 6), r.getY() + 1, stripW, r.getHeight() - 2 };
}

juce::Rectangle<int> Mixer::faderBox (int row) const
{
    auto s = stripBox (row);
    const int top = s.getY() + header + 6 + insertRow * 6 + 6;
    return { s.getX() + 7 + 24, top, row < 0 ? stripW - 14 - 24 - 10 : stripW - 14 - 24 - 10, s.getBottom() - 6 - 22 - 4 - top };
}

void Mixer::resized()
{
    auto r = getLocalBounds().reduced (6);
    auto in = r.removeFromRight (insertsW);
    insertsView.setBounds (in);
    inserts.setBounds (0, 0, in.getWidth() - 10, juce::jmax (in.getHeight(), inserts.preferredHeight() + 10));
}

int Mixer::stripAt (juce::Point<int> p) const
{
    if (stripBox (-1).contains (p))
        return -1;
    for (int i = 0; i < (int) page.rows.size(); ++i)
        if (stripBox (i).contains (p))
            return i;
    return -2;
}

void Mixer::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().reduced (6);
    auto stripsArea = r.withRight (stripBox (-1).getX() - 10);
    g.setColour (col::field);
    g.fillRect (stripsArea);
    g.setColour (col::outline);
    g.drawRect (stripsArea, 1);
    for (int i = 0; i < (int) page.rows.size(); ++i)
        paintStrip (g, stripBox (i), i);
    paintStrip (g, stripBox (-1), -1);
}

void Mixer::paintStrip (juce::Graphics& g, juce::Rectangle<int> box, int row)
{
    const bool isMaster = row < 0;
    const auto& s = isMaster ? page.master : page.rows[(size_t) row].settings;
    const auto colour = isMaster ? col::muted : stemColour (page.rows[(size_t) row].name);
    const bool selected = isMaster ? page.masterChosen : (! page.masterChosen && page.current == row);
    auto r = box.toFloat().reduced (0.5f);
    g.setColour (col::card);
    g.fillRoundedRectangle (r, 2.0f);
    g.setColour (colour.withAlpha (0.22f));
    g.fillRoundedRectangle (r.withHeight ((float) header), 2.0f);
    g.setColour (colour);
    g.fillRoundedRectangle (r.withHeight (3.0f), 2.0f);
    g.setColour (selected ? colour : col::grid);
    g.drawRoundedRectangle (selected ? r.reduced (0.5f) : r, 2.0f, selected ? 2.0f : 1.0f);
    g.setColour (col::text);
    g.setFont (uiFont (15.0f, true));
    g.drawText (isMaster ? aicore::ui::upper (aicore::ui::tr ("Master")) : page.rows[(size_t) row].name, juce::Rectangle<float> (r.getX() + 9, r.getY() + 3, r.getWidth() - 16, (float) header - 3),
                juce::Justification::centredLeft, true);
    g.setFont (uiFont (13.0f));
    int i = 0;
    for (auto& [name, on] : insertStates (s))
    {
        juce::Rectangle<float> line (r.getX() + 7, r.getY() + header + 6 + i * insertRow + 1, r.getWidth() - 14, insertRow - 2.0f);
        g.setColour (col::field);
        g.fillRoundedRectangle (line, 2.0f);
        g.setColour (on ? colour : col::dim);
        g.fillEllipse (line.getRight() - 12, line.getCentreY() - 3, 6, 6);
        g.setColour (on ? col::text : col::muted);
        g.drawText (aicore::ui::tr (name), line.withTrimmedLeft (6).withTrimmedRight (16), juce::Justification::centredLeft);
        ++i;
    }
    // the fader with its dB scale (and the meter on a track)
    auto fb = faderBox (row).toFloat();
    const double lo = isMaster ? -24 : -60, hi = 12;
    const float top = fb.getY() + 10, height = juce::jmax (20.0f, fb.getHeight() - 40);
    g.setColour (col::dim);
    g.setFont (uiFont (12.0f));
    for (int v : { 12, 6, 0, -12, -24, -36, -60 })
        if (lo <= v && v <= hi)
        {
            const float y = top + height - (float) ((v - lo) / (hi - lo)) * height;
            g.drawText (v > 0 ? "+" + juce::String (v) : juce::String (v), juce::Rectangle<float> (fb.getX() - 28, y - 6, 25, 12), juce::Justification::centredRight, false);
        }
    const float cx = fb.getX() + fb.getWidth() * (isMaster ? 0.5f : 0.45f);
    g.setColour (col::grid);
    g.fillRoundedRectangle (cx - 1.5f, top, 3.0f, height, 1.5f);
    const double gain = sv (s, "gainDb");
    const float y = top + height - (float) ((juce::jlimit (lo, hi, gain) - lo) / (hi - lo)) * height;
    g.setColour (col::level);
    g.fillRoundedRectangle (cx - 1.5f, y, 3.0f, top + height - y, 1.5f);
    juce::Rectangle<float> handle (cx - 12, y - 6, 24, 12);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xfff6f8fa), 0, handle.getY(), juce::Colour (0xffd5dce4), 0, handle.getBottom(), false));
    g.fillRoundedRectangle (handle, 2.0f);
    g.setColour (col::muted);
    g.drawRoundedRectangle (handle, 2.0f, 1.0f);
    g.drawLine (handle.getX() + 5, handle.getCentreY(), handle.getRight() - 5, handle.getCentreY(), 1.4f);
    if (! isMaster)
        drawMeterBar (g, juce::Rectangle<float> (fb.getRight() - 2.5f, top, 5, height), page.levelDb (row), true, -60.0f, 12.0f);
    // M, S and the value
    auto foot = box.toFloat().removeFromBottom (28).reduced (7, 3);
    if (! isMaster)
    {
        const auto& rw = page.rows[(size_t) row];
        drawToggle (g, foot.withWidth (26).withHeight (22), "M", rw.mute, col::muteOn);
        drawToggle (g, foot.withTrimmedLeft (29).withWidth (26).withHeight (22), "S", rw.solo, col::accent);
    }
    g.setColour (col::text);
    g.setFont (uiFont (14.0f));
    g.drawText ((gain <= -59.95 ? juce::String::fromUTF8 ("-\xe2\x88\x9e") : juce::String (gain, 1)) + " dB", foot, juce::Justification::centredRight);
}

void Mixer::mouseDown (const juce::MouseEvent& e)
{
    dragRow = -2;
    const int row = stripAt (e.getPosition());
    if (row == -2)
        return;
    if (row < 0)
        page.chooseMaster();
    else
        page.select (row);
    auto foot = stripBox (row).removeFromBottom (28).reduced (7, 3);
    if (row >= 0 && juce::Rectangle<int> (foot.getX(), foot.getY(), 26, 22).contains (e.getPosition()))
    {
        page.rows[(size_t) row].mute = ! page.rows[(size_t) row].mute;
        page.changed();
        return;
    }
    if (row >= 0 && juce::Rectangle<int> (foot.getX() + 29, foot.getY(), 26, 22).contains (e.getPosition()))
    {
        page.rows[(size_t) row].solo = ! page.rows[(size_t) row].solo;
        page.changed();
        return;
    }
    if (faderBox (row).expanded (4).contains (e.getPosition()))
    {
        dragRow = row;
        dragStart = sv (row < 0 ? page.master : page.rows[(size_t) row].settings, "gainDb");
        dragY = (float) e.y;
    }
}

void Mixer::mouseDrag (const juce::MouseEvent& e)
{
    if (dragRow == -2)
        return;
    const double lo = dragRow < 0 ? -24 : -60, hi = 12;
    const float height = juce::jmax (20.0f, faderBox (dragRow).getHeight() - 40.0f);
    const double v = juce::jlimit (lo, hi, std::round ((dragStart - (e.y - dragY) / height * (hi - lo)) * 10) / 10);
    (dragRow < 0 ? page.master : page.rows[(size_t) dragRow].settings)["gainDb"] = v;
    page.changed();
}

void Mixer::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int row = stripAt (e.getPosition());
    if (row != -2 && faderBox (row).expanded (4).contains (e.getPosition()))
    {
        (row < 0 ? page.master : page.rows[(size_t) row].settings)["gainDb"] = 0;
        page.changed();
    }
}

//==============================================================================
StemsPage::StemsPage (bool voice) : voiceOnly (voice)
{
    formats.registerBasicFormats();
    setWantsKeyboardFocus (true);
    master = masterDefaults();
    source.setFont (uiFont (15.0f));
    source.setIndents (8, 5);
    source.setTextToShowWhenEmpty (voice ? "Song to split into voice and instrumental" : "Song, excerpt or stem to separate", col::dim);
    folderButton.setTooltip ("Choose a song, excerpt or stem");
    folderButton.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser> ("Choose a song, excerpt or stem", juce::File (source.getText()));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc) {
            if (fc.getResult() != juce::File())
                source.setText (fc.getResult().getFullPathName());
        });
    };
    mode.addItemList ({ juce::String::fromUTF8 ("4 stems \xc2\xb7 htdemucs"), juce::String::fromUTF8 ("4 stems \xc2\xb7 htdemucs_ft"),
                        juce::String::fromUTF8 ("6 stems \xc2\xb7 htdemucs_6s") }, 1);
    mode.setSelectedItemIndex (0, juce::dontSendNotification);
    mode.setTooltip ("How to split the song: 4 parts (drums, bass, other, voice) or 6 parts.");
    mode.setColour (juce::ComboBox::backgroundColourId, col::field);
    mode.setColour (juce::ComboBox::textColourId, col::text);
    mode.setColour (juce::ComboBox::outlineColourId, col::outline);
    separateButton.onClick = [this] { separate(); };
    tracksButton.onClick = [this] { showView (0); };
    mixButton.onClick = [this] { showView (1); };
    dotsButton.setTooltip ("More");
    dotsButton.onClick = [this] { options(); };
    exportButton.setTooltip ("Choose the tracks, the folder and the format");
    exportButton.onClick = [this] { exportTracks(); };
    stopButton.onClick = [this] { worker.stop(); };
    for (juce::Component* c : { (juce::Component*) &source, (juce::Component*) &folderButton, (juce::Component*) &separateButton,
                                (juce::Component*) &tracksButton, (juce::Component*) &mixButton, (juce::Component*) &dotsButton,
                                (juce::Component*) &exportButton, (juce::Component*) &state, (juce::Component*) &lanesView, (juce::Component*) &empty })
        addAndMakeVisible (c);
    if (! voice)
        addAndMakeVisible (mode);
    addChildComponent (stopButton);
    addChildComponent (mixer);
    lanesView.setViewedComponent (&lanes, false);
    lanesView.setScrollBarsShown (true, false);
    state.setFont (uiFont (15.0f));
    state.setColour (juce::Label::textColourId, col::muted);
    state.setText ("Ready.", juce::dontSendNotification);
    empty.setFont (uiFont (15.0f));
    empty.setColour (juce::Label::textColourId, col::text);
    empty.setJustificationType (juce::Justification::centred);
    empty.setText (voice ? "Choose a song above and press Separate: the voice and the instrumental become two tracks here. MIX shows the mixer."
                         : "Choose a song above and press Separate: it is split into drums, bass, other sounds and voice, and each part becomes a track here. MIX shows the mixer.",
                   juce::dontSendNotification);
    empty.setInterceptsMouseClicks (false, false);

    worker.onEvent = [this] (const juce::var& v0) {
        auto v = v0["event"].toString() == "provider" ? v0["value"] : v0;
        if (v["event"].toString() == "progress")
        {
            auto text = v["stage"].toString().isNotEmpty() ? v["stage"].toString() : juce::String ("Running");
            const auto f = v["fraction"];
            if ((f.isDouble() || f.isInt()) && v["step"].isVoid())
            {
                const int pct = juce::jlimit (0, 100, juce::roundToInt ((double) f * 100));
                progress = pct > 0 ? pct / 100.0 : 0.0;
                if (pct > 0)
                    text += juce::String::fromUTF8 (" \xc2\xb7 ") + juce::String (pct) + "%";
            }
            if (! v["step"].isVoid())
                text += juce::String::fromUTF8 (" \xc2\xb7 step ") + v["step"].toString() + juce::String::fromUTF8 (" \xc2\xb7 loss ") + v["loss"].toString();
            state.setText (text, juce::dontSendNotification);
            repaint();
        }
    };
    worker.onError = [this] (const juce::String& m) { fail (m); };
    worker.onDone = [this] (const juce::var& r) { done (r); };
    worker.onFinished = [this] {
        stopButton.setVisible (false);
        progress = -1;
        resized();
        repaint();
    };
    showView (0);
}

StemsPage::~StemsPage()
{
    stopTimer();
    transport.stop();
    AudioOut::get().detach (&transport);
    transport.setSource (nullptr);
    peakPool.removeAllJobs (true, 5000);
}

double StemsPage::timeline() const
{
    double longest = 0;
    for (auto& r : rows)
        if (auto it = peaks.find (r.path); it != peaks.end() && it->second != nullptr)
            longest = juce::jmax (longest, it->second->seconds);
    return longest;
}

float StemsPage::levelDb (int r) const
{
    if (r < 0 || r >= (int) rows.size())
        return meterFloor;
    const auto& row = rows[(size_t) r];
    auto it = peaks.find (row.path);
    if (row.mute || it == peaks.end() || it->second == nullptr || it->second->numChannels == 0)
        return meterFloor;
    float loudest = 0;
    for (int ch = 0; ch < it->second->numChannels; ++ch)
        for (auto& c : it->second->channels[ch])
            loudest = juce::jmax (loudest, std::abs (c[0]), std::abs (c[1]));
    if (loudest <= 1e-6f)
        return meterFloor;
    return 20 * std::log10 (loudest) + (float) sv (row.settings, "gainDb");
}

void StemsPage::showView (int v)
{
    view = v;
    tracksButton.setToggleState (v == 0, juce::dontSendNotification);
    mixButton.setToggleState (v == 1, juce::dontSendNotification);
    lanesView.setVisible (v == 0);
    mixer.setVisible (v == 1);
    if (v == 1)
        mixer.refresh();
    resized();
    repaint();
}

void StemsPage::select (int r)
{
    current = r;
    masterChosen = false;
    if (r >= 0 && r < (int) rows.size() && rows[(size_t) r].role.isNotEmpty())
        vocal = rows[(size_t) r].role == "vocals" || rows[(size_t) r].role == "voice";
    mixer.refresh();
    lanes.repaint();
}

void StemsPage::chooseMaster()
{
    masterChosen = true;
    mixer.refresh();
}

void StemsPage::remember()
{
    undoStack.push_back (rows);
    if (undoStack.size() > 50)
        undoStack.erase (undoStack.begin());
    redoStack.clear();
}

void StemsPage::changed (bool rememberIt)
{
    if (rememberIt)
        remember();
    updatePlayer();
    saveSession();
    lanes.repaint();
    mixer.repaint();
    if (mixer.isVisible())
    {
        // the inserts page follows the values (a fader or M/S moved elsewhere)
    }
    if (onTransportChanged) onTransportChanged();
}

void StemsPage::setZone (Zone z)
{
    zone = z;
    lanes.repaint();
}

void StemsPage::loadPeaks (const juce::String& path)
{
    if (path.isEmpty() || peaks.count (path))
        return;
    peaks[path] = nullptr; // pending
    juce::Component::SafePointer<StemsPage> safe (this);
    peakPool.addJob (new PeakJob (path, [safe] (juce::String p, std::shared_ptr<Peaks> pk) {
                         if (safe == nullptr)
                             return;
                         safe->peaks[p] = pk;
                         safe->lanes.repaint();
                         safe->mixer.repaint();
                         if (safe->onTransportChanged) safe->onTransportChanged();
                     }),
                     true);
}

void StemsPage::addRows (const std::vector<StemRow>& fresh)
{
    for (auto r : fresh)
    {
        if (r.original.isEmpty())
            r.original = r.path;
        for (auto& [k, v] : trackDefaults())
            if (! r.settings.count (k))
                r.settings[k] = v;
        rows.push_back (r);
        loadPeaks (r.path);
    }
    if (current < 0 && ! rows.empty())
        select (0);
    empty.setVisible (rows.empty());
    updatePlayer();
    saveSession();
    resized();
    repaint();
}

void StemsPage::setRows (std::vector<StemRow> fresh)
{
    if (transport.isPlaying())
        togglePlay();
    zone = {};
    const int keep = current;
    rows.clear();
    current = -1;
    addRows (fresh);
    if (! rows.empty())
        select (juce::jlimit (0, (int) rows.size() - 1, keep));
    empty.setVisible (rows.empty());
    resized();
    repaint();
}

void StemsPage::importFiles (const juce::StringArray& paths)
{
    std::vector<StemRow> fresh;
    for (auto& p : paths)
    {
        StemRow r;
        r.name = juce::File (p).getFileNameWithoutExtension();
        r.path = p;
        fresh.push_back (r);
    }
    addRows (fresh);
}

juce::var StemsPage::rowsVar (const std::vector<StemRow>& list) const
{
    juce::Array<juce::var> out;
    for (auto& r : list)
    {
        auto* o = new juce::DynamicObject();
        if (auto* e = r.extra.getDynamicObject())
            for (auto& p : e->getProperties())
                o->setProperty (p.name, p.value);
        o->setProperty ("name", r.name);
        o->setProperty ("path", r.path);
        o->setProperty ("original", r.original);
        if (r.role.isNotEmpty()) o->setProperty ("role", r.role);
        if (r.sourceHash.isNotEmpty()) o->setProperty ("sourceHash", r.sourceHash);
        o->setProperty ("mute", r.mute);
        o->setProperty ("solo", r.solo);
        o->setProperty ("settings", settingsVar (r.settings));
        if (! r.versions.isEmpty())
        {
            juce::Array<juce::var> v;
            for (auto& s : r.versions)
                v.add (s);
            o->setProperty ("versions", v);
        }
        out.add (juce::var (o));
    }
    return out;
}

juce::var StemsPage::masterVar() const { return settingsVar (master); }

void StemsPage::saveSession()
{
    if (rows.empty())
        return;
    if (session == juce::File())
        session = juce::File (AppSettings::get ("workspaceRoot")).getChildFile ("sessions").getChildFile (juce::Uuid().toString().removeCharacters ("-")).getChildFile ("stems.json");
    session.getParentDirectory().createDirectory();
    auto* o = new juce::DynamicObject();
    o->setProperty ("schemaVersion", "stem-session-1");
    o->setProperty ("rows", rowsVar (rows));
    o->setProperty ("master", masterVar());
    auto tmp = session.getSiblingFile (session.getFileName() + ".tmp");
    if (tmp.replaceWithText (juce::JSON::toString (juce::var (o), false)))
        tmp.moveFileTo (session);
}

void StemsPage::openSession()
{
    chooser = std::make_unique<juce::FileChooser> ("Open stems session", juce::File(), "*.json");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc) {
        auto f = fc.getResult();
        if (f == juce::File())
            return;
        auto v = juce::JSON::parse (f);
        bool ok = v["schemaVersion"].toString() == "stem-session-1" && v["rows"].isArray();
        if (ok)
            for (auto& r : *v["rows"].getArray())
                ok = ok && juce::File (r["path"].toString()).existsAsFile();
        if (! ok)
        {
            fail ("Invalid session or missing audio.");
            return;
        }
        rows.clear();
        current = -1;
        session = f;
        transport.stop();
        master = masterDefaults();
        for (auto& [k, val] : settingsFromVar (v["master"]))
            master[k] = val;
        std::vector<StemRow> fresh;
        for (auto& r : *v["rows"].getArray())
        {
            StemRow row;
            row.name = r["name"].toString();
            row.path = r["path"].toString();
            row.original = r["original"].toString();
            row.role = r["role"].toString();
            row.sourceHash = r["sourceHash"].toString();
            row.mute = (bool) r["mute"];
            row.solo = (bool) r["solo"];
            row.settings = settingsFromVar (r["settings"]);
            if (auto* a = r["versions"].getArray())
                for (auto& s : *a)
                    row.versions.add (s.toString());
            row.extra = r;
            fresh.push_back (row);
        }
        addRows (fresh);
    });
}

void StemsPage::resetMaster()
{
    master = masterDefaults();
    changed();
    mixer.refresh();
}

std::vector<StemRow> StemsPage::heardRows() const
{
    std::vector<StemRow> out;
    for (auto& r : rows)
        if (! r.mute)
            out.push_back (r);
    bool anySolo = false;
    for (auto& r : out)
        anySolo = anySolo || r.solo;
    if (anySolo)
    {
        std::vector<StemRow> solo;
        for (auto& r : out)
            if (r.solo)
                solo.push_back (r);
        return solo;
    }
    return out;
}

//------------------------------------------------------------------------------ playing
void StemsPage::updatePlayer()
{
    // the stems and the master through their chains, as exported (tracks::Mixer: per-track chain + master)
    tracks::Session s;
    for (int i = 0; i < (int) rows.size(); ++i)
    {
        const auto& r = rows[(size_t) i];
        auto* clip = tracks::addClip (s, i, juce::File (r.path), 0.0);
        while ((int) s.tracks.size() <= i)
            tracks::addTrack (s);
        auto& t = s.tracks[(size_t) i];
        t.name = r.name;
        t.gainDb = sv (r.settings, "gainDb");
        t.fx = r.settings;
        t.fx.erase ("gainDb");
        t.mute = only >= 0 ? i != only : r.mute;
        t.solo = only >= 0 ? false : r.solo;
        if (clip != nullptr)
        {
            clip->fadeIn = sv (r.settings, "fadeInS");
            clip->fadeOut = sv (r.settings, "fadeOutS");
        }
    }
    s.master = master;
    mix.setSession (s);
}

void StemsPage::togglePlayKeep (bool keepOnly)
{
    if (! keepOnly && ! transport.isPlaying())
        only = -1;
    if (transport.isPlaying())
    {
        transport.stop();
        stopTimer();
        playhead = transport.getCurrentPosition();
        only = -1;
        updatePlayer();
        lanes.repaint();
        if (onTransportChanged) onTransportChanged();
        return;
    }
    if (rows.empty())
    {
        fail ("Import or separate tracks first.");
        return;
    }
    updatePlayer();
    mix.measureLufs();
    AudioOut::get().attach (&transport);
    transport.setSource (&mix, 0, nullptr, mix.rate());
    transport.setPosition (playhead);
    playFrom = playhead;
    transport.start();
    startTimer (40);
    lanes.repaint();
    if (onTransportChanged) onTransportChanged();
}

void StemsPage::stop()
{
    if (transport.isPlaying())
        togglePlay();
    seek (playFrom);
}

void StemsPage::seek (double seconds)
{
    playhead = juce::jlimit (0.0, juce::jmax (0.0, timeline()), seconds);
    if (transport.isPlaying())
        transport.setPosition (playhead);
    lanes.repaint();
    if (onTransportChanged) onTransportChanged();
}

void StemsPage::listenStem (int r)
{
    if (transport.isPlaying() && only == r)
    {
        togglePlay();
        return;
    }
    only = r;
    if (transport.isPlaying())
    {
        updatePlayer();
        lanes.repaint();
        return;
    }
    togglePlayKeep (true);
}

void StemsPage::timerCallback()
{
    if (! transport.isPlaying())
    {
        stopTimer();
        only = -1;
        updatePlayer();
        lanes.repaint();
        if (onTransportChanged) onTransportChanged();
        return;
    }
    playhead = transport.getCurrentPosition();
    lanes.repaint();
    if (onTransportChanged) onTransportChanged();
}

juce::String StemsPage::clock() const
{
    return clockText (transport.isPlaying() ? transport.getCurrentPosition() : playhead) + " / " + clockText (timeline());
}

void StemsPage::visibilityChanged()
{
    if (! isVisible() && transport.isPlaying())
        togglePlay(); // leaving the page stops the sound
}

//------------------------------------------------------------------------------ jobs
void StemsPage::startTask (const juce::String& kind, juce::var fields)
{
    auto error = worker.start (kind, fields);
    if (error.isNotEmpty())
    {
        fail (error);
        return;
    }
    state.setText (juce::String::fromUTF8 ("Running\xe2\x80\xa6"), juce::dontSendNotification);
    progress = -1;
    stopButton.setVisible (true);
    resized();
}

void StemsPage::separate()
{
    activeKind = "separate";
    auto* o = new juce::DynamicObject();
    o->setProperty ("source", source.getText().trim());
    const juce::String models[] = { "htdemucs", "htdemucs_ft", "htdemucs_6s" };
    o->setProperty ("model", voiceOnly ? juce::String ("htdemucs") : models[juce::jlimit (0, 2, mode.getSelectedItemIndex())]);
    o->setProperty ("voice", voiceOnly);
    startTask ("separate", juce::var (o));
}

void StemsPage::done (const juce::var& result)
{
    if (result["stems"].isArray())
    {
        // separating the same song again replaces its tracks, never doubles them
        std::set<juce::String> fresh;
        for (auto& x : *result["stems"].getArray())
            fresh.insert (x["sourceHash"].toString() + "|" + (x["role"].toString().isNotEmpty() ? x["role"].toString() : x["name"].toString()));
        std::vector<StemRow> keep;
        for (auto& r : rows)
            if (! fresh.count (r.sourceHash + "|" + (r.role.isNotEmpty() ? r.role : r.name)))
                keep.push_back (r);
        if (keep.size() != rows.size())
        {
            remember();
            setRows (keep);
        }
        std::vector<StemRow> add;
        for (auto& x : *result["stems"].getArray())
        {
            StemRow r;
            r.name = x["name"].toString();
            r.path = x["path"].toString();
            r.role = x["role"].toString();
            r.sourceHash = x["sourceHash"].toString();
            r.extra = x;
            add.push_back (r);
        }
        addRows (add);
        state.setText ({}, juce::dontSendNotification);
    }
    else if (result["candidate"].isString())
    {
        candidate = result;
        state.setText ("Alternative ready; listen A/B before using it.", juce::dontSendNotification);
    }
    else if (result["files"].isArray())
    {
        auto files = *result["files"].getArray();
        juce::String text = result["file"].isString() ? "Exported: " + result["file"].toString()
                                                       : "Export saved in " + juce::File (files[0].toString()).getParentDirectory().getFullPathName();
        auto m = result["metrics"];
        if (! m["integratedLUFS"].isVoid())
            text += juce::String::fromUTF8 (" \xc2\xb7 ") + (files.size() > 1 ? "mix" : "track") + juce::String (": ") + juce::String ((double) m["integratedLUFS"], 1) + " LUFS";
        if (! m["samplePeakDbFS"].isVoid())
            text += juce::String::fromUTF8 (" \xc2\xb7 peak ") + juce::String ((double) m["samplePeakDbFS"], 1) + " dBFS";
        state.setText (text, juce::dontSendNotification);
        if (activeKind == "preview")
            juce::File (files[0].toString()).startAsProcess();
        else if (activeKind == "training-export" && onTrainingAudio)
            onTrainingAudio (files[0].toString());
    }
}

void StemsPage::exportTracks()
{
    if (rows.empty())
    {
        fail ("Import or separate tracks first.");
        return;
    }
    auto heard = heardRows();
    if (heard.empty())
    {
        fail ("Every track is muted: nothing to export.");
        return;
    }
    juce::StringArray names;
    for (auto& r : heard)
        names.add (r.name);
    auto name = heard.size() == 1 ? heard[0].name : (juce::File (source.getText()).getFileNameWithoutExtension().isNotEmpty() ? juce::File (source.getText()).getFileNameWithoutExtension() : juce::String ("mix"));
    auto folder = lastExportFolder != juce::File() ? lastExportFolder : juce::File (AppSettings::get ("workspaceRoot"));
    auto* dlg = new ExportTracksDialog (names, name, folder, lastChoice);
    juce::Component::SafePointer<StemsPage> safe (this);
    dlg->onExport = [safe, heard, name] (juce::String n, juce::File f, sample::ExportChoice c) {
        if (safe == nullptr)
            return;
        auto run = [safe, heard, name, n, f, c] {
            if (safe == nullptr)
                return;
            const auto ext = "." + c.format;
            auto target = f.getChildFile ((n.isNotEmpty() ? n : name) + ext);
            safe->lastExportFolder = f;
            safe->lastChoice = c;
            safe->activeKind = "export";
            safe->saveSession();
            std::vector<StemRow> rws = heard;
            for (auto& r : rws)
                r.mute = r.solo = false;
            auto* o = new juce::DynamicObject();
            o->setProperty ("file", target.getFullPathName());
            o->setProperty ("mixdown", rws.size() > 1);
            o->setProperty ("rows", safe->rowsVar (rws));
            o->setProperty ("mix", false);
            o->setProperty ("subtype", "FLOAT");
            auto* ch = new juce::DynamicObject();
            ch->setProperty ("format", c.format);
            ch->setProperty ("bits", c.bits);
            ch->setProperty ("rate", c.rate);
            ch->setProperty ("mono", c.mono);
            o->setProperty ("choice", juce::var (ch));
            o->setProperty ("master", safe->masterVar());
            safe->startTask ("export", juce::var (o));
        };
        const auto target = f.getChildFile ((n.isNotEmpty() ? n : name) + "." + c.format);
        if (target.exists())
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Export", "Replace these files?\n" + target.getFileName(), "Yes", "No", nullptr,
                                                juce::ModalCallbackFunction::create ([run] (int r) { if (r == 1) run(); }));
        else
            run();
    };
    launch (dlg, "Export");
}

//------------------------------------------------------------------------------ zone edits and tracks
bool StemsPage::zoneAudio (int& r, sample::Audio& audio, double& rate, int& a, int& b)
{
    if (zone.row < 0 || zone.t1 <= zone.t0)
    {
        fail ("Drag on a wave to choose a zone first.");
        return false;
    }
    r = zone.row;
    juce::String error;
    if (! sample::load (juce::File (rows[(size_t) r].path), audio, rate, error))
    {
        fail (error);
        return false;
    }
    a = juce::jmax (0, (int) (zone.t0 * rate));
    b = juce::jmin (audio.getNumSamples(), (int) (zone.t1 * rate));
    return true;
}

void StemsPage::writeEdit (int r, const sample::Audio& audio, double rate, const juce::String& note)
{
    saveSession();
    auto folder = session.getParentDirectory().getChildFile ("edits");
    folder.createDirectory();
    auto target = folder.getChildFile (rows[(size_t) r].name + "-" + juce::Uuid().toString().substring (0, 8) + ".wav");
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (target);
        juce::WavAudioFormat wav;
        auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}.withSampleRate (rate).withNumChannels (2).withBitsPerSample (32)
                                                  .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (w == nullptr || ! w->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples()))
        {
            fail ("Could not write: " + target.getFullPathName());
            return;
        }
    }
    remember();
    auto copy = rows;
    copy[(size_t) r].path = target.getFullPathName();
    setRows (copy);
    select (r);
    saveSession();
    state.setText (note + " Ctrl+Z brings it back.", juce::dontSendNotification);
}

void StemsPage::copyZone()
{
    int r, a, b;
    double rate;
    sample::Audio audio;
    if (! zoneAudio (r, audio, rate, a, b))
        return;
    clipboard = std::make_shared<sample::Audio> (sample::range (audio, a, b));
    clipboardRate = rate;
    state.setText ("Copied: " + juce::String ((b - a) / rate, 2) + " s", juce::dontSendNotification);
}

void StemsPage::cutZone()
{
    int r, a, b;
    double rate;
    sample::Audio audio;
    if (! zoneAudio (r, audio, rate, a, b))
        return;
    clipboard = std::make_shared<sample::Audio> (sample::range (audio, a, b));
    clipboardRate = rate;
    audio.clear (a, b - a);
    writeEdit (r, audio, rate, "Cut: " + juce::String ((b - a) / rate, 2) + " s.");
}

void StemsPage::deleteZone()
{
    int r, a, b;
    double rate;
    sample::Audio audio;
    if (! zoneAudio (r, audio, rate, a, b))
        return;
    audio.clear (a, b - a);
    writeEdit (r, audio, rate, "Zone deleted.");
}

void StemsPage::paste()
{
    if (clipboard == nullptr)
    {
        fail ("Copy or cut a zone first.");
        return;
    }
    if (current < 0 || current >= (int) rows.size())
    {
        fail ("Select a track.");
        return;
    }
    sample::Audio audio;
    double rate = 0;
    juce::String error;
    if (! sample::load (juce::File (rows[(size_t) current].path), audio, rate, error))
    {
        fail (error);
        return;
    }
    if (rate != clipboardRate)
    {
        fail ("The copied audio has another sample rate.");
        return;
    }
    const int at = (int) ((zone.row == current ? zone.t0 : playhead) * rate);
    if (at >= audio.getNumSamples())
    {
        fail ("The playhead is after the end of the track.");
        return;
    }
    const int n = juce::jmin (clipboard->getNumSamples(), audio.getNumSamples() - at);
    for (int ch = 0; ch < 2; ++ch)
        audio.copyFrom (ch, at, *clipboard, ch, 0, n);
    writeEdit (current, audio, rate, "Pasted: " + juce::String (n / rate, 2) + " s.");
}

void StemsPage::deleteTrack()
{
    if (current < 0 || current >= (int) rows.size())
        return;
    const auto name = rows[(size_t) current].name;
    remember();
    auto copy = rows;
    copy.erase (copy.begin() + current);
    setRows (copy);
    saveSession();
    state.setText ("Track deleted: " + name + ". Ctrl+Z brings it back.", juce::dontSendNotification);
}

void StemsPage::renameTrack()
{
    if (current < 0 || current >= (int) rows.size())
        return;
    auto* w = new juce::AlertWindow ("Rename track", "Name:", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", rows[(size_t) current].name);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<StemsPage> safe (this);
    const int r = current;
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w, r] (int res) {
                            const auto text = w->getTextEditorContents ("name").trim();
                            if (safe == nullptr || res != 1 || text.isEmpty() || r >= (int) safe->rows.size() || text == safe->rows[(size_t) r].name)
                                return;
                            safe->remember();
                            auto copy = safe->rows;
                            copy[(size_t) r].name = text;
                            safe->setRows (copy);
                            safe->saveSession();
                        }),
                        true);
}

void StemsPage::undo()
{
    if (undoStack.empty())
        return;
    redoStack.push_back (rows);
    auto back = undoStack.back();
    undoStack.pop_back();
    setRows (back);
    saveSession();
}

void StemsPage::redo()
{
    if (redoStack.empty())
        return;
    undoStack.push_back (rows);
    auto back = redoStack.back();
    redoStack.pop_back();
    setRows (back);
    saveSession();
}

void StemsPage::enhance()
{
    if (current < 0 || current >= (int) rows.size())
    {
        fail ("Select a track.");
        return;
    }
    auto* w = new juce::AlertWindow ("Make an alternative with ACE", "What ACE should improve in this stem:", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("prompt", prompt);
    w->addButton ("OK", 1);
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<StemsPage> safe (this);
    const int r = current;
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w, r] (int res) {
                            if (safe == nullptr || res != 1)
                                return;
                            safe->prompt = w->getTextEditorContents ("prompt");
                            safe->enhancedRow = r;
                            safe->activeKind = "enhance";
                            auto* o = new juce::DynamicObject();
                            o->setProperty ("source", safe->rows[(size_t) r].path);
                            o->setProperty ("prompt", safe->prompt);
                            o->setProperty ("instrumental", ! safe->vocal);
                            safe->startTask ("enhance", juce::var (o));
                        }),
                        true);
}

void StemsPage::applyCandidate()
{
    if (! candidate["candidate"].isString() || enhancedRow < 0 || enhancedRow >= (int) rows.size())
    {
        fail ("Make an alternative first.");
        return;
    }
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> a (fm.createReaderFor (juce::File (rows[(size_t) enhancedRow].path)));
    std::unique_ptr<juce::AudioFormatReader> b (fm.createReaderFor (juce::File (candidate["candidate"].toString())));
    if (a == nullptr || b == nullptr || a->sampleRate != b->sampleRate || a->lengthInSamples != b->lengthInSamples)
    {
        fail ("The alternative changed rate or length. Not applied: it needs explicit alignment.");
        return;
    }
    auto& row = rows[(size_t) enhancedRow];
    row.versions.add (row.path);
    row.path = candidate["candidate"].toString();
    loadPeaks (row.path);
    saveSession();
    state.setText ("Alternative in use; original and versions kept.", juce::dontSendNotification);
    candidate = juce::var();
    updatePlayer();
    repaint();
}

void StemsPage::options()
{
    const bool has = current >= 0 && current < (int) rows.size();
    const bool hasZone = zone.row >= 0 && zone.t1 > zone.t0;
    juce::PopupMenu m;
    m.addItem (has ? rows[(size_t) current].name : juce::String ("No track selected"), false, false, [] {});
    m.addSeparator();
    m.addItem ("Listen to the original", has, false, [this] { juce::File (rows[(size_t) current].path).startAsProcess(); });
    auto exportOne = [this] (const juce::String& kind) {
        activeKind = kind;
        std::vector<StemRow> one { rows[(size_t) current] };
        auto* o = new juce::DynamicObject();
        o->setProperty ("rows", rowsVar (one));
        o->setProperty ("mix", false);
        o->setProperty ("subtype", "FLOAT");
        startTask ("export", juce::var (o));
    };
    m.addItem ("Listen to the processed preview", has, false, [exportOne] { exportOne ("preview"); });
    m.addItem ("Send the processed track to training", has, false, [exportOne] { exportOne ("training-export"); });
    m.addItem ("Cut\tCtrl+X", hasZone, false, [this] { cutZone(); });
    m.addItem ("Copy\tCtrl+C", hasZone, false, [this] { copyZone(); });
    m.addItem ("Paste\tCtrl+V", has && clipboard != nullptr, false, [this] { paste(); });
    m.addItem ("Delete the zone\tDel", hasZone, false, [this] { deleteZone(); });
    m.addSeparator();
    m.addItem (juce::String::fromUTF8 ("Rename track\xe2\x80\xa6"), has, false, [this] { renameTrack(); });
    m.addItem ("Delete track", has, false, [this] { deleteTrack(); });
    m.addSeparator();
    m.addItem ("Edit this track as a sample", has && onEditAsSample != nullptr, false, [this] { onEditAsSample (rows[(size_t) current].path); });
    m.addItem ("Open every stem in Edit > Tracks", ! rows.empty() && onOpenInTracks != nullptr, false, [this] {
        juce::StringArray paths;
        for (auto& r : rows)
            paths.add (r.path);
        onOpenInTracks (paths);
    });
    m.addSeparator();
    m.addItem (juce::String::fromUTF8 ("Make an alternative with ACE\xe2\x80\xa6"), has, false, [this] { enhance(); });
    m.addItem ("This stem is a voice", true, vocal, [this] { vocal = ! vocal; });
    m.addItem ("Listen to the alternative", candidate["candidate"].isString(), false, [this] { juce::File (candidate["candidate"].toString()).startAsProcess(); });
    m.addItem ("Use the aligned alternative", candidate["candidate"].isString(), false, [this] { applyCandidate(); });
    m.addSeparator();
    m.addItem ("Undo\tCtrl+Z", ! undoStack.empty(), false, [this] { undo(); });
    m.addItem ("Redo\tCtrl+Y", ! redoStack.empty(), false, [this] { redo(); });
    m.addSeparator();
    m.addItem (juce::String::fromUTF8 ("Import stems\xe2\x80\xa6"), [this] {
        chooser = std::make_unique<juce::FileChooser> ("Import stems", juce::File(), "*.wav;*.flac;*.mp3");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this] (const juce::FileChooser& fc) {
                                  juce::StringArray paths;
                                  for (auto& f : fc.getResults())
                                      paths.add (f.getFullPathName());
                                  if (! paths.isEmpty())
                                      importFiles (paths);
                              });
    });
    m.addItem (juce::String::fromUTF8 ("Open a session\xe2\x80\xa6"), [this] { openSession(); });
    m.addItem ("Reset mastering", [this] { resetMaster(); });
    m.addItem (juce::String::fromUTF8 ("Export\xe2\x80\xa6"), ! rows.empty(), false, [this] { exportTracks(); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&dotsButton));
}

bool StemsPage::keyPressed (const juce::KeyPress& k)
{
    const bool cmd = k.getModifiers().isCommandDown();
    if (ui::Keys::matches (k, "toggle_play")) { togglePlay(); return true; }
    if (ui::Keys::matches (k, "go_start")) { seek (0); return true; }
    if (ui::Keys::matches (k, "undo")) { undo(); return true; }
    if (ui::Keys::matches (k, "redo") || (cmd && k.getKeyCode() == 'Y')) { redo(); return true; }
    if (ui::Keys::matches (k, "cut")) { cutZone(); return true; }
    if (ui::Keys::matches (k, "copy")) { copyZone(); return true; }
    if (ui::Keys::matches (k, "paste")) { paste(); return true; }
    if (ui::Keys::matches (k, "export")) { exportTracks(); return true; }
    if (ui::Keys::matches (k, "delete_selection"))
    {
        if (zone.row >= 0)
            deleteZone();
        else
            deleteTrack();
        return true;
    }
    return false;
}

//------------------------------------------------------------------------------ layout
void StemsPage::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().withTrimmedTop (-2).toFloat();
    g.setGradientFill (juce::ColourGradient (col::centreTop, 0, area.getY(), col::centreBottom, 0, area.getBottom(), false));
    g.fillRoundedRectangle (area, 2.0f);
    g.setColour (col::outline);
    g.drawRoundedRectangle (area.reduced (0.5f), 2.0f, 1.0f);
    g.setColour (col::cardEdge);
    g.fillRect (area.getX() + 1, area.getBottom() - 2, area.getWidth() - 2, 2.0f);
    auto body = (view == 0 ? lanesView.getBounds() : mixer.getBounds()).toFloat();
    g.setColour (col::field);
    g.fillRect (body);
    g.setColour (col::outline);
    g.drawRect (body.expanded (1), 1.0f);
    if (progress >= 0 && stopButton.isVisible())
    {
        auto bar = state.getBounds().toFloat().removeFromBottom (3).translated (0, 4);
        g.setColour (col::grid);
        g.fillRect (bar);
        g.setColour (col::accent);
        g.fillRect (bar.withWidth (bar.getWidth() * (float) progress));
    }
}

void StemsPage::resized()
{
    auto r = getLocalBounds().reduced (8, 0).withTrimmedTop (6).withTrimmedBottom (8);
    auto top = r.removeFromTop (28);
    separateButton.setBounds (top.removeFromRight (separateButton.idealWidth (26)));
    top.removeFromRight (6);
    if (! voiceOnly)
    {
        mode.setBounds (top.removeFromRight (210));
        top.removeFromRight (6);
    }
    folderButton.setBounds (top.removeFromRight (36));
    top.removeFromRight (6);
    source.setBounds (top);
    r.removeFromTop (8);
    auto bar = r.removeFromTop (28);
    tracksButton.setBounds (bar.removeFromLeft (tracksButton.idealWidth (30)));
    bar.removeFromLeft (4);
    mixButton.setBounds (bar.removeFromLeft (mixButton.idealWidth (30)));
    dotsButton.setBounds (bar.removeFromRight (36));
    r.removeFromTop (6);
    auto foot = r.removeFromBottom (30);
    exportButton.setBounds (foot.removeFromRight (exportButton.idealWidth (26)));
    foot.removeFromRight (6);
    if (stopButton.isVisible())
    {
        stopButton.setBounds (foot.removeFromRight (stopButton.idealWidth (26)));
        foot.removeFromRight (6);
    }
    state.setBounds (foot.withTrimmedLeft (14));
    r.removeFromBottom (6);
    lanesView.setBounds (r.reduced (1));
    mixer.setBounds (r.reduced (1));
    empty.setBounds (r);
    const int n = (int) rows.size();
    const int h = n == 0 ? r.getHeight() - 2
                         : Lanes::rulerH + n * juce::jmax (Lanes::rowMin, (lanesView.getHeight() - Lanes::rulerH - 2) / juce::jmax (1, n)) + 2;
    lanes.setBounds (0, 0, lanesView.getWidth() - (h > lanesView.getHeight() ? lanesView.getScrollBarThickness() : 0), juce::jmax (h, lanesView.getHeight()));
    empty.setVisible (rows.empty() && view == 0);
}
} // namespace aicore::stems
