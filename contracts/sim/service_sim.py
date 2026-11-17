"""S1b contract simulator — CPU, stdlib only, ZERO models/GPU/weights/network.

Exercises `aicore.service/1` (contracts/CONTRACT.md) with a deterministic stand-in for the S2
audio→MIDI service, over the ONE transport the slimmed contract keeps: **subprocesso JSON-lines**
(CONTRACT.md §2). It is NOT a transcriber: notes come from an envelope onset detector plus a fixed
synthetic scale, and every output is marked `synthetic=true` / `validation=unreviewed`.

  python service_sim.py hello [--fault bad-contract]
  python service_sim.py job [--fault FAULT]        # reads ONE request JSON line on stdin
  python service_sim.py fixture --out mix.wav --seconds 8 --bpm 145 --hits-per-beat 1

There is no HTTP server, no port, no polling, no cancel-file, no parent-pid watchdog and no
idempotency store: those left with the deferred features (CONTRACT.md §13). What stayed, because file
integrity is not optional: atomic staging + `os.replace`, sha256 per file in `manifest.json`, and a
fail-closed schema validator (`jsonschema_mini.py`).

Ruling S1b: the published `.mid` is SMF type 1 at **PPQ 960** — the resolution the Core DAW records
its templates at. `media.midi_bytes` refuses any other ppq, `media.read_midi` refuses to read one, and
`midi-params.schema.json` pins `ppq` to 960, so a 480 file cannot cross this boundary unnoticed. The
quantization grid goes down to **1/32 = 120 ticks**, and a note's length always comes from the measured
hold, never from the grid — that is how rolls and sub-grid notes survive.

FAULTs are TEST HOOKS for the gate's canaries, never real-service behavior:
  backend-failed  fail in 'separate' with BACKEND_FAILED
  oom             fail in 'transcribe' with OUT_OF_MEMORY
  fake-success    report done WITHOUT writing the .mid (gate must catch)
  wrong-sha       report a sha256 that does not match the bytes on disk (gate must catch)
  bad-contract    hello/start/done echo aicore.service/2 (client must refuse)
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import media
from jsonschema_mini import Registry, validation_errors

CONTRACT = "aicore.service/1"
SERVICE = {"name": "midi-sim", "version": "sim-1.1.0"}
SCHEMA_DIR = Path(__file__).resolve().parent.parent / "schemas" / "v1"
# The caps of CONTRACT.md §6, the same numbers as media-format.schema.json (the gate compares them):
# WAV in/out (the stems) <= 128 MiB, MIDI <= 8 MiB, 600 s, 2 MiB per JSON line.
LIMITS = {"maxInputBytes": media.MAX_WAV_BYTES, "maxOutputBytes": media.MAX_WAV_BYTES,
          "maxDurationSeconds": 600, "maxResponseBytes": media.MAX_LINE_BYTES}
GRID_ORDER = ["1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "off"]
FAULTS = {"backend-failed", "oom", "fake-success", "wrong-sha", "bad-contract", "late-fail"}
# The names a midi job publishes; an outputDir that already holds one belongs to an earlier job (§2.7).
FINAL_NAMES = ("transcription.mid", "report.json", "manifest.json")
ID_CHARS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._:-")
# code -> retryable (CONTRACT.md §7: no HTTP statuses in this contract anymore).
RETRYABLE = {"SCHEMA_INVALID": False, "UNSUPPORTED_SCHEMA": False, "INVALID_JSON": False,
             "LIMIT_EXCEEDED": False, "BROKEN_REFERENCE": False, "HASH_MISMATCH": False,
             "UNSUPPORTED_OPERATION": False, "UNSUPPORTED_FORMAT": False, "BACKEND_FAILED": False,
             "OUT_OF_MEMORY": False, "OUTPUT_EXISTS": False, "WORKER_UNAVAILABLE": True}


def safe_id(value):
    """A requestId echoed in an error line must itself be a valid id, or the error line breaks the schema."""
    if isinstance(value, str) and 1 <= len(value) <= 128 and set(value) <= ID_CHARS:
        return value
    return "unknown"
PHASES = ("read", "separate", "transcribe", "quantize", "write")
SLEEP = 0.02  # pacing only: keeps the progress lines a second apart in spirit, not in wall time


def now():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


class ContractError(Exception):
    def __init__(self, code, message, details=None):
        super().__init__(message)
        self.code = code
        self.message = message[:500]
        self.details = details or {}

    def to_dict(self, request_id=None):
        out = {"code": self.code, "message": self.message, "requestId": request_id,
               "retryable": RETRYABLE.get(self.code, False)}
        if self.details:
            out["details"] = self.details
        return out


def log(text):
    sys.stderr.write(f"[midi-sim] {text}\n")
    sys.stderr.flush()


def emit_line(doc):
    sys.stdout.write(json.dumps(doc, ensure_ascii=True, allow_nan=False) + "\n")
    sys.stdout.flush()


# ---- hello / capabilities ----

def hello_doc(fault=""):
    """One stdout line: what this service accepts. The client reads it BEFORE submitting (§3)."""
    return {"schemaVersion": "1.0",
            "contract": "aicore.service/2" if fault == "bad-contract" else CONTRACT,
            "service": {**SERVICE, "pid": os.getpid()},
            "profiles": [{"name": "midi", "operations": ["transcribe"]}],
            "limits": dict(LIMITS),
            "ppq": media.PPQ,
            "quantizeGrid": list(GRID_ORDER),
            "authRequired": False,
            "startedAt": now()}


# ---- submission validation (schema + semantics + inputs) ----

def check_inputs(request, deep=True):
    """Existence, caps and — when deep — declared hashes + full decode.

    Returns {"mix": wavFormat} (cheap pass, header only) or, when deep, {"mix": (wavFormat, samples),
    "inputs": [{role, path, bytes, sha256}]}. The deep pass reads each file ONCE: the declared sha256 is
    checked on those bytes and the mix is decoded from the same bytes, so what was verified is what is
    processed (audit 2026-11-16). Every input with a declared sha256 is checked, not only the mix.
    """
    infos = {"inputs": []}
    for ref in request["inputs"]:
        p = Path(ref["path"])
        if not p.is_file():
            raise ContractError("BROKEN_REFERENCE", f"input does not exist: {p}",
                                {"role": ref["role"], "path": str(p)})
        size = p.stat().st_size
        if size > LIMITS["maxInputBytes"]:
            raise ContractError("LIMIT_EXCEEDED",
                                f"input is {size} bytes; the cap is {LIMITS['maxInputBytes']}",
                                {"role": ref["role"]})
        if "bytes" in ref and ref["bytes"] != size:
            raise ContractError("HASH_MISMATCH", f"declared bytes {ref['bytes']} != actual {size}",
                                {"role": ref["role"]})
        if not deep:
            if ref["role"] == "mix":
                try:
                    infos["mix"] = media.read_wav_header(p)
                except media.MediaError as e:
                    raise ContractError(e.code, str(e), {"role": ref["role"]})
            continue
        try:
            data = media.read_file_capped(p, LIMITS["maxInputBytes"], f"input {ref['role']}")
        except media.MediaError as e:
            raise ContractError(e.code, str(e), {"role": ref["role"]})
        actual = media.sha256_bytes(data)
        if "bytes" in ref and ref["bytes"] != len(data):
            raise ContractError("HASH_MISMATCH", "the input changed size while it was read", {"role": ref["role"]})
        if "sha256" in ref and actual != ref["sha256"]:
            raise ContractError("HASH_MISMATCH", f"sha256 mismatch for {p}",
                                {"role": ref["role"], "expected": ref["sha256"], "actual": actual})
        infos["inputs"].append({"role": ref["role"], "path": ref["path"], "bytes": len(data), "sha256": actual})
        if ref["role"] == "mix":
            try:
                infos["mix"] = media.wav_decode(data)
            except media.MediaError as e:
                raise ContractError(e.code, str(e), {"role": ref["role"]})
    return infos


def validate_request(reg, request):
    if isinstance(request, dict) and request.get("schemaVersion") not in (None, "1.0"):
        raise ContractError("UNSUPPORTED_SCHEMA", "only schemaVersion 1.0 is supported",
                            {"schemaVersion": str(request.get("schemaVersion"))[:16]})
    errors = validation_errors(reg, "service-task-request", request)
    if errors:
        raise ContractError("SCHEMA_INVALID", f"service-task-request: {len(errors)} error(s)",
                            {"errors": errors[:20]})
    if request["profile"] != "midi" or request["operation"] != "transcribe":
        raise ContractError("UNSUPPORTED_OPERATION",
                            f"{request['profile']}/{request['operation']} not announced by this simulator")
    out = Path(request["outputDir"])
    if not out.is_dir():
        raise ContractError("BROKEN_REFERENCE", f"outputDir does not exist: {out}", {"outputDir": str(out)})
    taken = [n for n in FINAL_NAMES if (out / n).exists()] + sorted(p.name for p in out.glob("stem-*.wav"))
    if taken:
        # §2.7: a resend goes to a NEW outputDir; publishing here would overwrite (or, on failure, lose)
        # what an earlier job published
        raise ContractError("OUTPUT_EXISTS", "outputDir already holds a published job; use a new outputDir",
                            {"existing": taken[:8]})
    meta = check_inputs(request, deep=False)
    duration = meta["mix"]["durationSeconds"]
    params = request["params"]
    start = params.get("startSeconds", 0)
    length = params.get("lengthSeconds", duration)
    if start >= duration - 1e-9:
        raise ContractError("LIMIT_EXCEEDED",
                            f"startSeconds {start} is past the end of the input ({duration}s): no audio in scope",
                            {"duration": duration, "start": start})
    if start + length > duration + 1e-6:
        raise ContractError("LIMIT_EXCEEDED", "startSeconds+lengthSeconds beyond input duration",
                            {"duration": duration, "start": start, "length": length})
    return check_inputs(request, deep=True)


# ---- "separation" (stand-in for Demucs) ----

def separate(mix, rate, ratio=0.25):
    """Split the mix into a percussive band and everything else — deterministic, model-free.

    A real S2 service runs Demucs here. The simulator cannot (no weights, no GPU by contract), so it
    does the honest cheap thing: wherever the amplitude envelope (`media.envelope`, the same follower
    the onset detector uses) sits above `ratio` * peak, the sample is attributed to `drums`; the rest
    goes to `bass`; `other` is silence. That is a band split, NOT source separation — its only job is
    to prove the stems path carries one file per requested part, atomically written and hashed.
    """
    env = media.envelope(mix, rate)
    peak = max(env) if env else 0.0
    drums, bass = [], []
    for x, level in zip(mix, env):
        if peak > 0.0 and level > ratio * peak:
            drums.append(x)
            bass.append(0.0)
        else:
            drums.append(0.0)
            bass.append(x)
    return {"drums": drums, "bass": bass, "other": [0.0] * len(mix)}


# ---- job engine ----

class Job:
    """One request, one process, one job: `start` → `progress`* → `done` | `error`."""

    def __init__(self, request, task_id, fault="", checked=None):
        self.request = request
        self.checked = checked or {}
        self.staged = []      # (part, final): written + fsynced in staging, not visible yet
        self.published = []   # finals this job moved into outputDir (removed again if the job fails)
        self.task_id = task_id
        self.fault = fault
        self.event_seq = 0
        self.request_id = request["requestId"]
        self.contract = "aicore.service/2" if fault == "bad-contract" else CONTRACT
        self.output_dir = Path(request["outputDir"])
        self.staging = self.output_dir / ".staging" / task_id
        self.last_fraction = -1.0

    def _echo(self, doc):
        doc["contract"] = self.contract  # echoed on every line that carries it (schema: const /1)
        return doc

    def start_line(self):
        emit_line(self._echo({"event": "start", "schemaVersion": "1.0", "taskId": self.task_id,
                              "requestId": self.request_id, "profile": self.request["profile"],
                              "operation": self.request["operation"], "service": dict(SERVICE),
                              "synthetic": True, "at": now()}))

    def _progress(self, phase, fraction, step, total, message=None):
        if phase not in PHASES:
            raise ContractError("BACKEND_FAILED", f"phase {phase!r} not in this contract's enum")
        if fraction < self.last_fraction:  # monotonic non-decreasing (CONTRACT.md §4)
            raise ContractError("BACKEND_FAILED", f"fraction went backwards: {fraction} < {self.last_fraction}")
        self.last_fraction = fraction
        self.event_seq += 1
        doc = {"event": "progress", "schemaVersion": "1.0", "eventSeq": self.event_seq,
               "taskId": self.task_id, "phase": phase, "fraction": round(fraction, 4),
               "step": step, "totalSteps": total, "at": now()}
        if message:
            doc["message"] = message[:200]
        emit_line(doc)

    def _sleep(self):
        time.sleep(SLEEP)

    def _stage(self, final_path, data):
        """Write <staging>/<name>.part and fsync it. Nothing reaches outputDir before _publish."""
        self.staging.mkdir(parents=True, exist_ok=True)
        part = self.staging / (Path(final_path).name + ".part")
        with open(part, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        self.staged.append((part, Path(final_path)))
        return part

    def _publish(self):
        """os.replace every staged file into outputDir, manifest last (it is staged last)."""
        for part, final in self.staged:
            if self.fault == "late-fail" and self.published:
                # SABOTAGE (late-fail): a failure after the first file went public; the rollback must remove it
                raise ContractError("BACKEND_FAILED", "synthetic fault while publishing",
                                    {"phase": "write", "synthetic": True})
            os.replace(part, final)
            self.published.append(final)
        self.staged = []

    def _fail_lines(self, err):
        # a failed job publishes nothing (§7 rule 4): undo what this job already moved into outputDir
        for final in self.published:
            try:
                final.unlink()
            except OSError:
                pass
        self.published = []
        media.cleanup_staging(self.staging)
        emit_line(self._echo({"event": "error", "schemaVersion": "1.0", "taskId": self.task_id,
                              "requestId": self.request_id, "error": err.to_dict(self.request_id),
                              "at": now()}))
        return 2

    def run(self):
        t0 = time.monotonic()
        params = self.request["params"]
        bpm = params.get("bpm") or 145.0
        seed = params.get("seed", 42)
        quantize = params["quantize"]
        try:
            self.staging.mkdir(parents=True, exist_ok=True)  # from the start, so terminal cleanup is provable
            self._progress("read", 0.0, 0, 100)
            info, mix = self.checked["mix"]  # decoded from the very bytes whose sha256 was checked
            rate = info["sampleRate"]
            start = params.get("startSeconds", 0)
            length = params.get("lengthSeconds", info["durationSeconds"])
            mix = mix[int(start * rate):int((start + length) * rate)]
            seconds = len(mix) / rate
            self._sleep()

            self._progress("separate", 0.05, 5, 100, "banda transiente vs tonal (stand-in de separação)")
            if self.fault == "backend-failed":
                self._sleep()
                raise ContractError("BACKEND_FAILED", "synthetic backend fault in separate phase",
                                    {"phase": "separate", "synthetic": True})
            components = separate(mix, rate)  # drums/bass/other — see helper docstring
            self._progress("separate", 0.4, 40, 100)
            self._sleep()

            self._progress("transcribe", 0.45, 45, 100, "onsets + pitch medidos do WAV sintético")
            if self.fault == "oom":
                self._sleep()
                raise ContractError("OUT_OF_MEMORY", "synthetic OOM in transcribe phase (no retry, no offload)",
                                    {"phase": "transcribe", "synthetic": True})
            onsets = media.detect_onsets(mix, rate)          # [(start_s, hold_s)] — hold from the audio
            bands = [media.classify_hit(mix, rate, t) for t, _ in onsets]
            self._progress("transcribe", 0.6, 60, 100)
            self._sleep()

            self._progress("quantize", 0.65, 65, 100, f"grelha {quantize}")
            step_ticks = media.grid_ticks(quantize)          # 1/32 -> 120 ticks at PPQ 960
            ticks_per_sec = media.PPQ * bpm / 60.0
            phase_tick = int(onsets[0][0] * ticks_per_sec) % step_ticks if onsets else 0
            notes = []
            for (t, hold), band in zip(onsets, bands):
                tick = int(round(t * ticks_per_sec - phase_tick) / step_ticks) * step_ticks + phase_tick
                dur = max(1, round(hold * ticks_per_sec))    # length of the measured hold, never the grid
                pitch = params["kickNote"] if band == "low" else params["kickNote"] + 2
                notes.append((max(0, tick), dur, min(127, max(0, pitch)), 100, 9))
            for t, dur, pitch in media.bass_schedule(seconds, bpm, seed):
                tick = int(round(t * ticks_per_sec - phase_tick) / step_ticks) * step_ticks + phase_tick
                notes.append((max(0, tick), max(1, round(dur * ticks_per_sec)), pitch, 90, 0))
            merged = len(notes) - len({(s, p) for s, _, p, _, _ in notes})
            self._progress("quantize", 0.8, 80, 100)
            self._sleep()

            self._progress("write", 0.85, 85, 100)
            outputs = []
            mid_data = media.midi_bytes(bpm, notes)           # refuses ppq != 960 by construction
            mid_path = self.output_dir / "transcription.mid"
            mid_part = None
            if self.fault != "fake-success":
                mid_part = self._stage(mid_path, mid_data)
            # SABOTAGE (fake-success): the .mid is never written but the result claims it exists.
            # SABOTAGE (wrong-sha): the file is real, the reported sha256 is not.
            if self.fault == "fake-success":
                mid_sha = media.sha256_bytes(mid_data)
                mid_info = {"container": "midi", "smfType": 1, "ppq": media.PPQ, "tracks": 3,
                            "durationSeconds": round(seconds, 6), "bytes": len(mid_data)}
                log("fake-success: .mid not written, done will claim it exists")
            else:
                mid_sha = media.sha256_file(mid_part)
                if self.fault == "wrong-sha":
                    mid_sha = "0" * 64
                    log("wrong-sha: done will report a sha256 that does not match the bytes")
                mid_info = media.midi_format_of(media.read_midi(mid_part))
            outputs.append({"role": "midi", "path": str(mid_path), "bytes": len(mid_data),
                            "sha256": mid_sha, "format": mid_info})
            if params.get("includeStems"):
                for part in params["parts"]:
                    comp = components.get(part, [0.0] * len(mix))
                    stereo = [s for v in comp[:len(mix)] for s in (v, v)]
                    data = media.wav_bytes_float32(stereo, rate, 2)
                    sp = self.output_dir / f"stem-{part}.wav"
                    spart = self._stage(sp, data)
                    sinfo, _ = media.read_wav(spart)
                    outputs.append({"role": f"stem_{part}", "path": str(sp), "bytes": len(data),
                                    "sha256": media.sha256_file(spart), "format": sinfo})
            metrics = {"synthetic": True, "simulation": "deterministic stand-in, no models",
                       "onsetsDetected": len(onsets), "notesWritten": len(notes),
                       "mergedEvents": merged, "gridTicks": step_ticks, "quantize": quantize,
                       "ppq": media.PPQ, "bpmUsed": bpm, "wallSeconds": round(time.monotonic() - t0, 3)}
            report = {"schemaVersion": "1.0", "taskId": self.task_id, "metrics": metrics,
                      "params": params, "validation": "unreviewed"}
            report_data = json.dumps(report, ensure_ascii=True, indent=2).encode()
            rp = self.output_dir / "report.json"
            outputs.append({"role": "report", "path": str(rp), "bytes": len(report_data),
                            "sha256": media.sha256_file(self._stage(rp, report_data)),
                            "format": {"container": "json", "bytes": len(report_data)}})
            manifest = {"schemaVersion": "1.0", "contract": CONTRACT, "taskId": self.task_id,
                        "requestId": self.request_id, "profile": self.request["profile"],
                        "operation": self.request["operation"], "createdAt": now(), "service": dict(SERVICE),
                        "params": params,
                        # the hashes of the bytes that were read and processed, not a second read
                        "inputs": list(self.checked["inputs"]),
                        "outputs": outputs, "metrics": metrics, "validation": "unreviewed", "synthetic": True}
            manifest_path = self.output_dir / "manifest.json"
            manifest_data = json.dumps(manifest, ensure_ascii=True, indent=2).encode()
            self._stage(manifest_path, manifest_data)
            self._publish()  # all or nothing: a failure here removes what was already published
            media.cleanup_staging(self.staging)
            result = {"outputs": outputs, "manifestPath": str(manifest_path), "metrics": metrics,
                      "validation": "unreviewed",
                      "manifestSha256": media.sha256_bytes(manifest_data), "manifestBytes": len(manifest_data)}
            self._progress("write", 1.0, 100, 100, "manifest publicado")
            emit_line(self._echo({"event": "done", "schemaVersion": "1.0", "taskId": self.task_id,
                                  "requestId": self.request_id, "result": result, "synthetic": True,
                                  "at": now()}))
            return 0
        except ContractError as e:
            return self._fail_lines(e)
        except media.MediaError as e:
            return self._fail_lines(ContractError(e.code, str(e), {"synthetic": True}))
        except Exception as e:  # any unexpected failure is reported, never fake success
            return self._fail_lines(ContractError("BACKEND_FAILED", f"{type(e).__name__}: {e}"[:500],
                                                  {"synthetic": True}))


# ---- transport: one request line on stdin ----

def job_main(fault):
    reg = Registry(SCHEMA_DIR)
    task_id = "t-" + uuid.uuid4().hex[:12]
    def early(code, message):
        emit_line({"event": "error", "schemaVersion": "1.0", "contract": CONTRACT, "taskId": task_id,
                   "requestId": "unknown", "error": ContractError(code, message[:500]).to_dict("unknown"),
                   "at": now()})
        return 2

    # §2.5: the 2 MiB cap is enforced while reading, never after holding an unbounded line in memory
    raw = sys.stdin.buffer.readline(media.MAX_LINE_BYTES + 1)
    if len(raw) > media.MAX_LINE_BYTES:
        return early("LIMIT_EXCEEDED", f"request line exceeds {media.MAX_LINE_BYTES} bytes")
    if not raw.strip():
        return early("INVALID_JSON", "no request line on stdin")
    try:
        request = media.loads_request(raw.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as e:
        return early("INVALID_JSON", f"request line is not JSON: {e}")
    request_id = safe_id(request.get("requestId")) if isinstance(request, dict) else "unknown"

    def fail(err):
        emit_line({"event": "error", "schemaVersion": "1.0", "contract": CONTRACT, "taskId": task_id,
                   "requestId": request_id, "error": err.to_dict(request_id), "at": now()})
        return 2

    if not isinstance(request, dict):
        return fail(ContractError("SCHEMA_INVALID", "request line must be a JSON object"))
    try:
        checked = validate_request(reg, request)
    except ContractError as e:
        return fail(e)
    job = Job(request, task_id, fault, checked)
    job.start_line()
    return job.run()


def fixture_main(a):
    """Deterministic synthetic WAV input for the gate: percussion at a chosen spacing + a bass line.

    NOT music and NOT a model output. Prints one JSON line with the ground truth the gate expects the
    service to reproduce (`hitEvents`, `bassEvents`), so 'which notes should exist' has one definition.
    """
    mix, _ = media.synth_mix(a.seconds, a.rate, a.bpm, a.seed, a.hits_per_beat, a.hit_seconds,
                             a.decay, a.alternate)
    data = media.wav_bytes_pcm16(mix, a.rate)
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    info, _ = media.read_wav(out)
    hits = media.hit_schedule(a.seconds, a.bpm, a.hits_per_beat)
    bass = media.bass_schedule(a.seconds, a.bpm, a.seed)
    print(json.dumps({"path": str(out.resolve()), "sha256": media.sha256_file(out), "bytes": len(data),
                      "format": info, "synthetic": True,
                      "schedule": {"bpm": a.bpm, "hitsPerBeat": a.hits_per_beat, "hitSeconds": a.hit_seconds,
                                   "decay": a.decay, "alternate": a.alternate, "seed": a.seed,
                                   "hitEvents": len(hits), "bassEvents": len(bass),
                                   "hitSpacingSeconds": round(hits[1] - hits[0], 6) if len(hits) > 1 else None,
                                   "hitSpacingTicksAt960": round((hits[1] - hits[0]) * media.PPQ * a.bpm / 60.0, 3)
                                   if len(hits) > 1 else None}}, ensure_ascii=True))
    return 0


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description="S1b contract simulator (CPU, no models, one transport)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    h = sub.add_parser("hello")
    h.add_argument("--fault", default="", choices=sorted(FAULTS) + [""])
    j = sub.add_parser("job")
    j.add_argument("--fault", default="", choices=sorted(FAULTS) + [""])
    f = sub.add_parser("fixture")
    f.add_argument("--out", required=True)
    f.add_argument("--seconds", type=float, default=8.0)
    f.add_argument("--bpm", type=float, default=145.0)
    f.add_argument("--rate", type=int, default=44100)
    f.add_argument("--seed", type=int, default=42)
    f.add_argument("--hits-per-beat", type=int, default=1, choices=(1, 4, 8, 16))
    f.add_argument("--hit-seconds", type=float, default=0.12)
    f.add_argument("--decay", type=float, default=0.03)
    f.add_argument("--alternate", action="store_true")
    args = ap.parse_args(argv)
    if args.cmd == "hello":
        emit_line(hello_doc(args.fault))
        return 0
    if args.cmd == "job":
        fault = args.fault or os.environ.get("AICORE_SIM_FAULT", "")
        if fault and fault not in FAULTS:
            log(f"unknown fault {fault!r}")
            return 2
        return job_main(fault)
    return fixture_main(args)


if __name__ == "__main__":
    sys.exit(main())
