"""Proof that the MIDI maker unit tests really test (owner's rule: a test must fail when the behaviour breaks).

Each mutation below breaks ONE behaviour in-process (the files are never touched) and runs the suite; the suite
must FAIL for every mutation and PASS with no mutation. Exit 0 = every mutation caught; exit 1 = a mutation
survived (that test is decorative) or the clean run failed.

    python services/midi/tests/mutations.py
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parent / "eval"))

import numpy as np  # noqa: E402

import aicore_midi_service as svc  # noqa: E402
import note_metrics as nm  # noqa: E402
import test_midi_maker_units as units  # noqa: E402

import score_templates  # noqa: E402

ORIG = {"BASS_TAIL_DECAY": svc.BASS_TAIL_DECAY, "kick_rolls": svc.kick_rolls, "ROLL_RATIO": svc.ROLL_RATIO,
        "resolve_out": score_templates.resolve_out, "check_window": svc.check_window,
        "check_parts": svc.check_parts, "check_output_size": svc.check_output_size,
        "LIMITS": dict(svc.LIMITS), "adtof_kicks": svc.adtof_kicks, "kick_onsets": svc.kick_onsets, "dedupe_cells": svc.dedupe_cells, "grid_phase": svc.grid_phase,
        "tidy_lengths": svc.tidy_lengths, "place": svc.place, "smf_bytes": svc.smf_bytes, "match": nm.match}


def old_grid_phase(onsets, bpm):  # the baseline rule: circular mean modulo the beat
    beat = 60 / bpm
    ang = np.angle(np.mean(np.exp(2j * np.pi * (onsets / beat))))
    return float((ang / (2 * np.pi)) % 1 * beat)


def match_any_pitch(true, pred, tol, pitch_strict=False, offset=False, offset_tol=None):
    return ORIG["match"](true, pred, tol, False, offset, offset_tol)


def match_not_one_to_one(true, pred, tol, pitch_strict=False, offset=False, offset_tol=None):
    out = []
    for i, (t, p, d) in enumerate(true):
        for j, (u, q, e) in enumerate(pred):
            if abs(u - t) <= tol and (not pitch_strict or p == q):
                out.append((i, j, u - t))
    return out


MUTATIONS = {
    "kick time at the frame start (old 16-36 ms early)": ("kick_onsets", lambda x: ORIG["kick_onsets"](x) - 0.03),
    "fixed 60 ms refractory": ("dedupe_cells", lambda on, bpm, cell="1/32": np.array(
        [t for i, t in enumerate(on) if i == 0 or t - on[i - 1] > 0.06])),
    "grid phase = circular mean (old)": ("grid_phase", old_grid_phase),
    "lead lengths untouched": ("tidy_lengths", lambda rows, q, ppq=960: rows),
    "starts not snapped": ("place", lambda notes, bpm, phase, q, mono=False, ppq=960: [
        (int(round((s - phase) * ppq * bpm / 60)), max(1, int(round((e - s) * ppq * bpm / 60))), p, 100)
        for s, e, p, a in notes]),
    "silent ADTOF fallback (no label)": ("adtof_kicks", lambda wav, staging: (None, "onset")),
    "bass release tails become notes (no tail rule)": ("BASS_TAIL_DECAY", 0.0),
    "kick roll detector removed": ("kick_rolls", lambda x, bpm, phase, seconds: np.array([])),
    "kick roll detector fires on every beat": ("ROLL_RATIO", 0.0),
    "window past the end cut silently": ("check_window", lambda duration, start, length: None),
    "vocals accepted and dropped": ("check_parts", lambda parts: None),
    "stem size unchecked / midi uncapped": ("check_output_size", lambda role, n: None),
    "hello says 8 MiB output": ("LIMITS", dict(svc.LIMITS, maxOutputBytes=svc.MAX_MIDI_BYTES)),
    "eval --out kept relative": ("resolve_out", lambda arg: Path(arg)),
    "PPQ 480 accepted": ("smf_bytes", lambda bpm, tracks, ppq=960: ORIG["smf_bytes"](bpm, tracks, 960)),
    "matching ignores pitch": ("match", match_any_pitch),
    "matching not one-to-one": ("match", match_not_one_to_one),
}


def run_suite():
    suite = unittest.defaultTestLoader.loadTestsFromModule(units)
    res = unittest.TextTestRunner(stream=open(__import__("os").devnull, "w"), verbosity=0).run(suite)
    return res.wasSuccessful(), len(res.failures) + len(res.errors)


def patch(name, fn):
    if name == "match":
        nm.match = fn
    elif name == "resolve_out":
        score_templates.resolve_out = fn
    else:
        setattr(svc, name, fn)


def main():
    ok, _ = run_suite()
    print(f"clean run: {'PASS' if ok else 'FAIL'}")
    bad = not ok
    for label, (name, fn) in MUTATIONS.items():
        patch(name, fn)
        try:
            passed, nfail = run_suite()
        finally:
            patch(name, ORIG[name])
        caught = not passed
        bad |= not caught
        print(f"[{'caught' if caught else 'SURVIVED'}] {label}" + (f" ({nfail} test(s) failed)" if caught else ""))
    print(f"mutations caught: {len(MUTATIONS)}/{len(MUTATIONS)}" if not bad
          else "RED: a mutation survived or the clean run failed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
