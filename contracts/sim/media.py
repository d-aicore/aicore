"""WAV / MIDI I/O, sha256, atomic staging+rename — stdlib only (S1 simulator).

WAV reading follows core_stems.read_wav (RIFF chunk walk, tags 1/3/0xFFFE,
PCM 16/24/32 + float 32/64). WAV writing is canonical PCM16 (fixtures) and
float32 (stems, the core_stems output model). MIDI is written by hand as SMF
type 1 with **PPQ 960** — ruling S1b: the Core DAW stores its templates at
ppq 960, so the service emits 960 natively and no conversion step exists
(480 was the S1 value, inherited from midi_maker.PPQ). The quantization grid
goes down to 1/32 (= 120 ticks at 960), which is where psytrance/hitech rolls
live. Note durations come from the detected hit length, never from the grid,
so notes shorter than the grid survive.
Every published file goes through staging + os.replace (atomic). Caps per format (CONTRACT.md §6,
media-format.schema.json): WAV <= MAX_WAV_BYTES = 128 MiB, MIDI <= MAX_MIDI_BYTES = 8 MiB, applied to
the size before any sample is decoded (audit 2026-11-16: the simulator used 8 MiB for WAV too).
A chunk that claims more bytes than the file holds is refused, never read as shorter audio.
"""
from __future__ import annotations

import hashlib
import json
import math
import os
import struct
from pathlib import Path

# Ruling S1b: PPQ 960 (== Core templates). Anything else is a broken contract.
PPQ = 960
# Quantization ladder, in PPQ quarters. "off" = 1 tick (no quantization).
GRID_FRACTIONS = {"1/1": 4, "1/2": 2, "1/4": 1, "1/8": 1 / 2, "1/16": 1 / 4, "1/32": 1 / 8, "off": 1 / 960}


def grid_ticks(quantize, ppq=PPQ):
    """Ticks per grid step. At PPQ 960: 1/16 = 240, 1/32 = 120, off = 1."""
    if ppq != PPQ:
        raise MediaError("UNSUPPORTED_FORMAT", f"ppq {ppq} != {PPQ}")
    if quantize not in GRID_FRACTIONS:
        raise MediaError("UNSUPPORTED_FORMAT", f"unknown quantize grid {quantize!r}")
    return max(1, int(round(ppq * GRID_FRACTIONS[quantize])))


class MediaError(ValueError):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while block := f.read(1024 * 1024):
            h.update(block)
    return h.hexdigest()


def atomic_write(final_path, data, staging_dir):
    """Write to <staging>/<name>.part, fsync, os.replace to the final path."""
    final_path = Path(final_path)
    staging_dir = Path(staging_dir)
    staging_dir.mkdir(parents=True, exist_ok=True)
    part = staging_dir / (final_path.name + ".part")
    with open(part, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(part, final_path)


def cleanup_staging(staging_dir):
    staging_dir = Path(staging_dir)
    if staging_dir.exists():
        for p in staging_dir.iterdir():
            if p.is_file():
                p.unlink()
        staging_dir.rmdir()
    parent = staging_dir.parent
    if parent.name == ".staging" and parent.exists() and not any(parent.iterdir()):
        parent.rmdir()


# ---- WAV ----
ENCODINGS = {(1, 16): "pcm16", (1, 24): "pcm24", (1, 32): "pcm32",
             (3, 32): "float32", (3, 64): "float64"}


def wav_bytes_pcm16(samples, rate):
    """samples: iterable of floats in [-1, 1]; mono PCM16."""
    payload = b"".join(struct.pack("<h", max(-32768, min(32767, int(round(s * 32767))))) for s in samples)
    header = b"RIFF" + struct.pack("<I", 36 + len(payload)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
    header += b"data" + struct.pack("<I", len(payload))
    return header + payload


def wav_bytes_float32(samples, rate, channels=2):
    """samples: iterable of floats; interleaved frames of `channels`."""
    payload = b"".join(struct.pack("<f", s) for s in samples)
    header = b"RIFF" + struct.pack("<I", 36 + len(payload)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 3, channels, rate, rate * channels * 4, channels * 4, 32)
    header += b"data" + struct.pack("<I", len(payload))
    return header + payload


MAX_WAV_BYTES = 128 * 1024 * 1024   # CONTRACT.md §6 / media-format wavFormat.bytes
MAX_MIDI_BYTES = 8 * 1024 * 1024    # CONTRACT.md §6 / media-format midiFormat.bytes
MAX_LINE_BYTES = 2 * 1024 * 1024    # CONTRACT.md §2.5: one request line
SAMPLE_WIDTH = {"pcm16": 2, "pcm24": 3, "pcm32": 4, "float32": 4, "float64": 8}


def read_file_capped(path, cap, kind):
    """The file's bytes, read ONCE (callers hash and decode these same bytes: no time-of-check gap)."""
    p = Path(path)
    if not p.is_file():
        raise MediaError("BROKEN_REFERENCE", f"input does not exist: {p}")
    if p.stat().st_size > cap:
        raise MediaError("LIMIT_EXCEEDED", f"{kind} is {p.stat().st_size} bytes; the cap is {cap}")
    data = p.read_bytes()
    if len(data) > cap:
        raise MediaError("LIMIT_EXCEEDED", f"{kind} grew past the cap while being read")
    return data


def _wav_open(path):
    """Size cap + RIFF chunk walk + fmt parse: (data, encoding, channels, rate, data-chunk bytes).

    Raises MediaError(UNSUPPORTED_FORMAT|LIMIT_EXCEEDED|BROKEN_REFERENCE). The 128 MiB cap
    (CONTRACT.md §6, `wavFormat.bytes`) is checked on the file size before the bytes are read.
    """
    return wav_parse(read_file_capped(path, MAX_WAV_BYTES, "input WAV"))


def wav_parse(data):
    """Chunk walk over bytes already in memory (the same bytes that were hashed)."""
    if len(data) > MAX_WAV_BYTES:
        raise MediaError("LIMIT_EXCEEDED", f"WAV is {len(data)} bytes; the cap is {MAX_WAV_BYTES}")
    if len(data) < 44 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise MediaError("UNSUPPORTED_FORMAT", "not a RIFF/WAVE file")
    pos, fmt, frames = 12, None, None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if len(body) != size:
            raise MediaError("UNSUPPORTED_FORMAT",
                             f"WAV chunk {cid!r} is truncated ({len(body)} of {size} bytes)")
        if cid == b"fmt ":
            fmt = body
        elif cid == b"data":
            frames = body
        pos += 8 + size + (size & 1)
    if fmt is None or frames is None or len(fmt) < 16:
        raise MediaError("UNSUPPORTED_FORMAT", "WAV without fmt/data chunk")
    tag, channels, rate = struct.unpack("<HHI", fmt[0:8])
    bits = struct.unpack("<H", fmt[14:16])[0]
    if tag == 0xFFFE and len(fmt) >= 26:
        tag = struct.unpack("<H", fmt[24:26])[0]
    enc = ENCODINGS.get((tag, bits))
    if enc is None:
        raise MediaError("UNSUPPORTED_FORMAT", f"unsupported WAV encoding tag={tag} bits={bits}")
    if not 1 <= channels <= 2:
        raise MediaError("UNSUPPORTED_FORMAT", f"channels {channels} outside 1..2")
    if not 8000 <= rate <= 192000:
        raise MediaError("UNSUPPORTED_FORMAT", f"sample rate {rate} outside 8000..192000")
    return data, enc, channels, rate, frames


def _wav_info(data, enc, channels, rate, frames):
    """wavFormat info dict + the length caps (0.1 s .. 600 s) from chunk sizes: (info, n_values, usable)."""
    width = SAMPLE_WIDTH[enc]
    usable = len(frames) - len(frames) % (width * channels)
    n = usable // width
    duration = (n // channels) / rate
    if duration < 0.1:
        raise MediaError("LIMIT_EXCEEDED", f"audio shorter than 0.1 s ({duration:.3f})")
    if duration > 600:
        raise MediaError("LIMIT_EXCEEDED", f"audio longer than 600 s ({duration:.1f})")
    info = {"container": "wav", "encoding": enc, "sampleRate": rate, "channels": channels,
            "durationSeconds": round(duration, 6), "bytes": len(data)}
    return info, n, usable


def read_wav_header(path):
    """Metadata only (no sample decode) — how the submission path checks caps cheaply."""
    data, enc, channels, rate, frames = _wav_open(path)
    return _wav_info(data, enc, channels, rate, frames)[0]


def read_wav(path):
    """Returns (info dict matching media-format wavFormat, mono mix as list[float]).

    Raises MediaError(UNSUPPORTED_FORMAT|LIMIT_EXCEEDED|BROKEN_REFERENCE).
    """
    return wav_decode(read_file_capped(path, MAX_WAV_BYTES, "input WAV"))


def wav_decode(data):
    """(info, mono mix) from WAV bytes already in memory."""
    data, enc, channels, rate, frames = wav_parse(data)
    info, n, usable = _wav_info(data, enc, channels, rate, frames)
    raw = frames[:usable]
    if enc == "pcm16":
        vals = [v / 32768.0 for v in struct.unpack(f"<{n}h", raw)]
    elif enc == "float32":
        vals = list(struct.unpack(f"<{n}f", raw))
    elif enc == "float64":
        vals = list(struct.unpack(f"<{n}d", raw))
    elif enc == "pcm24":
        vals = []
        for i in range(0, n * 3, 3):
            v = raw[i] | (raw[i + 1] << 8) | (raw[i + 2] << 16)
            if v >= 1 << 23:
                v -= 1 << 24
            vals.append(v / float(1 << 23))
    else:  # pcm32
        vals = [v / 2147483648.0 for v in struct.unpack(f"<{n}i", raw)]
    mix = vals[0::channels] if channels == 1 else [(vals[i] + vals[i + 1]) / 2 for i in range(0, n - 1, 2)]
    return info, mix


HIT_HZ = (150.0, 1000.0)  # synthetic "kick band" / "snare-clap band" frequencies (a stand-in, not music)
BASS_SCALE = [28, 28, 31, 26, 28, 33, 31, 26]  # E1-ish synthetic bass pitches (MIDI numbers)


def hit_schedule(seconds, bpm, hits_per_beat):
    """Start times (s) of the percussion events a fixture places — single source of truth.

    hits_per_beat: 1 = quarters, 4 = 1/16, 8 = 1/32, 16 = 1/64. The gate compares
    this ground truth with the notes the service published: a roll event lost
    between detector and file, or snapped onto its neighbour, moves the count and
    the gate goes red.
    """
    spacing = (60.0 / bpm) / hits_per_beat
    return [i * spacing for i in range(int(math.floor(seconds / spacing + 1e-9)))]


def bass_schedule(seconds, bpm=145.0, seed=42):
    """Deterministic synthetic bass events (t_seconds, dur_seconds, midi_pitch) at 1/16.

    Stands in for Basic Pitch's bass output: the service writes exactly these and
    the gate expects exactly these, so "the notes that should exist" has one
    definition. The sparse (j + seed) % 3 rhythm mirrors the measured baseline.
    """
    step = (60.0 / bpm) / 4
    out, t, j = [], step / 2, 0
    while t < seconds - 1e-9:
        if not (j + seed) % 3:
            out.append((t, step * 0.9, BASS_SCALE[(j + seed) % len(BASS_SCALE)]))
        t += step
        j += 1
    return out


def synth_mix(seconds, rate=44100, bpm=145.0, seed=42, hits_per_beat=1, hit_seconds=0.12,
              decay=0.03, alternate=False):
    """Deterministic synthetic hits + bass line (NOT music, NOT a model output).

    `hits_per_beat` / `hit_seconds` / `decay` set how dense and how short the
    percussion events are: that is how the gate feeds the service rolls denser
    than 1/32 and notes shorter than the quantization grid. With `alternate=True`
    odd hits ring at 1000 Hz instead of 150 Hz, so the simulator's stand-in
    (classify_hit) reads two different drums from the audio instead of inventing
    pitches by index. Returns (mix_samples, components) so stems write separately.
    """
    n = int(seconds * rate)
    beat = 60.0 / bpm
    drums = [0.0] * n
    bass = [0.0] * n
    for k, t in enumerate(hit_schedule(seconds, bpm, hits_per_beat)):
        start = int(t * rate)
        hz = HIT_HZ[1] if (alternate and k % 2) else HIT_HZ[0]
        for i in range(min(int(hit_seconds * rate), n - start)):
            drums[start + i] += 0.9 * math.exp(-i / (decay * rate)) * math.sin(2 * math.pi * hz * i / rate)
    step = beat / 4  # 1/16 bass line, independent of the percussion density
    for t, dur, pitch in bass_schedule(seconds, bpm, seed):
        start = int(t * rate)
        freq = 440.0 * 2 ** ((pitch - 69) / 12)
        for i in range(min(int(dur * rate), n - start)):
            env = min(1.0, i / (0.005 * rate)) * math.exp(-i / (0.2 * rate))
            bass[start + i] += 0.03 * env * math.sin(2 * math.pi * freq * i / rate)
    mix = [max(-1.0, min(1.0, drums[i] + bass[i])) for i in range(n)]
    return mix, {"drums": drums, "bass": bass, "other": [0.0] * n}


def envelope(mix, rate, tau_seconds=0.002):
    """One-pole lowpass on |x|: the amplitude envelope the detector and the band split share.

    tau 2 ms is short enough that a 12 ms hit reaches its own peak (so a relative threshold
    reads it) and long enough to smooth the ripple of a 150 Hz period (so it does not fire
    twice per hit).
    """
    if not mix:
        return []
    alpha = 1.0 - math.exp(-1.0 / max(1.0, tau_seconds * rate))
    out = [0.0] * len(mix)
    level = 0.0
    for i, x in enumerate(mix):
        level += alpha * (abs(x) - level)
        out[i] = level
    return out


def detect_onsets(mix, rate, ratio=0.25, release_ratio=0.1, tau_seconds=0.002, min_gap=0.02):
    """Envelope-follower + hysteresis onset detector (simulator stand-in for kick onsets).

    Uses `envelope` (one pole lowpass on |x|, tau 2 ms) to turn the waveform into a smooth
    amplitude envelope; the detection cut is **relative to the loudest moment of that same
    file** (`ratio` * peak), so a dense roll of 20 ms hits is read as easily as a
    sparse 120 ms one — an absolute cut would silently drop the short hits.
    An event starts when the envelope rises through `ratio`*peak and ends when it
    falls under `release_ratio`*peak; `min_gap` (20 ms) keeps one event per hit and
    still sits under the densest roll the gate feeds in (1/64 at 145 BPM = 25.9 ms
    apart), so rolls survive instead of collapsing into their neighbour.

    Returns [(start_seconds, hold_seconds)] with the hold measured from the audio,
    never from the grid: a hit that rings for 12 ms becomes a 12 ms note even when
    the grid is 1/32 (120 ticks = 51.7 ms at 145 BPM).
    """
    n = len(mix)
    if n < int(0.01 * rate):
        return []
    env = envelope(mix, rate, tau_seconds)
    peak = max(env)
    if peak <= 0.0:
        return []
    threshold, release = ratio * peak, release_ratio * peak
    out = []
    start = -1
    last_start = -1.0
    for i, level in enumerate(env):
        if start < 0:
            if level > threshold and i / rate - last_start >= min_gap:
                start = i
                last_start = i / rate
        elif level < release:
            out.append((start / rate, (i - start) / rate))
            start = -1
    if start >= 0:
        out.append((start / rate, (n - start) / rate))
    return out


def classify_hit(mix, rate, start, length=0.02, split=0.05):
    """Brightness of one hit (RMS of the first difference / RMS of the signal).

    150 Hz kick band gives ~0.021, 1 kHz snare/clap band ~0.14, so the cut sits at
    0.05. The simulator's stand-in for timbre detection: two hits quantized onto
    the same grid tick get different pitches *from the audio*, so the gate can
    prove that neither one is written over.
    """
    a = int(start * rate)
    b = min(len(mix), a + max(int(length * rate), int(0.005 * rate)))
    if b - a < 2:
        return "low"
    diff_energy = signal_energy = 0.0
    prev = mix[a]
    for i in range(a + 1, b):
        cur = mix[i]
        d = cur - prev
        diff_energy += d * d
        signal_energy += cur * cur
        prev = cur
    if signal_energy <= 0.0:
        return "low"
    return "high" if math.sqrt(diff_energy / signal_energy) > split else "low"


# ---- MIDI (SMF type 1, PPQ 960) ----

MIDI_INFO_KEYS = ("container", "smfType", "ppq", "tracks", "durationSeconds", "bytes")


def midi_format_of(info):
    """Only the keys media-format.schema.json midiFormat allows (fail-closed against drift)."""
    return {k: info[k] for k in MIDI_INFO_KEYS}


def _vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(out))


def _track(events, name):
    """events: list of (tick, bytes). Adds name meta + end-of-track."""
    ev = [(0, b"\xFF\x03" + _vlq(len(name)) + name.encode("ascii"))] + sorted(events, key=lambda e: e[0])
    ev.append((ev[-1][0] if ev else 0, b"\xFF\x2F\x00"))
    body = b""
    last = 0
    for tick, payload in ev:
        body += _vlq(tick - last) + payload
        last = tick
    return b"MTrk" + struct.pack(">I", len(body)) + body


def midi_bytes(bpm, notes, ppq=PPQ):
    """notes: list of (start_tick, dur_ticks, pitch, velocity, channel).

    Track 0: tempo + time signature. Track 1: drums (ch 9). Track 2: bass (ch 0).
    Refuses any ppq other than 960 (ruling S1b) — a 480 file here would be a
    silent contract break, because the Core DAW imports 960.
    """
    if ppq != PPQ:
        raise MediaError("UNSUPPORTED_FORMAT", f"refusing to write ppq {ppq} != {PPQ}")
    tempo = int(round(60_000_000 / bpm))
    meta = [(0, b"\xFF\x51\x03" + tempo.to_bytes(3, "big")),
            (0, b"\xFF\x58\x04\x04\x02\x18\x08")]
    tracks = [_track(meta, "tempo")]
    for ch, name in ((9, "drums"), (0, "bass")):
        ev = []
        for start, dur, pitch, vel, c in notes:
            if c != ch:
                continue
            ev.append((start, bytes([0x90 | ch, pitch, vel])))
            ev.append((start + dur, bytes([0x80 | ch, pitch, 0])))
        tracks.append(_track(ev, name))
    header = b"MThd" + struct.pack(">IHHH", 6, 1, len(tracks), ppq)
    return header + b"".join(tracks)


def read_midi(path, with_notes=False):
    """Parse SMF header + tracks; returns info matching media-format midiFormat.

    Proves PPQ (== 960), track count, tempo presence and duration. With
    with_notes=True the returned dict also carries "noteEvents": list of
    {channel, pitch, start, dur} — how the gate shows that notes shorter than
    the grid, or two hits quantized onto the same tick, survive unwritten-over.
    Raises MediaError.
    """
    data = read_file_capped(path, MAX_MIDI_BYTES, "MIDI")
    if len(data) < 26 or data[0:4] != b"MThd":
        raise MediaError("UNSUPPORTED_FORMAT", "not a Standard MIDI File")
    hlen = struct.unpack(">I", data[4:8])[0]
    fmt, ntrks, division = struct.unpack(">HHH", data[8:14])
    if hlen < 6 or fmt not in (0, 1):
        raise MediaError("UNSUPPORTED_FORMAT", f"unsupported SMF format {fmt}")
    if division & 0x8000:
        raise MediaError("UNSUPPORTED_FORMAT", "SMPTE division not allowed (PPQ required)")
    ppq = division
    if ppq != PPQ:
        raise MediaError("UNSUPPORTED_FORMAT", f"PPQ {ppq} != {PPQ} (Core templates are {PPQ})")
    pos = 8 + hlen
    tracks = 0
    max_tick = 0
    tempo = None
    notes = 0
    note_events = []
    open_notes = {}
    while pos + 8 <= len(data):
        if data[pos:pos + 4] != b"MTrk":
            raise MediaError("UNSUPPORTED_FORMAT", "bad track chunk")
        tlen = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + tlen]
        if len(body) != tlen:
            raise MediaError("UNSUPPORTED_FORMAT", "truncated track")
        i, tick, end_seen = 0, 0, False
        while i < len(body):
            delta = 0
            while True:
                b = body[i]
                i += 1
                delta = (delta << 7) | (b & 0x7F)
                if not b & 0x80:
                    break
            tick += delta
            status = body[i]
            if status == 0xFF:
                meta_type = body[i + 1]
                i += 2
                length = 0
                while True:
                    b = body[i]
                    i += 1
                    length = (length << 7) | (b & 0x7F)
                    if not b & 0x80:
                        break
                payload = body[i:i + length]
                i += length
                if meta_type == 0x51 and length == 3 and tempo is None:
                    tempo = int.from_bytes(payload, "big")
                if meta_type == 0x2F:
                    end_seen = True
                    break
            elif status in (0xF0, 0xF7):
                i += 1
                length = 0
                while True:
                    b = body[i]
                    i += 1
                    length = (length << 7) | (b & 0x7F)
                    if not b & 0x80:
                        break
                i += length
            else:
                i += 1
                kind = status & 0xF0
                if kind in (0x80, 0x90, 0xA0, 0xB0, 0xE0):
                    if kind in (0x90, 0x80):
                        ch, pitch, vel = status & 0x0F, body[i], body[i + 1]
                        if kind == 0x90 and vel > 0:
                            notes += 1
                            open_notes[(ch, pitch)] = tick
                        else:
                            start = open_notes.pop((ch, pitch), None)
                            if start is not None:
                                note_events.append({"channel": ch, "pitch": pitch,
                                                    "start": start, "dur": tick - start})
                    i += 2
                elif kind in (0xC0, 0xD0):
                    i += 1
                else:
                    raise MediaError("UNSUPPORTED_FORMAT", f"bad status {status:#x}")
        if not end_seen:
            raise MediaError("UNSUPPORTED_FORMAT", "track without end-of-track")
        open_notes.clear()
        max_tick = max(max_tick, tick)
        tracks += 1
        pos += 8 + tlen
    if tracks != ntrks:
        raise MediaError("UNSUPPORTED_FORMAT", f"header says {ntrks} tracks, found {tracks}")
    if tempo is None:
        raise MediaError("UNSUPPORTED_FORMAT", "no tempo map (contract requires one)")
    duration = max_tick / ppq * tempo / 1_000_000
    if not 0 < duration <= 600:
        raise MediaError("LIMIT_EXCEEDED", f"MIDI duration {duration:.1f}s outside 0..600")
    info = {"container": "midi", "smfType": fmt, "ppq": ppq, "tracks": tracks,
            "durationSeconds": round(duration, 6), "bytes": len(data), "notes": notes}
    if with_notes:
        info["noteEvents"] = note_events
    return info

# ---- request JSON ----

def _no_constant(name):
    raise ValueError(f"{name} is not JSON (RFC 8259 has no NaN/Infinity)")


def _number(text):
    v = float(text)
    return int(v) if v.is_integer() and abs(v) < 2 ** 53 else v


def loads_request(raw):
    """json.loads for request lines: NaN/Infinity refused (ValueError), and an integral float such as
    36.0 becomes 36, so a value the schema accepts as an integer is an int for the code after it."""
    return json.loads(raw, parse_constant=_no_constant, parse_float=_number)
