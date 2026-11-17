// AICore-C — EDIT > SAMPLE: the edits, export and save of the old AICORE's sample_edit.py, on juce::AudioBuffer
// (always 2 channels, as read_audio gives them). Every edit makes a new buffer, so Undo is the previous one.
// Positions are frames. The source file is never changed.
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace aicore::sample
{
using Audio = juce::AudioBuffer<float>;

struct Role { const char* key; const char* folder; };
extern const Role kRoles[19];
juce::String roleFolder (const juce::String& key);
constexpr const char* kEditFolder = "AICORE edits";

/** Reads a file (WAV, FLAC, MP3, OGG, AIFF) as stereo float; the error goes to `error`. */
bool load (const juce::File&, Audio& out, double& rate, juce::String& error);

std::pair<int, int> clampRange (const Audio&, int start, int end);
int zeroCross (const Audio&, int frame, double rate, double windowMs = 5.0);
Audio keep (const Audio&, int start, int end);
Audio cut (const Audio&, int start, int end);
Audio gainDb (const Audio&, int start, int end, double db);
Audio normalize (const Audio&, int start, int end, double peakDb = -1.0);
Audio reverse (const Audio&, int start, int end);
Audio silence (const Audio&, int start, int end);
Audio trimSilence (const Audio&, double rate, double thresholdDb = -60.0);
Audio declick (const Audio&, double rate, double ms = 3.0);
Audio insert (const Audio&, int at, const Audio& clip);
Audio range (const Audio&, int start, int end);

/** A rise from 0 to 1 over n frames: smooth (S), linear, exponential (x^3). */
std::vector<float> fadeCurve (int n, const juce::String& curve);
std::pair<int, int> fadeLengths (int frames, int fadeIn, int fadeOut);
Audio applyFades (const Audio&, int fadeIn, int fadeOut, const juce::String& curve);
int barsFrames (double bpm, double bars, double rate, juce::String& error);

juce::String cleanName (const juce::String&);
juce::String fileName (const juce::String& name, const juce::String& role, const juce::String& kind, double bpm, const juce::String& key);

struct ExportChoice
{
    juce::String format = "wav", bits = "24"; // wav 16/24/32f, flac 16/24, mp3 ("" = 320 kbps)
    int rate = 0;                              // 0 = the sound's own rate
    bool mono = false;
};
struct ExportResult { juce::File file; double seconds = 0; int rate = 0, channels = 0; juce::String format, bits; };
/** WAV / FLAC / MP3 (MP3 through AICore's own Python and soundfile). Integer formats never go over 0 dBFS;
    16-bit gets TPDF dither. */
bool exportAudio (const Audio&, double rate, const juce::File& path, const ExportChoice&, ExportResult& out, juce::String& error);

struct SaveResult { juce::File file; juce::String sha256; double seconds = 0; };
/** Into <samples>\AICORE edits\<Role>\One-shots|Loops, WAV 24-bit, ends declicked, never over an existing name;
    a line in edits.jsonl. */
bool saveSample (const Audio&, double rate, const juce::File& root, const juce::String& name, const juce::String& role,
                 const juce::String& kind, double bpm, const juce::String& key, const juce::String& source,
                 SaveResult& out, juce::String& error);
} // namespace aicore::sample
