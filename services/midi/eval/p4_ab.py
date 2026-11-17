"""P4 A/B (owner's protocol, docs/MIDI-MAKER.md §11): Basic Pitch vs YourMT3 (pretrained, CPU), same stems.

Stems = the baseline evaluation stems (D:\\AI Dev\\midi-maker\\avaliacao\\stems\\<window>, READ-ONLY, one Demucs
run reused by every arm). Grid phase = the P1 service run (<work>\\depois-1-32\\<case>\\report.json).
Decoder/quantize = the service's (place(), 2-voice cap when leadFilter is on, bass mono). Truth = the template
notes, written and calibrated to the SOUNDING pitch (CALIBRATION below, measured, declared in the report).

  python p4_ab.py --work <dir> [--mt3 <dir with <case>-<input>.json>]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pickle
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
import aicore_midi_service as s  # noqa: E402
import note_metrics as nm  # noqa: E402
import score_templates as st  # noqa: E402

STEMS = Path(r"D:\AI Dev\midi-maker\avaliacao\stems")
# Sounding-pitch calibration, measured 2026-11-16 (MIDI-MAKER.md 10.1/10.2):
#  lead "FM answer" (Core builtin 'bass' engine): harmonic comb on the other stem, 107/108 notes at -12;
#  bass remember / t1: f0 of the bass stem -84 / -68 cents vs written (controlled Vita render of the same
#  notes transcribes at 0, so it is the render) -> -1; wakeup / t2: +17 / +18 cents -> 0.
CALIBRATION = {"track": {"fm answer": -12}, "bassByCase": {"remember": -1, "t1": -1}}
BASS_PROG = range(32, 40)
SYNTH_LEAD_PROG = range(80, 88)


def truth(doc, part, case, start, length, calibrated):
    out = []
    for tr in doc["tracks"]:
        k = tr["name"].lower()
        if not st.PARTS[part](k):
            continue
        off = 0
        if calibrated:
            off = CALIBRATION["track"].get(k, 0) + (CALIBRATION["bassByCase"].get(case, 0) if part == "bass" else 0)
        out += [(a, p + off, d) for a, p, d in st.truth(doc, lambda q, k=k: q == k, start, length)]
    return sorted(out)


def placed(notes, bpm, phase, grid, mono=False, cap2=False):
    tps = 960 * bpm / 60
    rows = s.place(notes, bpm, phase, grid, mono=mono)
    if cap2:
        by = {}
        for q in rows:
            by.setdefault(q[0], []).append(q)
        rows = sorted(q for v in by.values() for q in sorted(v, key=lambda n: -n[2])[:2])
    return [(phase + t / tps, p, d / tps) for t, d, p, v in rows]


def lead_select(notes, bpm, phase, register=19):
    """Lead selection DECLARED A PRIORI (thread, 2026-11-16 07:36, before any MT3 run), same for every window:
    candidates = non-drum, non-bass notes; keep pitches within +-19 semitones of the duration-weighted
    median pitch; per 1/32 cell keep only the highest note (skyline)."""
    import numpy as np
    if not notes:
        return []
    p = np.array([n[2] for n in notes], float)
    w = np.array([max(n[1] - n[0], 1e-3) for n in notes])
    order = np.argsort(p)
    cum = np.cumsum(w[order])
    med = p[order][int(np.searchsorted(cum, cum[-1] / 2))]
    cell = 60 / bpm / 8
    best = {}
    for n in notes:
        if abs(n[2] - med) > register:
            continue
        k = int(round((n[0] - phase) / cell))
        if k not in best or n[2] > best[k][2]:
            best[k] = n
    return sorted(best.values())


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()[:16]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True, help="dir with depois-1-32/<case>/report.json and cache/")
    ap.add_argument("--mt3", default=None)
    a = ap.parse_args()
    work = Path(a.work)
    cache = work / "cache"
    cache.mkdir(exist_ok=True)
    rows, stems_sha = [], {}
    for case, (name, start, length) in st.CASES.items():
        sd = STEMS / f"{name}-{int(start)}"
        for f in ("mix.wav", "mix - other.wav", "mix - bass.wav"):
            stems_sha[f"{case}/{f}"] = sha(sd / f)
        doc = json.loads((st.TEMPLATES / f"{name}.core-template.json").read_text(encoding="utf-8"))["document"]
        bpm = doc["transport"]["bpm"]
        tps = 960 * bpm / 60
        phase = json.loads((work / "depois-1-32" / case / "report.json").read_text())["metrics"]["gridPhaseSeconds"]
        other = s.to_rate(s.read_wav_np(str(sd / "mix - other.wav"))[1].mean(1), 44100)
        bass_x = s.to_rate(s.read_wav_np(str(sd / "mix - bass.wav"))[1].mean(1), 44100)
        cp = cache / f"{case}-other-notes.pkl"
        if not cp.is_file():
            pickle.dump(s.basic_pitch_notes(sd / "mix - other.wav", 150, 5000), open(cp, "wb"))
        bp_lead = pickle.load(open(cp, "rb"))
        cb = cache / f"{case}-bass-notes.pkl"
        if not cb.is_file():
            pickle.dump([n[:4] for n in s.basic_pitch_notes(sd / "mix - bass.wav", 30, 400)], open(cb, "wb"))
        bp_bass_raw = pickle.load(open(cb, "rb"))
        bp_bass = s.split_on_onsets(bp_bass_raw, bass_x, bpm, phase) or bp_bass_raw
        arms = {"BP filter on": {"lead": (s.clean_lead(bp_lead, other, bpm), True), "bass": (bp_bass, False)},
                "BP filter off": {"lead": ([n[:4] for n in bp_lead], False), "bass": (bp_bass, False)}}
        if a.mt3:
            def mt3(inp, sel):
                f = Path(a.mt3) / f"{case}-{inp}.json"
                if not f.is_file():
                    return None
                return [(on, off, p, v / 127) for on, off, p, prog, drum, v in json.loads(f.read_text())["notes"]
                        if sel(prog, drum)]

            def pitched(prog, drum):
                return not drum and prog not in BASS_PROG

            def synth(prog, drum):
                return not drum and prog in SYNTH_LEAD_PROG

            def bassp(prog, drum):
                return not drum and prog in BASS_PROG

            def anyp(prog, drum):
                return not drum

            for inp in ("other", "mix"):
                lead_all, lead_syn = mt3(inp, pitched), mt3(inp, synth)
                bass_n = mt3("bass", anyp) if inp == "other" else mt3("mix", bassp)
                if lead_all is None:
                    continue
                arms[f"MT3 {inp} filter on"] = {
                    "lead": (s.clean_lead([n + (None,) for n in lead_all], other, bpm), True), "bass": (bass_n, False)}
                arms[f"MT3 {inp} filter off"] = {"lead": (lead_all, False), "bass": (bass_n, False)}
                arms[f"MT3 {inp} synth-lead only"] = {"lead": (lead_syn, False), "bass": (bass_n, False)}
                arms[f"MT3 {inp} lead-select"] = {"lead": (lead_select(lead_all, bpm, phase), False),
                                                  "bass": (bass_n, False)}
        for arm, parts in arms.items():
            for part, (notes, cap2) in parts.items():
                if notes is None:
                    continue
                for grid in ("1/32", "1/16"):
                    made = placed(notes, bpm, phase, grid, mono=(part == "bass"), cap2=cap2)
                    tol = 60 / bpm / (16 if grid == "1/32" else 8)
                    for cal in (True, False):
                        m = nm.protocol(truth(doc, part, case, start, length, cal), made, tol, tps)
                        rows.append({"case": case, "arm": arm, "part": part, "grid": grid,
                                     "reference": "calibrated" if cal else "written", **m})
    out = {"kind": "p4-ab", "calibration": CALIBRATION, "stemsSha256_16": stems_sha, "rows": rows}
    (work / "p4-ab.json").write_text(json.dumps(out, indent=1), encoding="utf-8")
    print(f"{'case':9} {'arm':24} {'part':4} {'grid':4} {'ref':4} {'D':>4} {'T':>4} {'P':>6} {'R':>6} {'F1on':>6} "
          f"{'F1full':>6} {'J':>5} {'endMed':>6}")
    for r in rows:
        if r["grid"] == "1/32" or r["reference"] == "calibrated":
            em = r["endErrTicksMed"]
            print(f"{r['case']:9} {r['arm']:24} {r['part']:4} {r['grid']:4} {r['reference'][:4]:4} {r['D']:4} {r['T']:4} "
                  f"{r['P']:6.3f} {r['R']:6.3f} {r['F1onset']:6.3f} {r['F1full']:6.3f} {r['J'] or 0:5.2f} "
                  f"{em if em is not None else '-':>6}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
