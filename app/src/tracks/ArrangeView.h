// AICore-C — EDIT > TRACKS: the tracks and their clips, a port of the old AICORE's ArrangeView
// (multitrack.py). Same geometry (HEAD_W 330, RULER_H 48, rows 56..140), same painting, same mouse rules:
// drag the name strip to move, the edges to trim, the top squares for fades, the wave to choose a zone,
// Alt = no magnet, Ctrl+wheel zoom.
#pragma once

#include "Arrange.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <optional>
#include <set>

namespace aicore::tracks
{
class ArrangeView : public juce::Component, public juce::FileDragAndDropTarget, private juce::Thread
{
public:
    static constexpr int headW = 330, rulerH = 48, trackH = 96, rowMin = 56, rowMax = 140, edgePx = 7, fadeBox = 9,
                         nameH = 24, snapPx = 10;

    ArrangeView();
    ~ArrangeView() override;

    Session session;
    std::set<int> selected;
    int currentTrack = 0;
    double playhead = 0, pps = 40, scrollX = 0;
    int scrollY = 0, rowH = trackH;
    bool snapping = true;
    std::vector<float> levels;
    struct Zone { int lo, hi; double t0, t1; };
    std::optional<Zone> zone;

    std::function<void (const Session& before)> onEdited; // one undo step
    std::function<void (double)> onSeek;
    std::function<void()> onSelectionChanged, onViewChanged;
    std::function<void (const juce::StringArray&, int track, double at)> onFilesDropped;
    std::function<void (int track)> onFx;
    std::function<void (const juce::MouseEvent&, int track, Clip*)> onContextMenu;

    double xOf (double s) const { return headW + (s - scrollX) * pps; }
    double secondsAt (double x) const { return juce::jmax (0.0, scrollX + (x - headW) / pps); }
    int trackTop (int i) const { return rulerH + i * rowH - scrollY; }
    int trackAt (float y) const;
    juce::Rectangle<float> clipRect (int ti, const Clip& c) const;
    Clip* clipAt (juce::Point<float> p, int& ti);
    double visibleSeconds() const { return juce::jmax (0.1, (getWidth() - headW) / pps); }
    void setView (std::optional<double> scroll = {}, std::optional<double> newPps = {});
    void zoom (double factor, std::optional<float> atX = {});
    void zoomAll();
    void setSession (const Session&);
    void setPlayhead (double s, bool follow = false);
    void renameTrack (int ti);

    void paint (juce::Graphics&) override;
    void resized() override { setView(); }
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

private:
    struct HeadRects { juce::Rectangle<float> fx, m, s, vol, meter; };
    HeadRects headRects (int ti) const;
    void paintRuler (juce::Graphics&, double beat, double bar, int every, int first, int last);
    void paintHead (juce::Graphics&, int ti, const Track&);
    void paintMeter (juce::Graphics&, juce::Rectangle<float>, float peak);
    void paintClip (juce::Graphics&, int ti, const Clip&, juce::Colour, bool silent);
    void paintZone (juce::Graphics&);
    juce::String hit (int ti, const Clip&, juce::Point<float>) const;
    std::pair<double, double> fadeXs (juce::Rectangle<float>, const Clip&) const;
    bool snapOn (const juce::ModifierKeys&) const;
    double snapTime (double s, const juce::ModifierKeys&) const;
    void startZone (int ti, juce::Point<float>, const juce::ModifierKeys&);
    void setVolume (int ti, float x);
    int contentHeight() const { return rulerH + (int) session.tracks.size() * rowH + 60; }

    struct Drag
    {
        juce::String kind;
        float x = 0;
        int grab = 0, grabTrack = 0, track = 0;
        std::vector<int> ids;
        Session base;
        bool moved = false, free = false;
        double at = 0;
        std::optional<double> guide;
    };
    std::optional<Drag> drag;
    int hoverClip = 0;

    // peaks: per file, (seconds, lows, highs, rms) computed on a background thread
    struct Peaks { double seconds = 0; std::vector<float> lo, hi, rms; bool ready = false; };
    std::map<juce::String, Peaks> peaks;
    juce::StringArray peakQueue;
    juce::CriticalSection peakLock;
    void wantPeaks (const juce::String& path);
    void run() override;
};

juce::Colour trackColour (const juce::String& name, int index);
} // namespace aicore::tracks
