"""S2 quality gate: note scores per part on Core template renders, service vs the midi_maker baseline.

Truth = the notes of the Core template (`<Templates>\\<name>.core-template.json`, READ-ONLY) inside the
window; audio = the template's render (`D:\\AI Dev\\flamingo-teste\\templates\\<name>.wav`, READ-ONLY).
Same windows and the same matcher as the baseline scorer `D:\\AI Dev\\midi-maker\\avaliar.py`: a made note
matches a true note when its onset is within half a 1/16 step (greedy, nearest first, one-to-one).
F1 = 2PR/(P+R). Bass pitch is reported as pitch-class too (HANDOFF §3: Core sampler basses).
That headline F1 is ONSET-ONLY (pitch is ignored for the hit; an octave error still counts as a hit) and
the tolerance is half a 1/16 whatever --quantize says. It stays as it is (changing the golden needs the
owner's OK). Next to it, `p0` (note_metrics.py) adds pitch-strict and pitch-strict + offset F1, P/R, the
junk ratio (made/true) and the median signed onset error, at half a 1/16 AND half a 1/32.

The baseline numbers are NOT copied: the baseline .mid files the owner's run left in
`D:\\AI Dev\\midi-maker\\avaliacao` (PPQ 480) are re-scored here with this same code, so both columns come
from one matcher.

  python score_templates.py --out <dir> [--quantize 1/16] [--cases all|t1,...] [--rescore]
  --rescore: score the transcription.mid already in <out>/<case> (no service run)

Writes <out>/score-<quantize>.json and prints a table. Exit 0 = ran (this is a measurement, the
pass/fail rule lives in the README and is applied by the coordinator, not hidden in a threshold here).
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

import mido

sys.path.insert(0, str(Path(__file__).resolve().parent))
import note_metrics  # noqa: E402

HERE = Path(__file__).resolve().parent
SERVICE = HERE.parent / "aicore_midi_service.py"
TEMPLATES = Path(os.environ.get("AICORE_CORE_TEMPLATES", r"C:\Users\vk_do\AppData\Roaming\Core\Templates"))
RENDERS = Path(os.environ.get("AICORE_TEMPLATE_RENDERS", r"D:\AI Dev\flamingo-teste\templates"))
BASELINE = Path(os.environ.get("AICORE_MIDI_BASELINE", r"D:\AI Dev\midi-maker\avaliacao"))
# The four windows the baseline was measured on (avaliacao/*-pontuacao.json): name, start, length.
CASES = {"remember": ("hitech-ref-remember", 200.0, 30.0),
         "wakeup": ("hitech-ref-wakeup", 18.0, 25.0),
         "t1": ("hitech-t1-v1", 61.7, 25.0),
         "t2": ("hitech-t2-v1", 23.9, 25.0)}
LEAD_WORDS = ("lead", "arp", "pluck", "answer", "melod", "stab")
FX_WORDS = ("zap", "sweep", "riser", "fx", "impact", "reverse")
PARTS = {"kick": lambda n: n.startswith("kick"),
         "bass": lambda n: n.startswith("bass"),
         "lead": lambda n: any(w in n for w in LEAD_WORDS) and not any(w in n for w in FX_WORDS)}
BELOW_NORMAL = 0x00004000
# Tracks whose render SOUNDS at another pitch than written (measured 2026-11-16 on the 'other' stem, harmonic
# comb vs written pitch: "FM answer" (Core builtin 'bass' engine) 107/108 notes one octave down). `p0Sounding`
# scores against written + offset; the written-pitch numbers stay as they are.
SOUNDING = {"fm answer": -12}


def truth(doc, pred, start, length):
    bpm, ppq = doc["transport"]["bpm"], doc["transport"]["ppq"]
    spt = 60 / bpm / ppq
    out = []
    for tr in doc["tracks"]:
        if not pred(tr["name"].lower()) or tr.get("mix", {}).get("mute"):
            continue
        pats = {p["id"]: p for p in tr.get("patterns", [])}
        for c in tr.get("clips", []):
            p = pats.get(c["patternId"])
            if not p or not p.get("lengthTicks"):
                continue
            k = 0
            while k * p["lengthTicks"] < c["lengthTicks"]:
                for n in p["notes"]:
                    off = k * p["lengthTicks"] + n["tick"]
                    if off >= c["lengthTicks"]:
                        continue
                    t = (c["startTick"] + off) * spt
                    if start <= t < start + length:
                        out.append((t - start, n["pitch"], n.get("length", 0) * spt))
                k += 1
    return sorted(out)


def made(path, offset, track_pred=None):
    """Notes (onset s, pitch, duration s); +offset = grid phase (MIDI tick 0 is the grid phase in both writers)."""
    mid = mido.MidiFile(str(path))
    events = []
    for tr in mid.tracks:
        name = next((m.name for m in tr if m.type == "track_name"), "").lower()
        now = 0
        for m in tr:
            now += m.time
            if m.type == "set_tempo":
                events.append((now, 0, m.tempo, None))
            elif m.type in ("note_on", "note_off") and (track_pred is None or track_pred(name)):
                on = m.type == "note_on" and m.velocity > 0
                events.append((now, 2 if on else 1, None, m.note))  # offs before ons at one tick
    events.sort()
    tempo, last_tick, secs, out, held = 500000, 0, 0.0, [], {}
    for tick, kind, tmp, note in events:
        secs += mido.tick2second(tick - last_tick, mid.ticks_per_beat, tempo)
        last_tick = tick
        if kind == 0:
            tempo = tmp
        elif kind == 2:
            held.setdefault(note, []).append(secs)
        elif held.get(note):
            s0 = held[note].pop(0)
            out.append((offset + s0, note, secs - s0))
    for note, starts in held.items():  # never closed: zero length
        out += [(offset + s0, note, 0.0) for s0 in starts]
    return sorted(out), mid.ticks_per_beat


def score(true, pred, step):
    used, hit, hp, hc = set(), 0, 0, 0
    for t, p, *_ in true:
        best = None
        for i, (u, q, *_) in enumerate(pred):
            if i in used or abs(u - t) > step / 2:
                continue
            if best is None or abs(u - t) < abs(pred[best][0] - t):
                best = i
        if best is not None:
            used.add(best)
            hit += 1
            hp += pred[best][1] == p
            hc += pred[best][1] % 12 == p % 12
    r = hit / max(len(true), 1)
    pr = hit / max(len(pred), 1)
    return {"true": len(true), "made": len(pred), "recall": round(r, 3), "precision": round(pr, 3),
            "f1": round(2 * r * pr / (r + pr), 3) if r + pr else 0.0,
            "pitchExact": round(hp / max(hit, 1), 3), "pitchClass": round(hc / max(hit, 1), 3)}


def resolve_out(arg):
    """--out may be relative; the request's outputDir must be absolute (audit 2026-11-16)."""
    return Path(arg).resolve()


def run_service(name, start, length, bpm, quantize, out, seed=42, lead_filter=True):
    out.mkdir(parents=True, exist_ok=True)
    req = {"schemaVersion": "1.0", "requestId": f"s2-eval-{name}-{quantize.replace('/', '-')}",
           "profile": "midi", "operation": "transcribe",
           "params": {"ppq": 960, "quantize": quantize, "parts": ["drums", "bass", "other"], "kickNote": 36,
                      "bassMono": True, "bpm": float(bpm), "bpmSource": "provided", "leadFilter": lead_filter,
                      "splitOnOnsets": True, "includeStems": False, "startSeconds": start,
                      "lengthSeconds": length, "seed": seed},
           "inputs": [{"role": "mix", "path": str(RENDERS / f"{name}.wav")}], "outputDir": str(out)}
    t0 = time.monotonic()
    p = subprocess.run([sys.executable, str(SERVICE), "job"], input=json.dumps(req) + "\n", capture_output=True,
                       text=True, encoding="utf-8", errors="replace",
                       creationflags=BELOW_NORMAL if os.name == "nt" else 0)
    lines = [json.loads(ln) for ln in p.stdout.splitlines() if ln.strip()]
    (out / "service.stdout.jsonl").write_text(p.stdout, encoding="utf-8")
    if p.returncode != 0 or not lines or lines[-1]["event"] != "done":
        raise RuntimeError(f"service failed on {name}: exit {p.returncode} {lines[-1] if lines else p.stderr[-400:]}")
    return lines[-1]["result"], round(time.monotonic() - t0, 2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--quantize", default="1/16")
    ap.add_argument("--cases", default="all")
    ap.add_argument("--seed", type=int, default=42, help="Demucs seed (params.seed)")
    ap.add_argument("--rescore", action="store_true", help="score the existing <out>/<case> run, no service")
    ap.add_argument("--lead-filter", default="on", choices=["on", "off"],
                    help="params.leadFilter of the request (on = the golden request, unchanged)")
    args = ap.parse_args()
    out = resolve_out(args.out)
    keys = list(CASES) if args.cases == "all" else args.cases.split(",")
    rows = []
    for key in keys:
        name, start, length = CASES[key]
        doc = json.loads((TEMPLATES / f"{name}.core-template.json").read_text(encoding="utf-8"))["document"]
        bpm = doc["transport"]["bpm"]
        step = 60 / bpm / 4
        if args.rescore:
            rep = json.loads((out / key / "report.json").read_text(encoding="utf-8"))
            mp = out / key / "transcription.mid"
            result = {"metrics": rep["metrics"], "outputs": [{"role": "midi", "path": str(mp),
                      "sha256": __import__("hashlib").sha256(mp.read_bytes()).hexdigest()}]}
            wall = rep["metrics"].get("wallSeconds")
        else:
            result, wall = run_service(name, start, length, bpm, args.quantize, out / key, args.seed,
                                       args.lead_filter == "on")
        phase = result["metrics"]["gridPhaseSeconds"]
        mid = next(o for o in result["outputs"] if o["role"] == "midi")
        tag = f"{name}-{int(start)}"
        brep = BASELINE / f"{tag}-relatorio.json"
        bphase = json.loads(brep.read_text(encoding="utf-8"))["gridPhaseS"] if brep.is_file() else None
        row = {"case": key, "template": name, "start": start, "length": length, "bpm": bpm,
               "quantize": args.quantize, "seed": args.seed, "wallSeconds": wall, "serviceMidi": mid["path"],
               "serviceMidiSha256": mid["sha256"], "parts": {}}
        for part, pred in PARTS.items():
            t = truth(doc, pred, start, length)
            if not t:
                continue
            s_notes, s_ppq = made(mid["path"], phase, lambda n, part=part: n == part)
            snd = sorted((a, p_ + SOUNDING.get(tr["name"].lower(), 0), d) for tr in doc["tracks"]
                         if pred(tr["name"].lower())
                         for a, p_, d in truth(doc, lambda n, k=tr["name"].lower(): n == k, start, length))
            entry = {"service": score(t, s_notes, step), "servicePpq": s_ppq,
                     "p0": {g: note_metrics.scores(t, s_notes, step / 2 * (1 if g == "1/16" else 0.5))
                            for g in ("1/16", "1/32")}}
            if snd != t:
                entry["p0Sounding"] = {g: note_metrics.scores(snd, s_notes, step / 2 * (1 if g == "1/16" else 0.5))
                                       for g in ("1/16", "1/32")}
            bmid = BASELINE / f"{tag}-{part}.mid"
            if bphase is not None and bmid.is_file():
                b_notes, b_ppq = made(bmid, bphase)
                entry["baseline"] = score(t, b_notes, step)
                entry["baselineP0"] = {g: note_metrics.scores(t, b_notes, step / 2 * (1 if g == "1/16" else 0.5))
                                       for g in ("1/16", "1/32")}
                entry["baselinePpq"] = b_ppq
            row["parts"][part] = entry
        rows.append(row)
    report = {"kind": "s2-template-score", "matcher": "onset within half a 1/16 step, greedy nearest, 1:1",
              "quantize": args.quantize, "rows": rows}
    rp = out / f"score-{args.quantize.replace('/', '-')}-seed{args.seed}.json"
    rp.write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(f"{'case':9} {'part':5} {'true':>5} | {'svc R':>6} {'P':>6} {'F1':>6} {'pc':>6} | {'base R':>6} {'P':>6} {'F1':>6} {'pc':>6}")
    for row in rows:
        for part, e in row["parts"].items():
            s, b = e["service"], e.get("baseline")
            bt = f"{b['recall']:6.3f} {b['precision']:6.3f} {b['f1']:6.3f} {b['pitchClass']:6.3f}" if b else "     -"
            print(f"{row['case']:9} {part:5} {s['true']:5} | {s['recall']:6.3f} {s['precision']:6.3f} "
                  f"{s['f1']:6.3f} {s['pitchClass']:6.3f} | {bt}")
        print(f"{row['case']:9} wall {row['wallSeconds']} s, service .mid ppq 960 sha {row['serviceMidiSha256'][:16]}")
    print("\nP0 (service): F1 onset-only / pitch-strict / strict+offset, junk = made/true, dt = median onset error")
    print(f"{'case':9} {'part':5} {'grid':5} {'on':>6} {'strict':>6} {'+off':>6} {'R str':>6} {'P str':>6} "
          f"{'junk':>5} {'dt ms':>7}  intervals")
    for row in rows:
        for part, e in row["parts"].items():
            for g, m in e["p0"].items():
                dt = m["onsetErrorMedianMs"]
                if "p0Sounding" in e:
                    m2 = e["p0Sounding"][g]
                    g = g + "*"
                    print(f"{row['case']:9} {part:5} {g:5} {m['onsetOnly']['f1']:6.3f} {m['pitchStrict']['f1']:6.3f} "
                          f"{m['pitchStrictOffset']['f1']:6.3f} {m['pitchStrict']['recall']:6.3f} "
                          f"{m['pitchStrict']['precision']:6.3f}  (written)")
                    m = m2
                print(f"{row['case']:9} {part:5} {g:5} {m['onsetOnly']['f1']:6.3f} {m['pitchStrict']['f1']:6.3f} "
                      f"{m['pitchStrictOffset']['f1']:6.3f} {m['pitchStrict']['recall']:6.3f} "
                      f"{m['pitchStrict']['precision']:6.3f} {m['junkRatio']:5.2f} "
                      f"{dt if dt is not None else '-':>7}  {m['pitchIntervals']}")
    print(f"report: {rp}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
