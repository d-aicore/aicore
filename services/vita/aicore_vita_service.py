"""AICore-C — Vita render service: a PPQ 960 .mid through Vita (the Vital engine as a Python module).

Contract aicore.service/1, profile "render", params.engine = "vita" (CONTRACT.md §5b). Same transport as
every service: one JSON request line on stdin, JSON lines on stdout (start -> progress* -> done | error),
exit 0 / 2. Vita is not a VST3, so it cannot run inside aicore-render; this service writes the dry synth
WAV, and aicore-render applies the effect chain to it (input role "audio").

  python aicore_vita_service.py hello
  python aicore_vita_service.py job < request.json

Runs in D:\\AICore\\plugins\\vita-venv (python 3.11, vita 0.1.0, numpy). Vita renders one note per call
(`render(note, velocity, note_dur, render_dur)`), so each note of the .mid is rendered on its own and summed
at its start sample: chords and overlaps work, legato/glide between notes does not [H: owner's listening].
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO / "contracts" / "sim"))
import media  # sha256, atomic_write, PPQ
from jsonschema_mini import Registry, validation_errors

CONTRACT = "aicore.service/1"
SERVICE = {"name": "aicore-vita", "version": "s5-vita-0.1.0"}
SCHEMA_DIR = REPO / "contracts" / "schemas" / "v1"
MAX_WAV = 128 * 1024 * 1024
_PROTO = None


def now():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def protect_stdout():
    global _PROTO
    sys.stdout.flush()
    fd = os.dup(1)
    os.dup2(2, 1)
    _PROTO = os.fdopen(fd, "w", encoding="ascii", newline="\n", buffering=1)
    sys.stdout = sys.stderr


def emit(doc):
    out = _PROTO or sys.stdout
    out.write(json.dumps(doc, ensure_ascii=True, allow_nan=False) + "\n")
    out.flush()


class ContractError(Exception):
    def __init__(self, code, message, details=None):
        super().__init__(message)
        self.code, self.message, self.details = code, str(message)[:500], details or {}

    def doc(self, rid):
        d = {"code": self.code, "message": self.message, "requestId": rid, "retryable": self.code == "WORKER_UNAVAILABLE"}
        if self.details:
            d["details"] = self.details
        return d


def _vlq(body, i):
    v = 0
    while True:
        if i >= len(body):
            raise ContractError("UNSUPPORTED_FORMAT", "track ends inside a variable-length number")
        b = body[i]
        i += 1
        v = (v << 7) | (b & 0x7F)
        if not b & 0x80:
            return v, i


def parse_mid(data):
    """(first bpm, [(start_s, dur_s, pitch, velocity_0_1)]) from SMF bytes. SMF 0/1, PPQ 960 only (S1b).

    Audit 2026-11-16: the SMF header is checked (format 0/1, declared track count = tracks found, no
    truncated track); every tempo change moves the note times (not only the first tempo); and two
    overlapping note-ons of the same pitch are two notes (first in, first out), not one overwriting the other.
    """
    if len(data) < 14 or data[:4] != b"MThd":
        raise ContractError("UNSUPPORTED_FORMAT", "not a Standard MIDI File")
    hlen = struct.unpack(">I", data[4:8])[0]
    fmt, ntrks, division = struct.unpack(">HHH", data[8:14])
    if hlen < 6 or fmt not in (0, 1):
        raise ContractError("UNSUPPORTED_FORMAT", f"SMF format {fmt} not supported (0 or 1)")
    if division & 0x8000:
        raise ContractError("UNSUPPORTED_FORMAT", "SMPTE time division not allowed (PPQ required)")
    if division != media.PPQ:
        raise ContractError("UNSUPPORTED_FORMAT", f"PPQ {division} != 960 (S1b)")
    pos, tempos, raw, tracks = 8 + hlen, [], [], 0
    while pos + 8 <= len(data):
        if data[pos:pos + 4] != b"MTrk":
            raise ContractError("UNSUPPORTED_FORMAT", "bad track chunk")
        tlen = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + tlen]
        if len(body) != tlen:
            raise ContractError("UNSUPPORTED_FORMAT", "truncated track")
        i, tick, running, open_, ended = 0, 0, None, {}, False
        try:
            while i < len(body):
                delta, i = _vlq(body, i)
                tick += delta
                status = body[i]
                if status & 0x80:
                    i += 1
                    if status < 0xF0:
                        running = status
                elif running is None:
                    raise ContractError("UNSUPPORTED_FORMAT", "data byte without a status")
                else:
                    status = running
                if status == 0xFF:
                    mtype = body[i]
                    ln, i = _vlq(body, i + 1)
                    if mtype == 0x51 and ln == 3:
                        tempos.append((tick, int.from_bytes(body[i:i + 3], "big")))
                    i += ln
                    if mtype == 0x2F:
                        ended = True
                        break
                elif status in (0xF0, 0xF7):
                    ln, i = _vlq(body, i)
                    i += ln
                else:
                    kind, ch = status & 0xF0, status & 0x0F
                    if kind in (0x80, 0x90):
                        pitch, vel = body[i], body[i + 1]
                        i += 2
                        if kind == 0x90 and vel > 0:
                            open_.setdefault((ch, pitch), []).append((tick, vel))
                        elif open_.get((ch, pitch)):
                            st, v = open_[(ch, pitch)].pop(0)
                            raw.append((st, tick - st, pitch, v))
                    elif kind in (0xA0, 0xB0, 0xE0):
                        i += 2
                    else:
                        i += 1
        except IndexError:
            raise ContractError("UNSUPPORTED_FORMAT", "track ends inside an event")
        if not ended:
            raise ContractError("UNSUPPORTED_FORMAT", "track without end-of-track")
        tracks += 1
        pos += 8 + tlen
    if tracks != ntrks:
        raise ContractError("UNSUPPORTED_FORMAT", f"header says {ntrks} tracks, found {tracks}")
    if not tempos:
        raise ContractError("UNSUPPORTED_FORMAT", "no tempo map (contract requires one)")
    if not raw:
        raise ContractError("UNSUPPORTED_FORMAT", "MIDI has no notes")
    tempos.sort()
    if tempos[0][0] > 0:  # SMF default before the first tempo event: 120 BPM
        tempos.insert(0, (0, 500000))
    segs, secs, last_tick, last_tempo = [], 0.0, 0, tempos[0][1]
    for t, tempo in tempos:
        secs += (t - last_tick) * last_tempo / 1e6 / media.PPQ
        segs.append((t, secs, tempo))
        last_tick, last_tempo = t, tempo

    def at(tick):
        t0, s0, tempo = segs[0]
        for seg in segs:
            if seg[0] <= tick:
                t0, s0, tempo = seg
            else:
                break
        return s0 + (tick - t0) * tempo / 1e6 / media.PPQ

    notes = sorted((at(s), max(at(s + max(d, 1)) - at(s), 1e-4), p, v / 127.0) for s, d, p, v in raw)
    return 60e6 / segs[0][2], notes


def read_mid(path):
    return parse_mid(media.read_file_capped(path, media.MAX_MIDI_BYTES, "MIDI"))


def validate(reg, req):
    if req.get("schemaVersion") != "1.0":
        raise ContractError("UNSUPPORTED_SCHEMA", "only schemaVersion 1.0 is supported")
    errs = validation_errors(reg, "service-task-request", req)
    if errs:
        raise ContractError("SCHEMA_INVALID", f"service-task-request: {len(errs)} error(s)", {"errors": errs[:20]})
    if req["profile"] != "render" or req["params"].get("engine") != "vita":
        raise ContractError("UNSUPPORTED_OPERATION", "aicore-vita only renders profile render with params.engine 'vita'")
    p = req["params"]
    mids = [r for r in req["inputs"] if r["role"] == "midi"]
    if len(mids) != 1 or len(req["inputs"]) != 1:
        raise ContractError("SCHEMA_INVALID", "engine vita needs exactly one input with role midi")
    if not Path(req["outputDir"]).is_dir():
        raise ContractError("BROKEN_REFERENCE", f"outputDir does not exist: {req['outputDir']}")
    taken = [n for n in ("render.wav", "report.json", "manifest.json") if (Path(req["outputDir"]) / n).exists()]
    if taken:
        raise ContractError("OUTPUT_EXISTS", "outputDir already holds a published job; use a new outputDir",
                            {"existing": taken})
    checked = []
    for ref in req["inputs"]:
        f = Path(ref["path"])
        if not f.is_file():
            raise ContractError("BROKEN_REFERENCE", f"input does not exist: {f}", {"role": ref["role"], "path": str(f)})
        try:
            data = media.read_file_capped(f, media.MAX_MIDI_BYTES, "MIDI input")  # read ONCE: hashed and parsed
        except media.MediaError as e:
            raise ContractError(e.code, str(e))
        if "bytes" in ref and ref["bytes"] != len(data):
            raise ContractError("HASH_MISMATCH", f"declared bytes do not match {f.name}")
        sha = media.sha256_bytes(data)
        if "sha256" in ref and ref["sha256"] != sha:
            raise ContractError("HASH_MISMATCH", f"sha256 mismatch for {f.name}")
        checked.append({"role": ref["role"], "path": ref["path"], "bytes": len(data), "sha256": sha, "data": data})
    if p.get("preset") and not Path(p["preset"]).is_file():
        raise ContractError("BROKEN_REFERENCE", f"preset does not exist: {p['preset']}")
    return checked


def wav_float32(buf, rate):
    import numpy as np
    inter = np.ascontiguousarray(buf.T, dtype="<f4").tobytes()
    fmt = struct.pack("<HHIIHH", 3, 2, rate, rate * 8, 8, 32)
    return (b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(inter)) + b"WAVE" + b"fmt " +
            struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(inter)) + inter)


def job_main():
    reg = Registry(SCHEMA_DIR)
    task_id = "t-" + uuid.uuid4().hex[:12]
    raw = sys.stdin.buffer.readline(media.MAX_LINE_BYTES + 1)
    rid = "unknown"

    def fail(e):
        emit({"event": "error", "schemaVersion": "1.0", "contract": CONTRACT, "taskId": task_id, "requestId": rid,
              "error": e.doc(rid), "at": now()})
        return 2

    if len(raw) > media.MAX_LINE_BYTES:
        return fail(ContractError("LIMIT_EXCEEDED", f"request line exceeds {media.MAX_LINE_BYTES} bytes"))
    if not raw.strip():
        return fail(ContractError("INVALID_JSON", "no request line on stdin"))
    try:
        req = media.loads_request(raw.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as e:
        return fail(ContractError("INVALID_JSON", f"request line is not JSON: {e}"))
    if not isinstance(req, dict):
        return fail(ContractError("SCHEMA_INVALID", "request line must be a JSON object"))
    r = req.get("requestId")
    if isinstance(r, str) and 0 < len(r) <= 128 and all(ch.isascii() and (ch.isalnum() or ch in "._:-") for ch in r):
        rid = r  # only a valid id is echoed: an invalid one would make the error line itself invalid
    try:
        checked = validate(reg, req)
    except ContractError as e:
        return fail(e)
    out = Path(req["outputDir"])
    staging = out / ".staging" / task_id
    published, seq, last = [], [0], [0.0]
    synthetic = bool(req.get("synthetic", False))

    def progress(phase, frac, msg=None):
        frac = max(last[0], min(1.0, frac))
        last[0] = frac
        seq[0] += 1
        d = {"event": "progress", "schemaVersion": "1.0", "eventSeq": seq[0], "taskId": task_id, "phase": phase,
             "fraction": round(frac, 4), "step": int(round(frac * 100)), "totalSteps": 100, "at": now()}
        if msg:
            d["message"] = msg[:200]
        emit(d)

    def publish(name, data):
        staging.mkdir(parents=True, exist_ok=True)
        p = out / name
        media.atomic_write(p, data, staging)
        published.append(p)
        return p

    def clean():
        import shutil
        shutil.rmtree(staging, ignore_errors=True)
        try:
            if staging.parent.exists() and not any(staging.parent.iterdir()):
                staging.parent.rmdir()
        except OSError:
            pass

    emit({"event": "start", "schemaVersion": "1.0", "contract": CONTRACT, "taskId": task_id, "requestId": rid,
          "profile": "render", "operation": "render", "service": dict(SERVICE), "synthetic": synthetic, "at": now()})
    t0 = time.monotonic()
    try:
        import numpy as np
        import vita
        p = req["params"]
        rate = int(p["sampleRate"])
        staging.mkdir(parents=True, exist_ok=True)
        progress("read", 0.0, "a ler o MIDI")
        mid = next(r for r in checked if r["role"] == "midi")
        bpm, notes = parse_mid(mid["data"])  # the bytes whose sha256 was checked
        last_end = max(s + d for s, d, _, _ in notes)
        seconds = p.get("lengthSeconds") or (last_end + float(p["tailSeconds"]))
        if not 0 < seconds <= 600:
            raise ContractError("LIMIT_EXCEEDED", f"render length {seconds:.2f} s outside (0, 600]")
        progress("read", 0.05, f"{len(notes)} notas, {bpm:.2f} BPM")
        progress("load", 0.06, "Vita")
        synth = vita.Synth()
        if rate != 44100:
            synth.set_sample_rate(rate)
        synth.set_bpm(bpm)
        preset = p.get("preset")
        if preset:
            if not synth.load_preset(str(preset)):
                raise ContractError("BACKEND_FAILED", f"Vita could not load preset {Path(preset).name}")
        else:
            synth.load_init_preset()
        progress("load", 0.1, f"preset {Path(preset).name if preset else 'init'}")
        total = int(round(seconds * rate))
        if 44 + total * 2 * 4 > MAX_WAV:  # before the buffer is allocated
            raise ContractError("LIMIT_EXCEEDED", f"{seconds:.1f} s at {rate} Hz would exceed the 128 MiB WAV cap")
        buf = np.zeros((2, total), dtype=np.float32)
        tail = float(p["tailSeconds"])  # as asked (schema: 0..30 s); it used to be cut at 4 s
        for k, (s, d, pitch, vel) in enumerate(notes):
            a = int(round(s * rate))
            if a >= total:
                continue
            audio = synth.render(int(pitch), float(vel), float(d), float(min(d + tail, (total - a) / rate)))
            n = min(audio.shape[1], total - a)
            buf[:, a:a + n] += audio[:, :n]
            if k % max(1, len(notes) // 10) == 0:
                progress("render", 0.1 + 0.8 * (k + 1) / len(notes))
        progress("render", 0.9)
        buf *= 10 ** (float(p.get("gainDb", 0)) / 20)
        peak = float(np.abs(buf).max()) if total else 0.0
        if not np.isfinite(peak):
            raise ContractError("BACKEND_FAILED", "Vita produced NaN/Inf samples")
        if peak < 1e-6:
            raise ContractError("BACKEND_FAILED", f"Vita rendered digital silence for {len(notes)} notes",
                                {"phase": "render", "notes": len(notes)})
        rms = float(np.sqrt((buf.astype(np.float64) ** 2).mean()))
        progress("write", 0.92)
        data = wav_float32(buf, rate)
        if len(data) > MAX_WAV:
            raise ContractError("LIMIT_EXCEEDED", "render would exceed 128 MiB")
        wav = publish("render.wav", data)
        outputs = [{"role": "audio", "path": str(wav), "bytes": len(data), "sha256": media.sha256_file(wav),
                    "format": {"container": "wav", "encoding": "float32", "sampleRate": rate, "channels": 2,
                               "durationSeconds": round(total / rate, 6), "bytes": len(data)}}]
        metrics = {"synthetic": synthetic, "engine": "vita", "vitaVersion": getattr(vita, "__version__", "?"),
                   "preset": preset, "notes": len(notes), "bpmUsed": bpm, "ppq": media.PPQ, "sampleRate": rate,
                   "seconds": total / rate, "peakDbfs": 20 * np.log10(max(peak, 1e-10)),
                   "rmsDbfs": 20 * np.log10(max(rms, 1e-10)), "silent": False,
                   "wallSeconds": round(time.monotonic() - t0, 3)}
        rdata = json.dumps({"schemaVersion": "1.0", "taskId": task_id, "metrics": metrics, "params": p,
                            "validation": "unreviewed"}, ensure_ascii=True, indent=2).encode()
        rp = publish("report.json", rdata)
        outputs.append({"role": "report", "path": str(rp), "bytes": len(rdata), "sha256": media.sha256_file(rp),
                        "format": {"container": "json", "bytes": len(rdata)}})
        manifest = {"schemaVersion": "1.0", "contract": CONTRACT, "taskId": task_id, "requestId": rid,
                    "profile": "render", "operation": "render", "createdAt": now(), "service": dict(SERVICE),
                    "params": p, "inputs": [{k: r[k] for k in ("role", "path", "bytes", "sha256")} for r in checked],
                    "outputs": outputs, "metrics": metrics, "validation": "unreviewed", "synthetic": synthetic}
        mdata = json.dumps(manifest, ensure_ascii=True, indent=2).encode()
        mp = publish("manifest.json", mdata)
        clean()
        progress("write", 1.0, "manifesto publicado")
        emit({"event": "done", "schemaVersion": "1.0", "contract": CONTRACT, "taskId": task_id, "requestId": rid,
              "result": {"outputs": outputs, "manifestPath": str(mp), "metrics": metrics, "validation": "unreviewed",
                         "manifestSha256": media.sha256_bytes(mdata), "manifestBytes": len(mdata)},
              "synthetic": synthetic, "at": now()})
        return 0
    except Exception as e:  # noqa: BLE001 - typed error, never fake success
        err = e if isinstance(e, ContractError) else ContractError(
            "OUT_OF_MEMORY" if isinstance(e, MemoryError) else "BACKEND_FAILED", f"{type(e).__name__}: {e}")
        for f in published:
            try:
                f.unlink()
            except OSError:
                pass
        clean()
        return fail(err)


def hello():
    emit({"schemaVersion": "1.0", "contract": CONTRACT, "service": {**SERVICE, "pid": os.getpid()},
          "profiles": [{"name": "render", "operations": ["render"]}],
          "limits": {"maxInputBytes": media.MAX_MIDI_BYTES, "maxOutputBytes": MAX_WAV, "maxDurationSeconds": 600,
                     "maxResponseBytes": 2 * 1024 * 1024},
          "ppq": 960, "quantizeGrid": ["1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "off"], "authRequired": False,
          "startedAt": now()})
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["hello", "job"])
    a = ap.parse_args()
    protect_stdout()
    if os.name == "nt":
        try:
            import ctypes
            ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)
        except Exception:  # noqa: BLE001
            pass
    return hello() if a.cmd == "hello" else job_main()


if __name__ == "__main__":
    sys.exit(main())
