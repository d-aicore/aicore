"""AICore-C S2 — audio->MIDI service (Demucs + Basic Pitch, CPU), contract `aicore.service/1` (S1b).

One transport (CONTRACT.md §2): one JSON request line on stdin, one JSON object per line on stdout
(`start` -> `progress`* -> `done` | `error`), logs on stderr, exit 0 only after `done`, 2 after `error`.

  python aicore_midi_service.py hello
  python aicore_midi_service.py job  < request.jsonl

Pipeline = the measured baseline `D:\\AICore\\midi-maker\\midi_maker.py` (READ-ONLY, ported here, not
imported): Demucs (core_stems.py in the stems env, CPU) -> kick onsets of the drum stem's 40-120 Hz band
-> note `kickNote`; Basic Pitch ONNX on the bass stem (30-400 Hz) and the other stem (150-5000 Hz);
bass re-cut on 1/16 attacks (`splitOnOnsets`), lowest note wins (`bassMono`); lead FX/blip filter
(`leadFilter`); BPM provided / from the file name / from the kicks; grid phase from the kicks.

What changes from the baseline (S1b ruling): the .mid is SMF type 1 at **PPQ 960** (baseline: 480);
the grid comes from `params.quantize` up to **1/32** (baseline: 1/16 only); a note's START snaps to the
grid, its LENGTH is the measured length from the audio (baseline: lengths in whole grid steps).
MIDI tick 0 = the first grid line at or after the window start minus the grid phase, i.e. the kick's
beat grid starts the bar, like the baseline; `metrics.gridPhaseSeconds` says where that is in the input.

Runs on Python 3.10+ with numpy + onnxruntime + basic_pitch (the midi-maker env); Demucs runs in its own
env through `core_stems.py`. Paths come from env vars (defaults = this PC's installs, see README).
Heavy children start at BELOW_NORMAL priority (audio on this PC crackles under load).
Every output is `validation: "unreviewed"`: musical approval is the owner's.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
SIM = REPO / "contracts" / "sim"
sys.path.insert(0, str(SIM))
import media  # contract helpers: PPQ, grid_ticks, sha256, atomic_write, read_midi (refuses ppq != 960)
from jsonschema_mini import Registry, validation_errors

CONTRACT = "aicore.service/1"
SERVICE = {"name": "aicore-midi", "version": "s2-0.1.0"}
SCHEMA_DIR = REPO / "contracts" / "schemas" / "v1"
GRID_ORDER = ["1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "off"]
MAX_WAV_BYTES = 128 * 1024 * 1024   # CONTRACT.md §6: WAV <= 128 MiB
MAX_MIDI_BYTES = 8 * 1024 * 1024    # CONTRACT.md §6: MIDI <= 8 MiB
# maxOutputBytes = the largest single output this service may publish: a stem WAV with includeStems (128 MiB);
# the .mid itself is capped at MAX_MIDI_BYTES (audit 2026-11-16: hello used to say 8 MiB while stems were bigger).
LIMITS = {"maxInputBytes": MAX_WAV_BYTES, "maxOutputBytes": MAX_WAV_BYTES,
          "maxDurationSeconds": 600, "maxResponseBytes": 2 * 1024 * 1024}
RETRYABLE = {"WORKER_UNAVAILABLE"}
TRANSCRIBED_PARTS = ("drums", "bass", "other")  # 'vocals' is in the schema but has no transcriber here
WINDOW_SLACK_S = 0.001
SR = 44100
PHASES = ("read", "separate", "transcribe", "quantize", "write")

# This PC's installs (HANDOFF §3/§4). Overridable, never written to.
STEMS_PY = os.environ.get("AICORE_STEMS_PY", r"D:\AICore\models\stems-env\Scripts\python.exe")
CORE_STEMS = os.environ.get("AICORE_CORE_STEMS",
                            r"D:\AICore\app\apps\studio\src\aicore_studio\providers\core_stems.py")
MODELS_HOME = os.environ.get("AICORE_MODELS_HOME", r"D:\AICore\models")
THREADS = os.environ.get("AICORE_MIDI_THREADS", "4")
# ADTOF (PyTorch port, pretrained, CPU) for the kick when its env is on this PC; else the onset detector below.
# Measured 2026-11-16 on the 4 template renders (drums stem, 1/32): F1 0.94/0.99/0.95/0.49 vs 0.76/0.95/0.97/0.51.
ADTOF_PY = os.environ.get("AICORE_ADTOF_PY", r"D:\AICore\models\adtof-env\Scripts\python.exe")
ADTOF_KICK_PITCH = "35"
BELOW_NORMAL = 0x00004000

# Baseline knobs (midi_maker.py), unchanged.
BASS_GATE = 0.25
LEAD_FLAT_FLOOR = 0.5
KICK_LOW, KICK_HIGH = 40, 120


def now():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def log(text):
    sys.stderr.write(f"[aicore-midi] {text}\n")
    sys.stderr.flush()


_PROTO = None  # the real stdout, reserved for protocol lines (see protect_stdout)


def protect_stdout():
    """stdout = one JSON object per line, nothing else (CONTRACT.md §2.2). Libraries print to stdout
    (basic_pitch: "Predicting MIDI for ..."), so fd 1 is pointed at stderr and protocol lines go to a
    private duplicate of the original stdout."""
    global _PROTO
    sys.stdout.flush()
    fd = os.dup(1)
    os.dup2(2, 1)
    _PROTO = os.fdopen(fd, "w", encoding="ascii", newline="\n", buffering=1)
    sys.stdout = sys.stderr


def emit_line(doc):
    out = _PROTO or sys.stdout
    out.write(json.dumps(doc, ensure_ascii=True, allow_nan=False) + "\n")
    out.flush()


class ContractError(Exception):
    def __init__(self, code, message, details=None):
        super().__init__(message)
        self.code = code
        self.message = str(message)[:500]
        self.details = details or {}

    def to_dict(self, request_id):
        out = {"code": self.code, "message": self.message, "requestId": request_id,
               "retryable": self.code in RETRYABLE}
        if self.details:
            out["details"] = self.details
        return out


def lower_priority():
    if os.name == "nt":
        try:
            import ctypes
            ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), BELOW_NORMAL)
        except Exception:  # noqa: BLE001 - best effort
            pass
    for k in ("OMP_NUM_THREADS", "MKL_NUM_THREADS", "OPENBLAS_NUM_THREADS"):
        os.environ.setdefault(k, THREADS)


# ---- WAV (numpy) ----

def read_wav_np(path):
    """(info, mono float32 at native rate, stereo float32 [frames, ch]). Same chunk walk as core_stems."""
    import numpy as np
    import struct
    data = Path(path).read_bytes()
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ContractError("UNSUPPORTED_FORMAT", "not a RIFF/WAVE file")
    pos, fmt, raw = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, ch, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            if tag == 0xFFFE and len(body) >= 26:
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, ch, rate, bits)
        elif cid == b"data":
            raw = body
        pos += 8 + size + (size & 1)
    if fmt is None or raw is None:
        raise ContractError("UNSUPPORTED_FORMAT", "WAV without fmt or data chunk")
    tag, ch, rate, bits = fmt
    enc = {(1, 16): "pcm16", (1, 24): "pcm24", (1, 32): "pcm32", (3, 32): "float32", (3, 64): "float64"}.get((tag, bits))
    if enc is None:
        raise ContractError("UNSUPPORTED_FORMAT", f"WAV encoding tag {tag} / {bits} bit not supported")
    if not 1 <= ch <= 2:
        raise ContractError("UNSUPPORTED_FORMAT", f"{ch} channels (midi profile takes 1-2)")
    if not 8000 <= rate <= 192000:
        raise ContractError("UNSUPPORTED_FORMAT", f"sample rate {rate} outside 8000-192000")
    width = bits // 8
    n = len(raw) // (width * ch)
    raw = raw[:n * width * ch]
    if enc == "pcm16":
        x = np.frombuffer(raw, "<i2").astype(np.float32) / 32768.0
    elif enc == "pcm24":
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3)
        v = (b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16))
        v = np.where(v & 0x800000, v - 0x1000000, v)
        x = v.astype(np.float32) / 8388608.0
    elif enc == "pcm32":
        x = np.frombuffer(raw, "<i4").astype(np.float32) / 2147483648.0
    elif enc == "float32":
        x = np.frombuffer(raw, "<f4").astype(np.float32)
    else:
        x = np.frombuffer(raw, "<f8").astype(np.float32)
    stereo = x.reshape(-1, ch)
    dur = n / rate
    if not 0.1 <= dur <= 600:
        raise ContractError("LIMIT_EXCEEDED", f"duration {dur:.2f}s outside 0.1-600 s")
    info = {"container": "wav", "encoding": enc, "sampleRate": rate, "channels": ch,
            "durationSeconds": round(dur, 6), "bytes": len(data)}
    return info, stereo


def to_rate(x, rate, target=SR):
    import numpy as np
    if rate == target:
        return x.astype(np.float32)
    n = int(round(len(x) * target / rate))
    t = np.linspace(0, len(x) - 1, n)
    return np.interp(t, np.arange(len(x)), x).astype(np.float32)


def wav_float32_bytes(stereo, rate):
    import numpy as np
    import struct
    data = np.ascontiguousarray(stereo, dtype="<f4").tobytes()
    ch = stereo.shape[1]
    fmt = struct.pack("<HHIIHH", 3, ch, rate, rate * ch * 4, ch * 4, 32)
    return (b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(data)) + b"WAVE" +
            b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data)


# ---- baseline DSP (ported from midi_maker.py, read-only source) ----

KICK_REFRACTORY_S = 0.025      # first pass only (< a 1/32 cell up to 260 BPM); the cell rule comes after the BPM
KICK_REFINE_THRESHOLD = 0.2    # attack = where the broadband envelope rises through this share of its peak
KICK_REFINE_BAND = (30, 5000)   # kick body + click: a pitch-swept click leads the 40-120 Hz body by ~20 ms
KICK_VALLEY_SLACK = 1.02       # walk-back ignores ripples under 2 %


def band_envelope(x, lo, hi, smooth_s=0.001):
    """Zero-phase band-pass (FFT mask) -> analytic-signal magnitude -> 1 ms moving average. No delay, so
    its times are the audio's times."""
    import numpy as np
    n = len(x)
    X = np.fft.fft(x)
    f = np.fft.fftfreq(n, 1 / SR)
    X *= np.where((f >= lo) & (f <= hi), 2.0, 0.0)  # positive band only, doubled = analytic signal
    env = np.abs(np.fft.ifft(X))
    w = max(1, int(smooth_s * SR))
    return np.convolve(env, np.ones(w) / w, mode="same").astype(np.float32)


def check_window(duration, start, length):
    """The requested window must lie inside the WAV (audit 2026-11-16: it used to be cut short silently)."""
    if start < 0 or start >= duration:
        raise ContractError("LIMIT_EXCEEDED", f"startSeconds {start:g} is outside the WAV (0-{duration:.3f} s)",
                            {"startSeconds": start, "durationSeconds": duration})
    if start + length > duration + WINDOW_SLACK_S:
        raise ContractError("LIMIT_EXCEEDED", f"window {start:g}+{length:g} s ends after the WAV ({duration:.3f} s)",
                            {"startSeconds": start, "lengthSeconds": length, "durationSeconds": duration})


def check_parts(parts):
    """Every requested part must have a transcriber (audit 2026-11-16: 'vocals' was accepted and silently dropped)."""
    missing = [p for p in parts if p not in TRANSCRIBED_PARTS]
    if missing:
        raise ContractError("UNSUPPORTED_OPERATION", f"parts {missing} are not transcribed by this service "
                            f"(offered: {list(TRANSCRIBED_PARTS)})", {"parts": missing})


def check_output_size(role, n):
    cap = MAX_MIDI_BYTES if role == "midi" else MAX_WAV_BYTES
    if n > cap:
        raise ContractError("LIMIT_EXCEEDED", f"output {role} would be {n} bytes (cap {cap})", {"role": role})


def kick_onsets(x):
    """Kick onsets in seconds (P1). Coarse: rises of the 40-120 Hz log energy, 2048-sample frames, hop 220
    (the baseline detector). The baseline stamped a hit at its frame's START, and the log-energy rise shows
    while the kick is still entering the window, so its times came out 16-36 ms early. Refine, per hit: the
    peak of a zero-phase 30 Hz-5 kHz envelope within [-30 ms, +60 ms] of the frame centre, walked back to
    where it falls under 20 % of that peak (or to a valley left by the previous hit). Measured on the 4
    template windows' drum stems, raw median error vs the truth: 40-120 Hz alone +8..+23 ms (a pitch-swept
    kick's click leads its low body), 30 Hz-5 kHz -0.05..+0.2 ms.
    Refractory 25 ms (the baseline's 60 ms hid 1/32 rolls above 125 BPM); `dedupe_cells` applies the
    grid-cell rule once the BPM is known."""
    import numpy as np
    hop, n = 220, 2048
    if len(x) < n + hop:
        return np.array([])
    frames = np.lib.stride_tricks.sliding_window_view(x, n)[::hop] * np.hanning(n)
    spec = np.abs(np.fft.rfft(frames, axis=1))
    f = np.fft.rfftfreq(n, 1 / SR)
    low = spec[:, (f > KICK_LOW) & (f < KICK_HIGH)].sum(1)
    env = np.log(low + 1e-6)
    d = np.maximum(np.diff(env), 0)
    thr = d.mean() + 2.5 * d.std()
    peaks = [i for i in range(1, len(d) - 1) if d[i] > thr and d[i] >= d[i - 1] and d[i] >= d[i + 1]]
    wide = band_envelope(x, KICK_REFINE_BAND[0], KICK_REFINE_BAND[1])
    out, last = [], -1e9
    for i in peaks:
        centre = ((i + 1) * hop + n // 2) / SR
        a, b = max(0, int((centre - 0.03) * SR)), min(len(wide), int((centre + 0.06) * SR))
        if b - a < 8:
            continue
        seg = wide[a:b]
        k = int(np.argmax(seg))
        floor, j = KICK_REFINE_THRESHOLD * seg[k], k
        while j > 0 and seg[j - 1] >= floor and seg[j - 1] <= seg[j] * KICK_VALLEY_SLACK:
            j -= 1
        t = (a + j) / SR
        if t - last > KICK_REFRACTORY_S:
            out.append(t)
            last = t
    return np.array(out)


def adtof_kicks(wav_path, staging):
    """Kick onsets (s) from ADTOF on the drums stem, or None when ADTOF is not installed or fails
    (the caller then uses kick_onsets and says so in metrics.kickDetector)."""
    import numpy as np
    if not Path(ADTOF_PY).is_file():
        return None, "onset (ADTOF not installed)"
    out = Path(staging) / "adtof.json"
    env = dict(os.environ, CUDA_VISIBLE_DEVICES="", AICORE_MIDI_THREADS=THREADS)
    for k in ("PYTHONPATH", "PYTHONHOME", "VIRTUAL_ENV"):
        env.pop(k, None)
    p = subprocess.run([ADTOF_PY, str(HERE / "adtof_kicks.py"), f"{wav_path}={out}"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", env=env,
                       creationflags=BELOW_NORMAL if os.name == "nt" else 0)
    if p.returncode != 0 or not out.is_file():
        log(f"ADTOF failed (exit {p.returncode}): {p.stderr[-300:]}")
        return None, "onset (ADTOF failed)"
    times = json.loads(out.read_text(encoding="utf-8"))["onsets"].get(ADTOF_KICK_PITCH, [])
    return np.array(sorted(times), dtype=float), "adtof"


ROLL_RATIO = 4.0      # declared a priori: 1/32 modulation at least 4x the 1/16 one inside a beat
ROLL_MIN_DEPTH = 0.05  # and a clear 1/32 pulse (normalised Fourier magnitude of the low-band envelope)


def kick_rolls(x, bpm, phase, seconds):
    """1/32 kick rolls (hi-tech): inside a beat, a roll makes the 40-120 Hz envelope pulse at the 1/32 rate with
    no 1/16 component, while single kicks, bass notes and 1/16 lines all carry a 1/16 component. Beats that pass
    get a kick on each of their eight 1/32 lines. (2026-11-16: Demucs routes rolls to the bass stem and both the
    onset detector and ADTOF miss them; on the controlled set the ratio is 17-81 on roll beats and <= 1.7
    elsewhere. On the real t2 render the roll has a long tail and does not pulse, so it stays missed.)"""
    import numpy as np
    env = band_envelope(x, KICK_LOW, KICK_HIGH, smooth_s=0.002)
    beat = 60 / bpm
    out = []
    b = phase - np.ceil(phase / beat) * beat
    while b + beat <= seconds:
        if b >= 0:
            seg = env[int(b * SR): int((b + beat) * SR)]
            if len(seg) > 16 and seg.sum() > 0:
                t = np.arange(len(seg)) / SR
                c8 = abs(np.sum(seg * np.exp(-2j * np.pi * 8 * t / beat))) / seg.sum()
                c4 = abs(np.sum(seg * np.exp(-2j * np.pi * 4 * t / beat))) / seg.sum()
                if c8 >= ROLL_MIN_DEPTH and c8 >= ROLL_RATIO * c4:
                    out += [b + k * beat / 8 for k in range(8)]
        b += beat
    return np.array(out)


def dedupe_cells(onsets, bpm, cell="1/32"):
    """Refractory tied to the grid: two kicks closer than 0.6 of a cell are one kick."""
    import numpy as np
    step = 60 / bpm / (int(cell.split("/")[1]) / 4)
    out = []
    for t in onsets:
        if not out or t - out[-1] > 0.6 * step:
            out.append(float(t))
    return np.array(out)


def estimate_bpm(onsets, lo=140, hi=260):
    import numpy as np
    if len(onsets) < 8:
        return None
    best, score = None, -1
    for bpm in np.arange(lo, hi, 0.25):
        ph = (onsets / (60 / bpm)) % 1
        s = np.abs(np.mean(np.exp(2j * np.pi * ph)))
        if s > score:
            best, score = bpm, s
    return float(best)


def bpm_from_name(path):
    m = re.search(r"(?<!\d)(\d{2,3}(?:[.,]\d+)?)\s*bpm", Path(path).stem, re.I)
    if m:
        v = float(m.group(1).replace(",", "."))
        if 40 <= v <= 300:
            return v
    return None


def grid_phase(onsets, bpm):
    """Grid phase in seconds, in [0, beat) (P1). The baseline took the circular MEAN of the onsets modulo
    the beat: kicks on off-beat 1/16s (rolling hi-tech kicks) pull that mean between grid lines. Here:
    1) the 1/16 phase = circular MEDIAN of the onsets modulo a 1/16 step (every grid kick agrees on it,
       outliers do not drag it);
    2) which 1/16 of the beat is the downbeat = the slot holding the most kicks."""
    import numpy as np
    beat = 60 / bpm
    step = beat / 4
    ph = np.mod(onsets, step)
    if not len(ph):
        return 0.0

    def cost(c):
        dd = np.abs(ph - c)
        return float(np.minimum(dd, step - dd).sum())
    p16 = float(min(ph, key=cost))
    slots = np.round((onsets - p16) / step).astype(int) % 4
    best = int(np.bincount(slots, minlength=4).argmax())
    return float((p16 + best * step) % beat)


def basic_pitch_notes(path, fmin, fmax):
    from basic_pitch import ICASSP_2022_MODEL_PATH
    from basic_pitch.inference import predict
    _, _, events = predict(str(path), ICASSP_2022_MODEL_PATH, onset_threshold=0.5, frame_threshold=0.3,
                           minimum_note_length=40, minimum_frequency=fmin, maximum_frequency=fmax,
                           multiple_pitch_bends=False, melodia_trick=True)
    return [(float(s), float(e), int(p), float(a), (rest[0] if rest else None)) for s, e, p, a, *rest in events]


def flatness_track(x):
    import numpy as np
    n, hop = 2048, 1024
    if len(x) < n:
        return np.array([]), hop / SR
    fr = np.lib.stride_tricks.sliding_window_view(x, n)[::hop] * np.hanning(n)
    s = np.abs(np.fft.rfft(fr, axis=1))[:, 10:] + 1e-9
    return np.exp(np.log(s).mean(1)) / s.mean(1), hop / SR


def clean_lead(notes, x, bpm):
    import numpy as np
    flat, fstep = flatness_track(x)
    beat = 60 / bpm
    fl = [float(np.median(flat[int(s / fstep): max(int(s / fstep) + 1, int(e / fstep))])) if len(flat) else 0.0
          for s, e, *_ in notes]
    flat_max = max(LEAD_FLAT_FLOOR, float(np.percentile(fl, 85))) if fl else 1
    kept = []
    for (s, e, p, a, bends), f in zip(notes, fl):
        if not 48 <= p <= 100 or a < 0.2 or f > flat_max:
            continue
        if bends is not None and len(bends) and np.max(np.abs(bends)) > 3:
            continue
        kept.append((s, e, p, a))
    out = []
    for s, e, p, a in kept:
        if e - s < 0.06 and not any(o[0] != s and abs(o[0] - s) < beat for o in kept):
            continue
        out.append((s, e, p, a))
    return out


BASS_TAIL_DECAY = 0.7  # declared, not tuned per song: a step that only decays (end < 70 % of start) is a tail


def split_on_onsets(notes, x, bpm, phase):
    """Baseline 'energia' mode: every loud 1/16 step of the bass stem becomes a note (hi-tech bass re-cut).
    2026-11-16: a step that is only a decaying tail (no louder than the end of the previous step, and its end below
    70 % of its start) is not a new note. The release tail of a note used to become an extra note one 1/16 later (wakeup controlled:
    80 of 83 extra bass notes were exactly that)."""
    import numpy as np
    stp = 60 / bpm / 4
    hop = max(1, int(SR * stp / 4))
    n = len(x) // hop
    if n < 8:
        return []
    env = np.sqrt((x[: n * hop].reshape(n, hop) ** 2).mean(1) + 1e-12)
    p95 = np.percentile(env, 95)
    out, k = [], 0
    while True:
        t = phase + k * stp
        if t >= len(x) / SR:
            break
        if t >= 0:
            i = int(t * SR / hop)
            seg = env[i: i + 4]
            tail = len(seg) == 4 and i > 0 and seg[0] <= env[i - 1] and seg[3] < BASS_TAIL_DECAY * seg[0]
            if len(seg) and seg.mean() > BASS_GATE * p95 and not tail:
                active = [p for s, e, p, a in notes if s - stp / 2 <= t < e] or \
                         [p for s, e, p, a in sorted(notes, key=lambda nn: abs(nn[0] - t)) if abs(s - t) < 4 * stp][:1]
                if active:
                    amp = float(np.clip(seg.mean() / p95, 0.3, 1.0))
                    out.append((t, t + stp * 0.9, min(active), amp))
        k += 1
    return out


# ---- quantization at PPQ 960 (S1b): start on the grid, length from the audio ----

def place(notes, bpm, phase, quantize, mono=False, ppq=media.PPQ):
    """(start_s, end_s, pitch, amp) -> (tick, dur_ticks, pitch, vel). Tick 0 = grid phase."""
    g = media.grid_ticks(quantize, ppq)
    tps = ppq * bpm / 60.0
    rows = {}
    for s, e, p, a in notes:
        raw = (s - phase) * tps
        tick = int(round(raw / g)) * g
        if tick < 0:
            continue
        dur = max(1, int(round((e - s) * tps)))
        vel = int(min(127, max(1, 40 + 87 * a)))
        key = tick if mono else (tick, p)
        if mono:
            if key not in rows or p < rows[key][2]:
                rows[key] = (tick, dur, p, vel)
        elif key not in rows or dur > rows[key][1]:
            rows[key] = (tick, dur, p, vel)
    out = sorted(rows.values())
    if mono:  # no overlaps in a monophonic line
        out = [(t, max(1, min(d, out[i + 1][0] - t)) if i + 1 < len(out) else d, p, v)
               for i, (t, d, p, v) in enumerate(out)]
    return out


def tidy_lengths(rows, quantize, ppq=media.PPQ):
    """'Guitar Pro' lengths for a lead line (declared rule, applied the same to every song): a note ends at the
    next note START of the line at the latest (no ringing over the next note), and its length is a whole number
    of grid cells (at least one). Synth releases made Basic Pitch lengths ~3x the written ones."""
    g = media.grid_ticks(quantize, ppq)
    starts = sorted({t for t, _, _, _ in rows})
    out = []
    for t, d, p, v in rows:
        i = starts.index(t)
        if i + 1 < len(starts):
            d = min(d, starts[i + 1] - t)
        out.append((t, max(g, int(round(d / g)) * g), p, v))
    return out


def place_kicks(kicks, bpm, phase, quantize, kick_note, ppq=media.PPQ):
    """Kick: one note per grid slot; length = time to the next kick, capped at 1/16 (the baseline wrote 1 step)."""
    tps = ppq * bpm / 60.0
    cap = media.grid_ticks("1/16", ppq)
    placed = place([(t, t + cap / tps, kick_note, 1.0) for t in kicks], bpm, phase, quantize, mono=True)
    return [(t, min(d, cap), p, 110) for t, d, p, _ in placed]


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(out))


def smf_bytes(bpm, tracks, ppq=media.PPQ):
    """SMF type 1. tracks: [(name, channel, [(tick, dur, pitch, vel)])]. Refuses ppq != 960 (S1b)."""
    import struct
    if ppq != media.PPQ:
        raise ContractError("UNSUPPORTED_FORMAT", f"refusing to write ppq {ppq} != {media.PPQ}")

    def chunk(events, name):
        ev = [(0, 0, b"\xFF\x03" + vlq(len(name)) + name.encode("ascii"))] + events
        ev.sort(key=lambda e: (e[0], e[1]))
        body, last = b"", 0
        for t, _, payload in ev:
            body += vlq(t - last) + payload
            last = t
        body += vlq(0) + b"\xFF\x2F\x00"
        return b"MTrk" + struct.pack(">I", len(body)) + body

    tempo = int(round(60_000_000 / bpm))
    out = [chunk([(0, 1, b"\xFF\x51\x03" + tempo.to_bytes(3, "big")),
                  (0, 1, b"\xFF\x58\x04\x04\x02\x18\x08")], "tempo")]
    for name, ch, notes in tracks:
        ev = []
        for t, d, p, v in notes:
            ev.append((t, 2, bytes([0x90 | ch, p, v])))
            ev.append((t + d, 1, bytes([0x80 | ch, p, 0])))  # offs before ons at the same tick
        out.append(chunk(ev, name))
    return b"MThd" + struct.pack(">IHHH", 6, 1, len(out), ppq) + b"".join(out)


def environment(params):
    """Fingerprint of what made the notes (plan B fase 0 / adenda 5): report.json and manifest.json
    (`environment`, optional in task-manifest.schema.json)."""
    import platform
    out = {"python": platform.python_version(), "threads": THREADS, "seed": int(params.get("seed", 42)),
           "demucsModel": "htdemucs", "stemsPython": STEMS_PY}
    out["kickModel"] = "adtof-pytorch (bundled weights)" if Path(ADTOF_PY).is_file() else "onset detector"
    for mod in ("numpy", "onnxruntime", "basic_pitch"):
        try:
            out[mod] = __import__(mod).__version__
        except Exception:  # noqa: BLE001 - a fingerprint never fails the job
            out[mod] = None
    try:
        from basic_pitch import ICASSP_2022_MODEL_PATH
        mp = Path(ICASSP_2022_MODEL_PATH)
        files = [mp] if mp.is_file() else sorted(q for q in mp.rglob("*") if q.is_file())
        out["basicPitchModel"] = {q.name: media.sha256_file(q) for q in files[:8]}
    except Exception:  # noqa: BLE001
        out["basicPitchModel"] = None
    return out


# ---- the job ----

class Job:
    def __init__(self, request, task_id):
        self.request = request
        self.task_id = task_id
        self.request_id = request["requestId"]
        self.output_dir = Path(request["outputDir"])
        self.staging = self.output_dir / ".staging" / task_id
        self.seq = 0
        self.last_fraction = 0.0

    def progress(self, phase, fraction, message=None):
        fraction = max(self.last_fraction, min(1.0, fraction))
        self.last_fraction = fraction
        self.seq += 1
        doc = {"event": "progress", "schemaVersion": "1.0", "eventSeq": self.seq, "taskId": self.task_id,
               "phase": phase, "fraction": round(fraction, 4), "step": int(round(fraction * 100)),
               "totalSteps": 100, "at": now()}
        if message:
            doc["message"] = message[:200]
        emit_line(doc)

    def separate(self, mix_path):
        """core_stems.py in the stems env, CPU, offline weights only (no downloads, HANDOFF §7)."""
        if not Path(STEMS_PY).is_file() or not Path(CORE_STEMS).is_file():
            raise ContractError("BACKEND_FAILED", "Demucs env or core_stems.py not found",
                                {"phase": "separate", "stemsPy": STEMS_PY, "coreStems": CORE_STEMS})
        out = self.staging / "stems"
        env = dict(os.environ, HF_HOME=str(Path(MODELS_HOME) / "huggingface"),
                   TORCH_HOME=str(Path(MODELS_HOME) / "torch"), HF_HUB_OFFLINE="1",
                   OMP_NUM_THREADS=THREADS, MKL_NUM_THREADS=THREADS, PYTHONIOENCODING="utf-8")
        for k in ("PYTHONPATH", "PYTHONHOME", "VIRTUAL_ENV"):  # our env's packages must not leak into Demucs's
            env.pop(k, None)
        seed = int(self.request["params"].get("seed", 42))
        cmd = [STEMS_PY, str(HERE / "demucs_seeded.py"), str(seed), CORE_STEMS,
               "separate", "--input", str(mix_path), "--out", str(out),
               "--model", "htdemucs", "--device", "cpu"]
        flags = BELOW_NORMAL if os.name == "nt" else 0
        errlog = self.staging / "demucs.stderr.log"
        with open(errlog, "w", encoding="utf-8", errors="replace") as ef:
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=ef, text=True, encoding="utf-8",
                                 errors="replace", env=env, creationflags=flags)
            done, err = None, None
            for line in p.stdout:
                try:
                    ev = json.loads(line)
                except ValueError:
                    continue
                if ev.get("event") == "progress" and ev.get("fraction") is not None:
                    self.progress("separate", 0.05 + 0.5 * float(ev["fraction"]), "Demucs htdemucs (CPU)")
                elif ev.get("event") == "done":
                    done = ev
                elif ev.get("event") == "error":
                    err = ev.get("message", "")
            code = p.wait()
        tail = errlog.read_text(encoding="utf-8", errors="replace")[-400:]
        if done is None or code != 0:
            msg = err or tail or f"exit {code}"
            kind = "OUT_OF_MEMORY" if re.search(r"out of memory|MemoryError|CUDA out", msg, re.I) else "BACKEND_FAILED"
            raise ContractError(kind, f"Demucs failed: {msg}"[:500], {"phase": "separate", "exit": code})
        return {s["name"]: Path(s["path"]) for s in done["stems"]}

    def run(self):
        import numpy as np
        t0 = time.monotonic()
        params = self.request["params"]
        parts = params["parts"]
        quantize = params["quantize"]
        mix_ref = next(r for r in self.request["inputs"] if r["role"] == "mix")
        try:
            self.staging.mkdir(parents=True, exist_ok=True)
            self.progress("read", 0.0, "a ler o WAV")
            info, stereo = read_wav_np(mix_ref["path"])
            rate = info["sampleRate"]
            start = float(params.get("startSeconds", 0))
            length = float(params.get("lengthSeconds", info["durationSeconds"] - start))
            check_window(info["durationSeconds"], start, length)
            win = stereo[int(start * rate): int((start + length) * rate)]
            if len(win) < int(0.1 * rate):
                raise ContractError("LIMIT_EXCEEDED", "window shorter than 0.1 s")
            win44 = np.stack([to_rate(win[:, c], rate) for c in range(win.shape[1])], 1)
            if win44.shape[1] == 1:
                win44 = np.repeat(win44, 2, 1)
            mix_path = self.staging / "mix.wav"
            mix_path.write_bytes(wav_float32_bytes(win44, SR))
            self.progress("read", 0.04, f"janela {start:g}s +{len(win44) / SR:.2f}s")

            stems = self.separate(mix_path)
            self.progress("separate", 0.56, "stems prontos")

            def mono(path):
                sinfo, st = read_wav_np(path)
                return to_rate(st.mean(1), sinfo["sampleRate"])

            drums = mono(stems["drums"])
            kicks, kick_detector = adtof_kicks(stems["drums"], self.staging)
            if kicks is None:
                kicks = kick_onsets(drums)
            bpm, bpm_source = params.get("bpm"), "provided"
            if bpm is None and params.get("bpmSource") == "filename":
                bpm, bpm_source = bpm_from_name(mix_ref["path"]), "filename"
            if bpm is None:
                bpm, bpm_source = estimate_bpm(kicks), "kick"
            if bpm is None:
                raise ContractError("BACKEND_FAILED", "no BPM: not provided, not in the file name, <8 kicks",
                                    {"phase": "transcribe", "kicks": int(len(kicks))})
            bpm = float(bpm)
            kicks = dedupe_cells(kicks, bpm)
            phase = grid_phase(kicks, bpm) if len(kicks) else 0.0
            if "bass" in stems:  # rolls land in the bass stem: look at drums + bass
                low = drums + mono(stems["bass"])[: len(drums)] if len(mono(stems["bass"])) >= len(drums) else drums
                rolls = kick_rolls(low, bpm, phase, len(drums) / SR)
                if len(rolls):
                    kicks = dedupe_cells(np.sort(np.concatenate([kicks, rolls])), bpm)
                counts_rolls = int(len(rolls) // 8)
            else:
                counts_rolls = 0
            self.progress("transcribe", 0.6, f"{len(kicks)} kicks, {bpm:g} BPM ({bpm_source})")

            tracks_s, counts = {}, {}
            if "drums" in parts:
                tracks_s["kick"] = kicks
            if "bass" in parts:
                bass_x = mono(stems["bass"])
                raw = [n[:4] for n in basic_pitch_notes(stems["bass"], 30, 400)]
                counts["bassRaw"] = len(raw)
                cut = split_on_onsets(raw, bass_x, bpm, phase) if params.get("splitOnOnsets", True) else []
                tracks_s["bass"] = cut or raw
                self.progress("transcribe", 0.75, f"baixo: {len(raw)} notas Basic Pitch, {len(cut)} re-cortadas")
            if "other" in parts:
                raw = basic_pitch_notes(stems["other"], 150, 5000)
                counts["leadRaw"] = len(raw)
                # leadFilter now defaults to OFF: on the controlled set it cost recall (t1 0.79 -> 0.57) and on the
                # template renders it changed nothing (ablation, MIDI-MAKER.md 10.3); `true` still turns it on.
                lead = clean_lead(raw, mono(stems["other"]), bpm) if params.get("leadFilter", False) else [n[:4] for n in raw]
                tracks_s["lead"] = lead
                self.progress("transcribe", 0.85, f"lead: {len(raw)} -> {len(lead)} depois do filtro")

            self.progress("quantize", 0.88, f"grelha {quantize} = {media.grid_ticks(quantize)} ticks a PPQ 960")
            kick_note = int(params["kickNote"])
            tracks = []
            if "kick" in tracks_s:
                tracks.append(("kick", 9, place_kicks(tracks_s["kick"], bpm, phase, quantize, kick_note)))
            if "bass" in tracks_s:
                tracks.append(("bass", 1, place(tracks_s["bass"], bpm, phase, quantize, mono=params["bassMono"])))
            if "lead" in tracks_s:
                lead = place(tracks_s["lead"], bpm, phase, quantize)
                by = {}  # at most 2 voices per slot, the highest (always: 'Guitar Pro' lead, not only with the filter)
                for n in lead:
                    by.setdefault(n[0], []).append(n)
                lead = sorted(x for v in by.values() for x in sorted(v, key=lambda n: -n[2])[:2])
                tracks.append(("lead", 2, tidy_lengths(lead, quantize)))
            if not tracks or not any(t[2] for t in tracks):
                raise ContractError("BACKEND_FAILED", "no notes found in the requested parts",
                                    {"phase": "quantize", "parts": parts})
            self.progress("quantize", 0.9)

            self.progress("write", 0.92)
            outputs = []
            mid = smf_bytes(bpm, tracks)
            check_output_size("midi", len(mid))
            mid_path = self.output_dir / "transcription.mid"
            media.atomic_write(mid_path, mid, self.staging)
            mid_info = media.read_midi(mid_path)  # proves 960 + tempo map + duration on the published bytes
            outputs.append({"role": "midi", "path": str(mid_path), "bytes": len(mid),
                            "sha256": media.sha256_file(mid_path), "format": media.midi_format_of(mid_info)})
            if params.get("includeStems"):
                for part in parts:
                    if part not in stems:
                        continue
                    data = stems[part].read_bytes()
                    check_output_size(f"stem_{part}", len(data))
                    sp = self.output_dir / f"stem-{part}.wav"
                    media.atomic_write(sp, data, self.staging)
                    sinfo, _ = read_wav_np(sp)
                    outputs.append({"role": f"stem_{part}", "path": str(sp), "bytes": len(data),
                                    "sha256": media.sha256_file(sp), "format": sinfo})
            notes_by = {name: len(n) for name, _, n in tracks}
            metrics = {"synthetic": bool(self.request.get("synthetic", False)), "backend": "demucs-htdemucs+basic-pitch-onnx",
                       "device": "cpu", "onsetsDetected": int(len(kicks)), "kickDetector": kick_detector, "kickRollBeats": counts_rolls,
                       "notesWritten": sum(notes_by.values()), "notesByPart": notes_by, **counts,
                       "mergedEvents": 0, "gridTicks": media.grid_ticks(quantize), "quantize": quantize,
                       "ppq": media.PPQ, "bpmUsed": bpm, "bpmSource": bpm_source,
                       "gridPhaseSeconds": round(phase, 6), "windowStartSeconds": start,
                       "windowSeconds": round(len(win44) / SR, 6),
                       "wallSeconds": round(time.monotonic() - t0, 3)}
            env_fp = environment(params)
            report = {"schemaVersion": "1.0", "taskId": self.task_id, "metrics": metrics, "params": params,
                      "environment": env_fp, "validation": "unreviewed"}
            rdata = json.dumps(report, ensure_ascii=True, indent=2).encode()
            rp = self.output_dir / "report.json"
            media.atomic_write(rp, rdata, self.staging)
            outputs.append({"role": "report", "path": str(rp), "bytes": len(rdata), "sha256": media.sha256_file(rp),
                            "format": {"container": "json", "bytes": len(rdata)}})
            manifest = {"schemaVersion": "1.0", "contract": CONTRACT, "taskId": self.task_id,
                        "requestId": self.request_id, "profile": "midi", "operation": "transcribe",
                        "createdAt": now(), "service": dict(SERVICE), "params": params,
                        "inputs": [{"role": r["role"], "path": r["path"], "bytes": Path(r["path"]).stat().st_size,
                                    "sha256": media.sha256_file(Path(r["path"]))} for r in self.request["inputs"]],
                        "outputs": outputs, "metrics": metrics, "environment": env_fp, "validation": "unreviewed",
                        "synthetic": bool(self.request.get("synthetic", False))}
            mp = self.output_dir / "manifest.json"
            media.atomic_write(mp, json.dumps(manifest, ensure_ascii=True, indent=2).encode(), self.staging)
            self.clean()
            self.progress("write", 1.0, "manifesto publicado")
            emit_line({"event": "done", "schemaVersion": "1.0", "contract": CONTRACT, "taskId": self.task_id,
                       "requestId": self.request_id,
                       "result": {"outputs": outputs, "manifestPath": str(mp), "metrics": metrics,
                                  "validation": "unreviewed"},
                       "synthetic": bool(self.request.get("synthetic", False)), "at": now()})
            return 0
        except ContractError as e:
            return self.fail(e)
        except media.MediaError as e:
            return self.fail(ContractError(e.code, str(e)))
        except MemoryError:
            return self.fail(ContractError("OUT_OF_MEMORY", "MemoryError (no retry, no offload)"))
        except Exception as e:  # any unexpected failure is reported, never fake success
            return self.fail(ContractError("BACKEND_FAILED", f"{type(e).__name__}: {e}"))

    def fail(self, err):
        try:
            self.unpublish()
            self.clean()
        except Exception as e:  # noqa: BLE001 - the error line must still go out
            log(f"cleanup after error failed: {e}")
        emit_line({"event": "error", "schemaVersion": "1.0", "contract": CONTRACT, "taskId": self.task_id,
                   "requestId": self.request_id, "error": err.to_dict(self.request_id), "at": now()})
        return 2

    def clean(self):
        """Remove .staging/<taskId> (Demucs stems live there too) and .staging if it ends up empty."""
        import shutil
        shutil.rmtree(self.staging, ignore_errors=True)
        parent = self.staging.parent
        try:
            if parent.name == ".staging" and parent.exists() and not any(parent.iterdir()):
                parent.rmdir()
        except OSError:
            pass
        if self.staging.exists():
            raise ContractError("BACKEND_FAILED", f"could not clean staging {self.staging}")

    def unpublish(self):
        """An error publishes nothing (CONTRACT.md §4/§7): remove finals this task already renamed in."""
        for name in ("manifest.json", "report.json", "transcription.mid") + tuple(
                f"stem-{p}.wav" for p in ("drums", "bass", "other", "vocals")):
            p = self.output_dir / name
            if p.is_file() and p.stat().st_mtime >= self.started:
                try:
                    p.unlink()
                except OSError:
                    pass

    started = 0.0


# ---- request validation ----

def validate(reg, request):
    if request.get("schemaVersion") not in (None, "1.0"):
        raise ContractError("UNSUPPORTED_SCHEMA", "only schemaVersion 1.0 is supported")
    errors = validation_errors(reg, "service-task-request", request)
    if errors:
        raise ContractError("SCHEMA_INVALID", f"service-task-request: {len(errors)} error(s)", {"errors": errors[:20]})
    if request["profile"] != "midi" or request["operation"] != "transcribe":
        raise ContractError("UNSUPPORTED_OPERATION", f"{request['profile']}/{request['operation']} not offered")
    check_parts(request["params"]["parts"])
    out = Path(request["outputDir"])
    if not out.is_dir():
        raise ContractError("BROKEN_REFERENCE", f"outputDir does not exist: {out}", {"outputDir": str(out)})
    for ref in request["inputs"]:
        p = Path(ref["path"])
        if not p.is_file():
            raise ContractError("BROKEN_REFERENCE", f"input does not exist: {p}", {"role": ref["role"], "path": str(p)})
        size = p.stat().st_size
        if size > MAX_WAV_BYTES:
            raise ContractError("LIMIT_EXCEEDED", f"input is {size} bytes (cap {MAX_WAV_BYTES})", {"role": ref["role"]})
        if "bytes" in ref and ref["bytes"] != size:
            raise ContractError("HASH_MISMATCH", f"declared bytes {ref['bytes']} != actual {size}", {"role": ref["role"]})
        if "sha256" in ref:
            actual = media.sha256_file(p)
            if actual != ref["sha256"]:
                raise ContractError("HASH_MISMATCH", f"sha256 mismatch for {p}",
                                    {"role": ref["role"], "expected": ref["sha256"], "actual": actual})
    mix = next(r for r in request["inputs"] if r["role"] == "mix")
    with open(mix["path"], "rb") as f:
        head = f.read(12)
    if head[:4] != b"RIFF" or head[8:12] != b"WAVE":
        raise ContractError("UNSUPPORTED_FORMAT", "mix is not a RIFF/WAVE file")


def job_main():
    reg = Registry(SCHEMA_DIR)
    task_id = "t-" + uuid.uuid4().hex[:12]
    raw = sys.stdin.readline()

    def fail(err, rid="unknown"):
        emit_line({"event": "error", "schemaVersion": "1.0", "contract": CONTRACT, "taskId": task_id,
                   "requestId": rid, "error": err.to_dict(rid), "at": now()})
        return 2

    if not raw.strip():
        return fail(ContractError("INVALID_JSON", "no request line on stdin"))
    if len(raw.encode("utf-8")) > LIMITS["maxResponseBytes"]:
        return fail(ContractError("LIMIT_EXCEEDED", "request line exceeds 2 MiB"))
    try:
        request = json.loads(raw)
    except ValueError as e:
        return fail(ContractError("INVALID_JSON", f"request line is not JSON: {e}"))
    if not isinstance(request, dict):
        return fail(ContractError("SCHEMA_INVALID", "request line must be a JSON object"))
    rid = request.get("requestId") if isinstance(request.get("requestId"), str) and \
        re.fullmatch(r"[A-Za-z0-9._:-]{1,128}", request.get("requestId", "")) else "unknown"
    try:
        validate(reg, request)
    except ContractError as e:
        return fail(e, rid)
    job = Job(request, task_id)
    job.started = time.time() - 1
    emit_line({"event": "start", "schemaVersion": "1.0", "contract": CONTRACT, "taskId": task_id,
               "requestId": rid, "profile": "midi", "operation": "transcribe", "service": dict(SERVICE),
               "synthetic": bool(request.get("synthetic", False)), "at": now()})
    return job.run()


def hello():
    emit_line({"schemaVersion": "1.0", "contract": CONTRACT, "service": {**SERVICE, "pid": os.getpid()},
               "profiles": [{"name": "midi", "operations": ["transcribe"]}], "limits": dict(LIMITS),
               "ppq": media.PPQ, "quantizeGrid": list(GRID_ORDER), "authRequired": False, "startedAt": now()})
    return 0


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description="AICore-C S2 audio->MIDI service (aicore.service/1)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("hello")
    sub.add_parser("job")
    args = ap.parse_args(argv)
    protect_stdout()
    lower_priority()
    if args.cmd == "hello":
        return hello()
    return job_main()


if __name__ == "__main__":
    sys.exit(main())
