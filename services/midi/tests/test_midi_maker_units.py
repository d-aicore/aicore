"""CI unit tests for the MIDI maker (CPU only, no models, no Demucs, no Basic Pitch, no downloads).

Run from the repository root:
    python -m unittest discover -s services/midi/tests -v
Needs numpy (+ mido for the MIDI round trip; that test is skipped without it).

They pin the behaviour measured on 2026-11-16 (docs: D:\\AICore\\docs\\MIDI-MAKER.md §10):
- the kick time is refined to the attack (the old detector was 16-36 ms early);
- grid phase = circular median modulo a 1/16 + downbeat slot, robust to off-beat kicks;
- refractory tied to the grid cell (1/32 rolls are not merged at 210 BPM);
- lead lengths: end at the next start at the latest, whole grid cells;
- metrics: pitch-exact matching, onset/offset tolerance, greedy one-to-one.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parent / "eval"))

import numpy as np  # noqa: E402

import aicore_midi_service as svc  # noqa: E402
import note_metrics as nm  # noqa: E402

SR = svc.SR


def synth_kick(times, seconds, decay=0.05, seed=1):
    """Pitch-swept sine kick (150 -> 45 Hz) with a short click, plus a little noise."""
    rng = np.random.default_rng(seed)
    x = rng.normal(0, 1e-4, int(seconds * SR)).astype(np.float32)
    t = np.arange(int(0.12 * SR)) / SR
    f = 45 + 105 * np.exp(-t / 0.015)
    k = np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t / decay)
    k[:40] += np.hanning(80)[:40] * 0.3  # click
    for a in times:
        i = int(round(a * SR))
        n = min(len(k), len(x) - i)
        if n > 0:
            x[i:i + n] += 0.8 * k[:n].astype(np.float32)
    return x


class KickTiming(unittest.TestCase):
    def test_refined_onsets_are_on_time(self):
        bpm = 210.0
        beat = 60 / bpm
        times = [0.25 + i * beat for i in range(24)]
        x = synth_kick(times, 0.25 + 24 * beat + 0.5)
        found = svc.dedupe_cells(svc.kick_onsets(x), bpm)
        self.assertGreaterEqual(len(found), 22)
        errs = [min(abs(f - t) for f in found) for t in times]
        self.assertLess(float(np.median(errs)), 0.004, "kick onsets must be within 4 ms (old detector: 16-36 ms early)")

    def test_cell_refractory_keeps_1_32_rolls(self):
        bpm = 210.0
        cell = 60 / bpm / 8  # 35.7 ms
        out = svc.dedupe_cells(np.array([1.0, 1.0 + cell, 1.0 + 2 * cell, 1.0 + 2 * cell + 0.01]), bpm)
        self.assertEqual(len(out), 3, "1/32 hits are kept, a hit 10 ms later is merged")


class KickDetectorChoice(unittest.TestCase):
    def test_falls_back_and_says_so_when_adtof_is_missing(self):
        saved = svc.ADTOF_PY
        svc.ADTOF_PY = str(HERE / "no-such-python.exe")
        try:
            kicks, how = svc.adtof_kicks(HERE / "missing.wav", HERE)
        finally:
            svc.ADTOF_PY = saved
        self.assertIsNone(kicks)
        self.assertEqual(how, "onset (ADTOF not installed)")


class GridPhase(unittest.TestCase):
    def test_offbeat_kicks_do_not_drag_the_phase(self):
        bpm = 200.0
        beat = 60 / bpm
        step = beat / 4
        true_phase = 0.071
        on = [true_phase + i * beat for i in range(32)]
        off = [true_phase + i * beat + 3 * step for i in range(24)]  # rolling hi-tech kicks on the 4th 1/16
        jitter = np.random.default_rng(3).normal(0, 0.002, len(on) + len(off))
        ph = svc.grid_phase(np.sort(np.array(on + off) + jitter), bpm)
        self.assertLess(abs(ph - true_phase), 0.004)

    def test_downbeat_slot(self):
        bpm = 180.0
        beat = 60 / bpm
        ph = svc.grid_phase(np.array([0.05 + beat / 4 + i * beat for i in range(16)]), bpm)
        self.assertAlmostEqual(ph, 0.05 + beat / 4, places=3)


class LeadLengths(unittest.TestCase):
    def test_end_at_next_start_and_whole_cells(self):
        g = 120  # 1/32 at PPQ 960
        rows = [(0, 700, 60, 100), (240, 500, 62, 100), (960, 50, 64, 100)]
        out = svc.tidy_lengths(rows, "1/32")
        self.assertEqual(out[0][1], 240, "ends at the next start")
        self.assertEqual(out[1][1], 480, "rounded to whole cells, capped by the next start")
        self.assertEqual(out[2][1], g, "at least one cell")
        self.assertTrue(all(d % g == 0 for _, d, _, _ in out))

    def test_place_snaps_starts(self):
        bpm, phase = 210.0, 0.1
        tps = 960 * bpm / 60
        rows = svc.place([(phase + 130 / tps, phase + 400 / tps, 60, 0.8)], bpm, phase, "1/32")
        self.assertEqual(rows[0][0], 120)


class Metrics(unittest.TestCase):
    def test_pitch_exact_and_offset(self):
        true = [(1.0, 60, 0.1), (2.0, 62, 0.1)]
        pred = [(1.005, 60, 0.1), (2.004, 50, 0.1), (2.006, 62, 0.4)]
        m = nm.protocol(true, pred, tol=0.0178, tps=960 * 210 / 60)
        self.assertEqual(m["TP"], 2)
        self.assertEqual(m["FP"], 1)
        self.assertAlmostEqual(m["F1full"], 2 * 1 / 5, places=3)  # second hit ends 0.3 s late

    def test_octave_error_is_a_miss(self):
        m = nm.protocol([(1.0, 60, 0.1)], [(1.0, 48, 0.1)], tol=0.018, tps=1000)
        self.assertEqual(m["TP"], 0)
        self.assertEqual(m["pitchResiduals"], {"-12": 1})

    def test_one_to_one(self):
        pairs = nm.match([(1.0, 60, 0.1)], [(1.0, 60, 0.1), (1.001, 60, 0.1)], 0.02, pitch_strict=True)
        self.assertEqual(len(pairs), 1)


class MidiWriter(unittest.TestCase):
    def test_ppq_960_and_ticks(self):
        data = svc.smf_bytes(210.0, [("lead", 2, [(0, 120, 60, 100), (240, 240, 62, 90)])])
        self.assertEqual(data[:4], b"MThd")
        self.assertEqual(int.from_bytes(data[12:14], "big"), 960)
        try:
            import io
            import mido
        except ImportError:
            self.skipTest("mido not installed")
        mf = mido.MidiFile(file=io.BytesIO(data))
        ons, now = [], 0
        for msg in mf.tracks[1]:
            now += msg.time
            if msg.type == "note_on" and msg.velocity:
                ons.append((now, msg.note))
        self.assertEqual(ons, [(0, 60), (240, 62)])

    def test_refuses_480(self):
        with self.assertRaises(svc.ContractError):
            svc.smf_bytes(120.0, [], ppq=480)


if __name__ == "__main__":
    unittest.main()


class AuditFixes(unittest.TestCase):
    """Audit 2026-11-16 (services/midi items). Each runs the real request path, no models."""

    def setUp(self):
        import tempfile
        self.tmp = Path(tempfile.mkdtemp(prefix="aicore-midi-test-"))
        self.wav = self.tmp / "one-second.wav"
        x = np.zeros((SR, 2), np.float32)
        x[::100] = 0.1
        self.wav.write_bytes(svc.wav_float32_bytes(x, SR))
        self.events = []
        self._emit = svc.emit_line
        svc.emit_line = self.events.append

    def tearDown(self):
        import shutil
        svc.emit_line = self._emit
        shutil.rmtree(self.tmp, ignore_errors=True)

    def request(self, **params):
        base = {"ppq": 960, "quantize": "1/32", "parts": ["drums", "bass", "other"], "kickNote": 36,
                "bassMono": True, "bpm": 210.0, "bpmSource": "provided"}
        base.update(params)
        return {"schemaVersion": "1.0", "requestId": "unit-1", "profile": "midi", "operation": "transcribe",
                "params": base, "inputs": [{"role": "mix", "path": str(self.wav)}], "outputDir": str(self.tmp)}

    def test_window_past_the_end_is_an_error(self):
        job = svc.Job(self.request(startSeconds=0.5, lengthSeconds=5.0), "t-unit")
        job.started = 0
        self.assertEqual(job.run(), 2)
        err = [e for e in self.events if e.get("event") == "error"]
        self.assertEqual(err[-1]["error"]["code"], "LIMIT_EXCEEDED")
        self.assertFalse((self.tmp / "transcription.mid").exists())

    def test_vocals_part_is_refused_before_work(self):
        from jsonschema_mini import Registry
        reg = Registry(svc.SCHEMA_DIR)
        with self.assertRaises(svc.ContractError) as cm:
            svc.validate(reg, self.request(parts=["drums", "vocals"]))
        self.assertEqual(cm.exception.code, "UNSUPPORTED_OPERATION")
        svc.validate(reg, self.request())  # the normal request still passes

    def test_output_caps(self):
        self.assertGreaterEqual(svc.LIMITS["maxOutputBytes"], svc.MAX_WAV_BYTES, "hello must cover stem WAVs")
        svc.check_output_size("stem_bass", svc.MAX_MIDI_BYTES + 1)  # a stem bigger than 8 MiB is fine
        with self.assertRaises(svc.ContractError):
            svc.check_output_size("midi", svc.MAX_MIDI_BYTES + 1)
        with self.assertRaises(svc.ContractError):
            svc.check_output_size("stem_bass", svc.MAX_WAV_BYTES + 1)

    def test_eval_out_dir_is_absolute(self):
        try:
            import score_templates
        except ImportError:
            self.skipTest("mido not installed")
        self.assertTrue(score_templates.resolve_out("relative/dir").is_absolute())


def synth_bass(notes, seconds, f0=55.0, release=0.12):
    """Saw-ish bass: each (start, end) note is a gated tone with an exponential release after the gate."""
    x = np.zeros(int(seconds * SR), np.float32)
    for a, e in notes:
        n = int((e - a + 5 * release) * SR)
        t = np.arange(n) / SR
        tone = sum(np.sin(2 * np.pi * k * f0 * t) / k for k in range(1, 6))
        env = np.where(t < e - a, 1.0, np.exp(-(t - (e - a)) / release))
        i = int(a * SR)
        m = min(n, len(x) - i)
        x[i:i + m] += (0.3 * tone * env)[:m].astype(np.float32)
    return x


class BassTails(unittest.TestCase):
    def test_release_tail_is_not_a_new_note(self):
        bpm, phase = 200.0, 0.1
        step = 60 / bpm / 4
        # notes on every other 1/16 (a gap after each one), each 0.9 of a step long
        starts = [phase + 2 * k * step for k in range(24)]
        x = synth_bass([(a, a + 0.9 * step) for a in starts], phase + 50 * step)
        bp = [(a, a + 0.9 * step, 33, 0.8) for a in starts]  # what Basic Pitch would give
        cut = svc.split_on_onsets(bp, x, bpm, phase)
        self.assertLessEqual(len(cut), len(starts) + 1, "a decaying tail must not become a note one 1/16 later")
        self.assertGreaterEqual(len(cut), len(starts) - 1, "the real notes are kept")


class KickRolls(unittest.TestCase):
    def test_roll_beat_gets_eight_kicks_and_plain_beats_none(self):
        bpm, phase = 210.0, 0.05
        beat = 60 / bpm
        plain = [phase + i * beat for i in range(8)]
        roll = [phase + 8 * beat + k * beat / 8 for k in range(8)]
        x = synth_kick(plain + roll + [phase + 9 * beat], phase + 11 * beat, decay=0.008)
        found = svc.kick_rolls(x, bpm, phase, phase + 11 * beat)
        self.assertEqual(len(found), 8)
        self.assertTrue(all(abs(f - r) < 0.002 for f, r in zip(found, roll)))
