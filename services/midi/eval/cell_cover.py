"""Diagnostic: is the lead melody there but with the rhythm collapsed (tremolo -> long notes)?
Cell-level (1/32) pitch-exact P/R: a cell 'has' pitch p when a note of pitch p covers it. Truth = calibrated lead."""
import sys, json, pickle
from pathlib import Path
R = Path(r"D:\AICore\trabalho\tarefa-03-midi-maker")
sys.path.insert(0, str(R / "services/midi")); sys.path.insert(0, str(R / "services/midi/eval"))
import aicore_midi_service as s, score_templates as st, p4_ab


def cells(notes, first, cell, n):
    out = set()
    for a, p, d in notes:
        c0, c1 = int((a - first) / cell + 0.5), int((a + d - first) / cell + 0.5)
        for c in range(max(c0, 0), min(max(c1, c0 + 1), n)):
            out.add((c, p))
    return out


for case, (name, start, length) in st.CASES.items():
    doc = json.loads((st.TEMPLATES / f"{name}.core-template.json").read_text(encoding="utf-8"))["document"]
    bpm = doc["transport"]["bpm"]
    phase = json.loads((R / "_medidas/depois-1-32" / case / "report.json").read_text())["metrics"]["gridPhaseSeconds"]
    cell = 60 / bpm / 8
    n = int(length / cell)
    true = p4_ab.truth(doc, "lead", case, start, length, True)
    raw = pickle.load(open(R / "_medidas/cache" / f"{case}-other-notes.pkl", "rb"))
    made = [(a, p, b - a) for a, b, p, *_ in raw]
    tc, mc = cells(true, phase, cell, n), cells(made, phase, cell, n)
    hit = len(tc & mc)
    pc = {(c, p % 12) for c, p in tc} & {(c, p % 12) for c, p in mc}
    print(f"{case:9} truth cells {len(tc):5} made cells {len(mc):5} | pitch-exact R {hit / max(len(tc), 1):.3f} "
          f"P {hit / max(len(mc), 1):.3f} | pitch-class R {len(pc) / max(len(tc), 1):.3f}")
