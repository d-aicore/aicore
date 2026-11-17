"""P5 step 1 (owner's protocol, MIDI-MAKER.md §11): does a finer separation (htdemucs_6s: other/piano/guitar)
raise the lead recall of the SAME transcriber (Basic Pitch) and the SAME decoder? Baseline arm = Basic Pitch on
the 4-stem 'other' (p4_ab.py). Lead selection for the union arm = p4_ab.lead_select (declared a priori).

  python p5_sep.py --work <dir with depois-1-32/ and cache/> --sep <dir with <case>/<stem>.wav>
"""
from __future__ import annotations

import argparse
import json
import pickle
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
import aicore_midi_service as s  # noqa: E402
import note_metrics as nm  # noqa: E402
import p4_ab  # noqa: E402
import score_templates as st  # noqa: E402

STEMS6 = ("other", "piano", "guitar")


def find_stem(folder, name):
    hits = sorted(Path(folder).rglob(f"*{name}*.wav"))
    return hits[0] if hits else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--sep", required=True)
    a = ap.parse_args()
    work, sep = Path(a.work), Path(a.sep)
    cache = work / "cache"
    rows = []
    for case, (name, start, length) in st.CASES.items():
        doc = json.loads((st.TEMPLATES / f"{name}.core-template.json").read_text(encoding="utf-8"))["document"]
        bpm = doc["transport"]["bpm"]
        tps = 960 * bpm / 60
        phase = json.loads((work / "depois-1-32" / case / "report.json").read_text())["metrics"]["gridPhaseSeconds"]
        arms = {}
        base_other = p4_ab.STEMS / f"{name}-{int(start)}" / "mix - other.wav"
        bx = s.to_rate(s.read_wav_np(str(base_other))[1].mean(1), 44100)
        arms["4 stems: other (baseline)"] = (s.clean_lead(pickle.load(open(cache / f"{case}-other-notes.pkl", "rb")), bx, bpm), True)
        union = []
        for stem in STEMS6:
            f = find_stem(sep / case, stem)
            if f is None:
                continue
            cp = cache / f"{case}-6s-{stem}-notes.pkl"
            if not cp.is_file():
                pickle.dump(s.basic_pitch_notes(f, 150, 5000), open(cp, "wb"))
            raw = pickle.load(open(cp, "rb"))
            x = s.to_rate(s.read_wav_np(str(f))[1].mean(1), 44100)
            lead = s.clean_lead(raw, x, bpm)
            arms[f"6 stems: {stem}"] = (lead, True)
            union += [n[:4] for n in lead]
        arms["6 stems: other+piano+guitar, lead-select"] = (p4_ab.lead_select(sorted(union), bpm, phase), False)
        arms["6 stems: other+piano+guitar, all"] = (sorted(union), True)
        for arm, (notes, cap2) in arms.items():
            for grid in ("1/32", "1/16"):
                made = p4_ab.placed(notes, bpm, phase, grid, cap2=cap2)
                tol = 60 / bpm / (16 if grid == "1/32" else 8)
                for cal in (True, False):
                    m = nm.protocol(p4_ab.truth(doc, "lead", case, start, length, cal), made, tol, tps)
                    rows.append({"case": case, "arm": arm, "grid": grid,
                                 "reference": "calibrated" if cal else "written", **m})
    (work / "p5-sep.json").write_text(json.dumps({"kind": "p5-sep", "rows": rows}, indent=1), encoding="utf-8")
    print(f"{'case':9} {'arm':42} {'D':>4} {'T':>4} {'P':>6} {'R':>6} {'F1on':>6} {'F1full':>6} {'J':>5}")
    for r in rows:
        if r["grid"] == "1/32" and r["reference"] == "calibrated":
            print(f"{r['case']:9} {r['arm']:42} {r['D']:4} {r['T']:4} {r['P']:6.3f} {r['R']:6.3f} "
                  f"{r['F1onset']:6.3f} {r['F1full']:6.3f} {r['J'] or 0:5.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
