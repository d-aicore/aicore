"""Controlled evaluation set (owner's OK 2026-11-16 07:54; measure only, no training).

For each template window: the WRITTEN notes of its kick, bass and lead tracks (no bend, no FX automation) ->
bass and lead rendered by Vita (init preset; its pitch = the written note, checked: controlled bass interval 0)
+ a synthetic kick -> mix.wav. Truth = the written notes, which here ARE the sounding notes.

  python controlled_set.py build --out <dir>       (in the midi-maker env; calls the Vita service)
  python controlled_set.py score --out <dir> [--mt3 <dir>]
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
import aicore_midi_service as s  # noqa: E402
import note_metrics as nm  # noqa: E402
import p4_ab  # noqa: E402
import score_templates as st  # noqa: E402

VITA_PY = r"D:\AICore\plugins\vita-venv\Scripts\python.exe"
SR = 44100
LEVELS = {"bass": 0.45, "lead": 0.35, "kick": 0.7}


def write_mid(path, notes, bpm):
    import mido
    tps = 960 * bpm / 60
    mf = mido.MidiFile(type=1, ticks_per_beat=960)
    tr = mido.MidiTrack()
    mf.tracks.append(tr)
    tr.append(mido.MetaMessage("set_tempo", tempo=mido.bpm2tempo(bpm), time=0))
    ev = []
    for t, p, d in notes:
        a = int(round(t * tps))
        ev += [(a, 1, p), (a + max(1, int(round(d * tps))), 0, p)]
    last = 0
    for tk, on, p in sorted(ev):
        tr.append(mido.Message("note_on" if on else "note_off", note=p, velocity=100 if on else 0, time=tk - last))
        last = tk
    mf.save(path)


def kick_wave():
    import numpy as np
    t = np.arange(int(0.12 * SR)) / SR
    f = 45 + 105 * np.exp(-t / 0.015)
    return (np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t / 0.05)).astype(np.float32)


def build(out):
    import numpy as np
    out.mkdir(parents=True, exist_ok=True)
    for case, (name, start, length) in st.CASES.items():
        d = out / case
        d.mkdir(exist_ok=True)
        doc = json.loads((st.TEMPLATES / f"{name}.core-template.json").read_text(encoding="utf-8"))["document"]
        bpm = doc["transport"]["bpm"]
        truth = {part: st.truth(doc, st.PARTS[part], start, length) for part in ("kick", "bass", "lead")}
        (d / "truth.json").write_text(json.dumps({"bpm": bpm, "length": length, **truth}), encoding="utf-8")
        n = int(length * SR)
        mix = np.zeros((n, 2), np.float32)
        for part in ("bass", "lead"):
            write_mid(d / f"{part}.mid", truth[part], bpm)
            vd = d / f"vita-{part}"
            vd.mkdir(exist_ok=True)
            req = {"schemaVersion": "1.0", "requestId": f"ctl-{case}-{part}", "profile": "render", "operation": "render",
                   "params": {"engine": "vita", "sampleRate": SR, "blockSize": 512, "tailSeconds": 0.2},
                   "inputs": [{"role": "midi", "path": str(d / f"{part}.mid")}], "outputDir": str(vd)}
            p = subprocess.run([VITA_PY, str(REPO / "services" / "vita" / "aicore_vita_service.py"), "job"],
                               input=json.dumps(req) + "\n", capture_output=True, text=True, creationflags=0x4000)
            last = json.loads(p.stdout.splitlines()[-1])
            if last.get("event") != "done":
                raise SystemExit(f"vita failed {case}/{part}: {last}")
            _, w = s.read_wav_np(str(vd / "render.wav"))
            w = w[:n]
            mix[:len(w)] += w / max(1e-9, float(np.abs(w).max())) * LEVELS[part]
        k = kick_wave()
        for t, _, _ in truth["kick"]:
            a = int(t * SR)
            m = min(len(k), n - a)
            if m > 0:
                mix[a:a + m] += LEVELS["kick"] * k[:m, None]
        mix /= max(1.0, float(np.abs(mix).max()) / 0.9)
        (d / "mix.wav").write_bytes(s.wav_float32_bytes(mix, SR))
        print(case, {p: len(v) for p, v in truth.items()}, "bpm", bpm)


def run_service(d, bpm, length):
    od = d / "svc"
    od.mkdir(exist_ok=True)
    req = {"schemaVersion": "1.0", "requestId": f"ctl-{d.name}", "profile": "midi", "operation": "transcribe",
           "params": {"ppq": 960, "quantize": "1/32", "parts": ["drums", "bass", "other"], "kickNote": 36,
                      "bassMono": True, "bpm": float(bpm), "bpmSource": "provided", "leadFilter": False,
                      "splitOnOnsets": True, "includeStems": True, "seed": 42},
           "inputs": [{"role": "mix", "path": str(d / "mix.wav")}], "outputDir": str(od)}
    p = subprocess.run([sys.executable, str(HERE.parent / "aicore_midi_service.py"), "job"], input=json.dumps(req) + "\n",
                       capture_output=True, text=True, creationflags=0x4000)
    last = json.loads(p.stdout.splitlines()[-1])
    if last.get("event") != "done":
        raise SystemExit(f"service failed {d.name}: {last}")
    return last["result"]["metrics"]


def score(out, mt3_dir):
    import pickle
    rows = []
    for case in st.CASES:
        d = out / case
        tr = json.loads((d / "truth.json").read_text())
        bpm, length = tr["bpm"], tr["length"]
        tps = 960 * bpm / 60
        rep = d / "svc" / "report.json"
        metrics = json.loads(rep.read_text())["metrics"] if rep.is_file() else run_service(d, bpm, length)
        phase = metrics["gridPhaseSeconds"]
        arms = {}
        for part in ("kick", "bass", "lead"):
            notes, _ = st.made(d / "svc" / "transcription.mid", phase, lambda n, part=part: n == part)
            arms.setdefault("service (Basic Pitch)", {})[part] = notes
        other = d / "svc" / "stem-other.wav"
        cp = d / "other-bp.pkl"
        if not cp.is_file():
            pickle.dump(s.basic_pitch_notes(other, 150, 5000), open(cp, "wb"))
        raw = [n[:4] for n in pickle.load(open(cp, "rb"))]
        arms["Basic Pitch, filter off"] = {"lead": p4_ab.placed(raw, bpm, phase, "1/32")}
        if mt3_dir and (Path(mt3_dir) / f"{case}-mix.json").is_file():
            notes = json.loads((Path(mt3_dir) / f"{case}-mix.json").read_text())["notes"]
            lead = [(a, b, p, v / 127) for a, b, p, prog, drum, v in notes if not drum and prog not in p4_ab.BASS_PROG]
            arms["MT3 mix, lead-select"] = {"lead": p4_ab.placed(p4_ab.lead_select(lead, bpm, phase), bpm, phase, "1/32")}
            allp = [(a, b, p, v / 127) for a, b, p, prog, drum, v in notes if not drum]
            low = [n for n in allp if n[2] < 52]
            arms["MT3 mix, lead-select"]["bass"] = p4_ab.placed(low, bpm, phase, "1/32", mono=True)
        for arm, parts in arms.items():
            for part, made in parts.items():
                true = [tuple(x) for x in tr[part]]
                for grid, tol in (("1/32", 60 / bpm / 16), ("1/16", 60 / bpm / 8)):
                    m = nm.protocol(true, made, tol, tps)
                    rows.append({"case": case, "arm": arm, "part": part, "grid": grid, **m,
                                 "serviceWallSeconds": metrics.get("wallSeconds")})
    (out / "score.json").write_text(json.dumps({"kind": "controlled-set", "rows": rows}, indent=1), encoding="utf-8")
    print(f"{'case':9} {'arm':34} {'part':4} {'D':>4} {'T':>4} {'P':>6} {'R':>6} {'F1on':>6} {'F1full':>6} {'J':>5} {'end':>5}")
    for r in rows:
        if r["grid"] == "1/32":
            print(f"{r['case']:9} {r['arm']:34} {r['part']:4} {r['D']:4} {r['T']:4} {r['P']:6.3f} {r['R']:6.3f} "
                  f"{r['F1onset']:6.3f} {r['F1full']:6.3f} {r['J'] or 0:5.2f} {r['endErrTicksMed'] or 0:5.0f}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["build", "score"])
    ap.add_argument("--out", required=True)
    ap.add_argument("--mt3", default=None)
    a = ap.parse_args()
    if a.cmd == "build":
        return build(Path(a.out)) or 0
    return score(Path(a.out), a.mt3)


if __name__ == "__main__":
    sys.exit(main())
