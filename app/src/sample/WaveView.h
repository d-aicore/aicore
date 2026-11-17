// AICore-C — EDIT > SAMPLE: the wave (sample_editor.py WaveView) and the small overview above it (Overview).
// Drag to select, drag an edge to move it, the fade handles at the top, Ctrl+wheel zooms, the wheel scrolls.
#pragma once

#include "SampleEdit.h"
#include "../ui/AicoreTheme.h"

namespace aicore::sample
{
class WaveView : public juce::Component
{
public:
    WaveView();
    void setAudio (std::shared_ptr<const Audio> audio, double rate, bool keepView = false);
    const Audio* audio() const { return audio_.get(); }
    std::shared_ptr<const Audio> audioPtr() const { return audio_; }
    double rate() const { return rate_; }
    int frames() const { return audio_ == nullptr ? 0 : audio_->getNumSamples(); }

    std::pair<int, int> selection() const; // the selection, or the whole sound
    bool hasSelection() const { return sel.second > sel.first; }
    void setSelection (int a, int b);
    std::pair<int, int> sel { 0, 0 };

    std::pair<int, int> fadeFrames() const;
    void setFades (std::optional<double> in, std::optional<double> out, std::optional<juce::String> curve = {});
    double fadeIn = 0, fadeOut = 0; // seconds
    juce::String curve = "smooth";
    /** The part that plays and saves, with its fades; first = its first frame. */
    Audio rendered (int& first) const;

    void setView (double start, double span);
    void zoom (double factor, std::optional<float> atX = {});
    void zoomToSelection();
    void zoomAll() { setView (0, frames()); }
    double viewStart = 0, viewSpan = 1;
    int playhead = -1;
    bool snap = true;
    int serial = 0;

    // the 256-frame stats (also the overview's)
    static constexpr int block = 256;
    std::vector<float> lows[2], highs[2];
    std::vector<double> sums[2];
    bool twoLanes = false;

    std::function<void()> onSelectionChanged, onFadesChanged, onViewChanged;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    static constexpr int ruler = 24, handle = 7;

private:
    float xOf (double frame) const { return (float) ((frame - viewStart) / viewSpan * getWidth()); }
    int frameOf (float x) const { return (int) std::round (viewStart + x / juce::jmax (1, getWidth()) * viewSpan); }
    int snapped (int f) const { return snap && audio_ != nullptr ? zeroCross (*audio_, f, rate_) : f; }
    std::pair<float, float> handles() const;
    juce::String nearHandle (juce::Point<float>) const;
    void paintRuler (juce::Graphics&);
    void paintLane (juce::Graphics&, int ch, juce::Rectangle<float>);
    void paintFades (juce::Graphics&, juce::Rectangle<float>);
    void computeStats();

    std::shared_ptr<const Audio> audio_;
    double rate_ = 44100;
    enum class Drag { none, fade, edge, fresh } drag = Drag::none;
    juce::String dragHandle;
    int anchor = 0;
};

class Overview : public juce::Component
{
public:
    explicit Overview (WaveView&);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { drag = {}; }
    void mouseDoubleClick (const juce::MouseEvent&) override { wave.zoomAll(); }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override { wave.mouseWheelMove (e, w); }

private:
    float xOf (double f) const { return (float) (f / juce::jmax (1, wave.frames()) * getWidth()); }
    double frameOf (float x) const { return x / juce::jmax (1, getWidth()) * wave.frames(); }
    juce::String edge (float x) const;
    WaveView& wave;
    juce::String drag;
    double offset = 0;
    std::vector<float> cache;
    std::pair<int, int> key { -1, -1 };
};
} // namespace aicore::sample
