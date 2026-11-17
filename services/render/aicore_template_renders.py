"""AI > TEMPLATE RENDERS: Diogo's sound templates play Guitar Pro MIDI, aicore-render renders, the notes are exact.

The old AICORE did this with Core's engine (template_renders.py: core-project + core-engine render). AICore is its own
program now (Diogo 2026-11-16): the sound is rendered by AICore's own VST3 host, ``aicore-render`` (Vital, Airwindows,
Dragonfly...), and nothing of the Core is read or run.

* A sound template is a JSON file ``{"format": "aicore-render-template", "name": ..., "params": {...}}`` whose
  ``params`` are aicore-render's render params (``role`` / ``plugin`` + ``pluginStateBase64``, ``effects`` or
  ``effectRoles``, ``engine``...). It is never changed.
* Each render: one template, a few bars of a Guitar Pro MIDI (EDIT > MIDI), moved ±5 semitones and ×0.85-1.15 in
  tempo, written as a PPQ 960 .mid, rendered by ``aicore-render job``.
* Each render is cut into 4 s pieces with the exact notes of each piece: ``audio.npy`` (N × 88200 float32, mono,
  22050 Hz, peak 0.7), ``notes.npy`` (N arrays of (start s, end s, MIDI pitch)) and ``catalogo.jsonl`` -- the same
  files as the old page made, so the transcriber training reads them unchanged.

    python aicore_template_renders.py request.json
request: {"templates": folder, "mids": [...], "renderer": aicore-render.exe, "destination": folder,
          "perTemplate": 40, "bars": 4, "seed": 1, "out": job folder}
stdout: JSON lines {"event": "progress", "stage", "fraction"} ... {"event": "done", "result"} | {"event": "error", "message"}
Stop: a file ``cancel.request`` in the job folder.
"""
from __future__ import annotations

import glob
import json
import os
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

PPQ = 960
BAR = 4 * PPQ
CONTRACTS = Path(__file__).resolve().parent.parent.parent / "contracts"


def request_errors(body):
    """The client checks each request against the published schema before sending it (audit P1): a template
    whose params do not fit the contract fails here, with the schema's words, not inside aicore-render."""
    sys.path.insert(0, str(CONTRACTS / "sim"))
    try:
        from jsonschema_mini import Registry, validation_errors
    finally:
        sys.path.pop(0)
    return validation_errors(Registry(CONTRACTS / "schemas" / "v1"), "service-task-request", body)
SR = 22050
SEG = 4.0


def emit(event, **fields):
    print(json.dumps({"event": event, **fields}, ensure_ascii=False), flush=True)


def load_templates(folder):
    out = []
    for p in sorted(glob.glob(os.path.join(str(folder), "**", "*.json"), recursive=True)):
        try:
            tpl = json.loads(Path(p).read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if isinstance(tpl, dict) and tpl.get("format") == "aicore-render-template" and isinstance(tpl.get("params"), dict):
            out.append((p, tpl))
    return out


def _vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(out))


def write_midi(path, notes, bpm):
    """One track, PPQ 960: the tempo, then the notes (tick, length, pitch, velocity 0..1)."""
    events = [(0, 0, b"\xff\x51\x03" + int(round(60_000_000 / bpm)).to_bytes(3, "big"))]
    for n in notes:
        vel = max(1, min(127, int(round(n["velocity"] * 127)) if n["velocity"] <= 1 else int(n["velocity"])))
        events.append((n["tick"], 1, bytes([0x90, n["pitch"], vel])))
        events.append((n["tick"] + n["length"], 0, bytes([0x80, n["pitch"], 0])))
    events.sort(key=lambda e: (e[0], e[1]))
    data, last = b"", 0
    for tick, _order, payload in events:
        data += _vlq(tick - last) + payload
        last = tick
    data += _vlq(0) + b"\xff\x2f\x00"
    head = b"MThd" + struct.pack(">IHHH", 6, 0, 1, PPQ)
    Path(path).write_bytes(head + b"MTrk" + struct.pack(">I", len(data)) + data)


def make_job(i, templates, mids, rnd, bars):
    path, tpl = rnd.choice(templates)
    mid = rnd.choice(mids)
    win, b0 = [], 0
    for _ in range(20):  # a window with notes in it
        b0 = rnd.randrange(0, max(1, mid["bars"] - bars + 1))
        lo, hi = b0 * BAR, (b0 + bars) * BAR
        win = [n for n in mid["notes"] if lo <= n["tick"] < hi]
        if len(win) >= 4:
            break
    lo, hi = b0 * BAR, (b0 + bars) * BAR
    shift = rnd.randint(-5, 5)
    notes = []
    for n in win:
        p = n["pitch"] + shift
        if 21 <= p <= 108:
            notes.append({"tick": n["tick"] - lo, "length": max(1, min(n["length"], hi - n["tick"])), "pitch": p,
                          "velocity": n.get("velocity", 0.8)})
    bpm = round(mid["bpm"] * rnd.uniform(0.85, 1.15), 2)
    sec = 60.0 / bpm / PPQ
    labels = [(n["tick"] * sec, (n["tick"] + n["length"]) * sec, n["pitch"]) for n in notes]
    params = dict(tpl["params"])
    params.setdefault("sampleRate", 44100)
    params.setdefault("blockSize", 512)
    params.setdefault("tailSeconds", 1)
    params["lengthSeconds"] = bars * 4 * 60.0 / bpm + float(params.get("tailSeconds", 1))
    info = {"id": f"treino-{i:05d}", "template": os.path.basename(path), "name": tpl.get("name", ""),
            "instrument": params.get("role") or os.path.basename(str(params.get("plugin", params.get("engine", "")))),
            "effects": params.get("effectRoles") or [os.path.basename(e.get("plugin", "")) for e in params.get("effects", [])],
            "mid": os.path.basename(mid["path"]), "bars": [b0 + 1, b0 + bars], "transpose": shift, "bpm": bpm,
            "notes": len(notes)}
    return {"params": params, "notes": notes, "bpm": bpm, "labels": labels, "info": info}


def render(job, renderer, work):
    import numpy as np
    import soundfile as sf
    from scipy.signal import resample_poly
    info = job["info"]
    base = Path(work) / info["id"]
    base.mkdir(parents=True, exist_ok=True)
    mid = base / "notes.mid"
    write_midi(mid, job["notes"], job["bpm"])
    body = {"schemaVersion": "1.0", "requestId": info["id"], "profile": "render", "operation": "render",
            "params": job["params"], "inputs": [{"role": "midi", "path": str(mid)}], "outputDir": str(base / "out")}
    errs = request_errors(body)
    if errs:
        return None, f"{info['template']}: request does not fit the contract: {'; '.join(errs[:3])}"
    (base / "out").mkdir(exist_ok=True)
    p = subprocess.run([renderer, "job"], input=json.dumps(body) + "\n", capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=600,
                       creationflags=0x08000000 if sys.platform == "win32" else 0)
    wav = base / "out" / "render.wav"
    if p.returncode != 0 or not wav.exists():
        last = [ln for ln in p.stdout.splitlines() if ln.strip()][-1:] or [p.stderr.strip()[:160]]
        return None, f"{info['template']}: {last[0][:200]}"
    x, sr = sf.read(str(wav), dtype="float32", always_2d=True)
    y = x.mean(1)
    peak = float(np.abs(y).max()) if y.size else 0.0
    if not np.isfinite(peak) or peak < 1e-4:
        return None, f"{info['template']}: silent or NaN"
    y = resample_poly(y / peak * 0.7, SR, sr).astype(np.float32)
    pieces, n = [], int(SEG * SR)
    for a in range(0, len(y) - n // 2, n):
        seg = np.zeros(n, np.float32)
        part = y[a:a + n]
        seg[:len(part)] = part
        t0 = a / SR
        lab = [(s - t0, min(e - t0, SEG), pp) for s, e, pp in job["labels"] if t0 <= s < t0 + SEG]
        pieces.append((seg, np.array(lab, np.float32).reshape(-1, 3)))
    return (pieces, info), None


def main(argv):
    import numpy as np
    request = json.loads(Path(argv[0]).read_text(encoding="utf-8"))
    out_job = Path(request.get("out") or Path(argv[0]).parent)
    stopped = (out_job / "cancel.request").exists
    from aicore_studio.template_renders import load_mids  # Guitar Pro MIDI in (AICore's own reader)
    found = load_templates(request["templates"])
    if not found:
        raise ValueError(f"No sound template (aicore-render-template .json) in {request['templates']}.")
    loaded = load_mids(request["mids"])
    if not loaded:
        raise ValueError("No MIDI file with at least 8 notes.")
    renderer = request["renderer"]
    if not Path(renderer).is_file():
        raise ValueError(f"aicore-render was not found: {renderer}")
    # aicore-render renders only the plugins of AICore's list: scan it first (its own list, never the Core's)
    emit("progress", stage="Looking for the plugins", fraction=0)
    subprocess.run([renderer, "scan"], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=600,
                   creationflags=0x08000000 if sys.platform == "win32" else 0)
    rnd = random.Random(int(request.get("seed", 1)))
    bars = int(request.get("bars", 4))
    jobs = [make_job(i, found, loaded, rnd, bars) for i in range(len(found) * int(request.get("perTemplate", 40)))]
    out = Path(request["destination"])
    out.mkdir(parents=True, exist_ok=True)
    X, Y, fails = [], [], []
    emit("progress", stage="Rendering", fraction=0)
    with tempfile.TemporaryDirectory(dir=str(out_job)) as work, (out / "catalogo.jsonl").open("w", encoding="utf-8") as cat:
        for k, job in enumerate(jobs, 1):  # one at a time: the plugins are heavy and the PC stays usable
            if stopped():
                break
            res, err = render(job, renderer, work)
            if err:
                fails.append(err)
            else:
                pieces, info = res
                info["pieces"] = [len(X), len(X) + len(pieces)]
                for seg, lab in pieces:
                    X.append(seg)
                    Y.append(lab)
                cat.write(json.dumps(info, ensure_ascii=False) + "\n")
            emit("progress", stage="Rendering", fraction=k / len(jobs))
    if not X:
        raise ValueError("Nothing was rendered: " + "; ".join(sorted(set(fails))[:5]))
    np.save(out / "audio.npy", np.stack(X))
    np.save(out / "notes.npy", np.array(Y + [None], dtype=object)[:-1], allow_pickle=True)
    return {"folder": str(out), "pieces": len(X), "renders": len(jobs) - len(fails), "failed": sorted(set(fails))[:10],
            "templates": len(found), "mids": len(loaded)}


if __name__ == "__main__":
    try:
        if os.name == "nt":
            import ctypes
            k = ctypes.WinDLL("kernel32", use_last_error=True)
            k.GetCurrentProcess.restype = ctypes.c_void_p
            k.SetPriorityClass.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
            k.SetPriorityClass(k.GetCurrentProcess(), 0x4000)  # below normal: no crackles while it renders
        emit("done", result=main(sys.argv[1:]))
        sys.exit(0)
    except Exception as e:  # noqa: BLE001
        emit("error", message=str(e)[:1000])
        sys.exit(1)
