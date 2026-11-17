"""Sonda CREPE (adenda do Plano B): f0 CREPE dos stems (lead=other, bass) -> 1 voz na grelha 1/32 -> altura exata.
   Usa os stems de D:\AI Dev\midi-maker\avaliacao (so leitura) e a fase de <clone>\_medidas\depois-1-32. Env: MODEL tiny|full, PART lead|bass, VOICED."""
import sys, json, os, subprocess, numpy as np
from pathlib import Path
R = Path(r"D:\AICore\trabalho\tarefa-03-midi-maker")
sys.path.insert(0, str(R / "services/midi")); sys.path.insert(0, str(R / "services/midi/eval"))
import score_templates as st, note_metrics as nm
CREPE_PY = r"D:\AICore\models\crepe-env\Scripts\python.exe"
C = R / "_medidas" / "cache"; C.mkdir(exist_ok=True)
MODEL = os.environ.get("MODEL", "full")
PART = os.environ.get("PART", "lead")
STEM = {"lead": "other", "bass": "bass"}[PART]
import time
WALL = {}
VOICED = float(os.environ.get("VOICED", "0.5"))
MIN_CELLS = int(os.environ.get("MIN_CELLS", "2"))
RMS_GATE = float(os.environ.get("RMS_GATE", "0.1"))  # share of the window's p95 rms


def crepe_lead(z, bpm, phase, seconds):
    times, f0, per, rms = z["times"], z["f0"], z["periodicity"], z["rms"]
    cell = 60 / bpm / 8
    first = phase - np.ceil(phase / cell) * cell
    n = int(np.ceil((seconds - first) / cell))
    rms = np.nan_to_num(rms)
    gate = RMS_GATE * np.percentile(rms, 95)
    idx = np.floor((times - first) / cell).astype(int)
    pitch = np.full(n, -1)
    conf = np.zeros(n)
    for c in range(n):
        m = (idx == c) & (per >= VOICED) & (rms >= gate) & (f0 > 0)
        if m.sum() >= 2:
            midi = 69 + 12 * np.log2(f0[m] / 440)
            pitch[c] = int(round(float(np.median(midi))))
            conf[c] = float(per[m].mean())
    out, c = [], 0
    while c < n:
        if pitch[c] < 0:
            c += 1
            continue
        s = c
        while c + 1 < n and pitch[c + 1] == pitch[s]:
            c += 1
        if c + 1 - s >= MIN_CELLS:
            out.append((first + s * cell, first + (c + 1) * cell, int(pitch[s]), float(conf[s:c + 1].mean())))
        c += 1
    return out


tot = {0: [], 1: []}
for key, (name, start, length) in st.CASES.items():
    stem = rf"D:\AI Dev\midi-maker\avaliacao\stems\{name}-{int(start)}\mix - {STEM}.wav"
    cp = C / f"{key}-{STEM}-crepe-{MODEL}.npz"
    if not cp.is_file():
        t0 = time.monotonic()
        p = subprocess.run([CREPE_PY, str(R / "services/midi/crepe_f0.py"), stem, str(cp), "--model", MODEL],
                           capture_output=True, text=True, creationflags=0x4000)
        if p.returncode:
            raise SystemExit(p.stderr[-800:])
        WALL[key] = round(time.monotonic() - t0, 1)
    z = np.load(cp)
    doc = json.loads((st.TEMPLATES / f"{name}.core-template.json").read_text(encoding="utf-8"))["document"]
    bpm = doc["transport"]["bpm"]
    phase = json.load(open(R / f"_medidas/depois-1-32/{key}/report.json"))["metrics"]["gridPhaseSeconds"]
    made = [(a, p, b - a) for a, b, p, _ in crepe_lead(z, bpm, phase, length)]
    for snd in (0, 1):
        true = sorted((a, p + (st.SOUNDING.get(tr["name"].lower(), 0) if snd else 0), d) for tr in doc["tracks"]
                      if st.PARTS[PART](tr["name"].lower())
                      for a, p, d in st.truth(doc, lambda q, k=tr["name"].lower(): q == k, start, length))
        m = nm.scores(true, made, 60 / bpm / 16)
        tot[snd].append(m["pitchStrict"]["f1"])
        print(f"{key:9} {'soa' if snd else 'esc'} made {m['made']:4}/{m['true']:4} junk {m['junkRatio']:5.2f} | on "
              f"{m['onsetOnly']['f1']:.3f} strict {m['pitchStrict']['f1']:.3f} (R {m['pitchStrict']['recall']:.3f} "
              f"P {m['pitchStrict']['precision']:.3f}) +off {m['pitchStrictOffset']['f1']:.3f} {m['pitchIntervals']}")
print("crepe wall s", WALL, "model", MODEL, "part", PART)
print("mean strict F1 written", round(float(np.mean(tot[0])), 3), "sounding", round(float(np.mean(tot[1])), 3))
