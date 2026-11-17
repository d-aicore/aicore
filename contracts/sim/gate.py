"""S1b gate — exercises aicore.service/1 (slimmed) against the simulator, ONE transport.

Ruling S1b (owner, 2026-11-16): PPQ 960, quantize grid down to 1/32, happy path on one transport
(subprocess JSON-lines). The happy path is `request -> progress -> .mid -> typed error`: the typed
error is PART of the happy path, so a backend failure after progress lines is a positive scenario
here, not an optional extra.

Proves the contract AND proves the gate can fail (AGENTS gate 5):
  * positive: hello (ppq 960, grid to 1/32), happy job at 960, typed error after progress
    (BACKEND_FAILED, OUT_OF_MEMORY), 1/64 roll quantized at 1/32 loses nothing, notes shorter than
    the grid stay short, determinism / content moves hash, client refuses a wrong contract major,
    PPQ 480 refused at both ends, every example validates, every deferred item still documented.
  * failure-path: missing reference, hash mismatch, limits/format, schema-invalid (incl. the S1
    fields that left: cancelToken/cancelFile/parentPid/deadlineSeconds), bad stdin.
  * CANARIES the checks MUST reject: fake-success (done without .mid), wrong-sha (done with a hash
    that does not match), tampered/missing manifest file, validator mutations. With --noop the
    checks become no-ops, the canaries go undetected and the gate exits RED.

Every stdout/stderr line of every simulator process is recorded in gate-report.json.
Stdlib only, CPU, zero models/GPU/weights/network.

  python gate.py            # full matrix, exit 0 green
  python gate.py --noop     # no-op control, exit 1 RED (required proof)
  python gate.py --keep     # keep the temp workdir for inspection
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

SIM_DIR = Path(__file__).resolve().parent
CONTRACTS = SIM_DIR.parent
SCHEMA_DIR = CONTRACTS / "schemas" / "v1"
EXAMPLES = CONTRACTS / "examples"
sys.path.insert(0, str(SIM_DIR))
import client
import make_manifest
import media
import service_sim
from jsonschema_mini import Registry, UnsupportedKeyword, validation_errors

CONTRACT = service_sim.CONTRACT
PY = sys.executable
PHASES = ("read", "separate", "transcribe", "quantize", "write")
TERMINAL = ("done", "error")
DRUM_CH, BASS_CH = 9, 0


def now():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


class CheckFailed(Exception):
    pass


class Verifier:
    """The load-bearing checks. --noop replaces this whole object."""

    def __init__(self, reg):
        self.reg = reg

    def doc(self, name, document):
        errors = validation_errors(self.reg, name, document)
        if errors:
            raise CheckFailed(f"{name}: {errors[:5]}")

    def expect(self, cond, msg):
        if not cond:
            raise CheckFailed(msg)

    def stream(self, docs, code):
        """CONTRACT.md §2.3/§2.4/§4: start -> progress* -> exactly one terminal line; exit matches it."""
        for d in docs:
            self.doc("service-event", d)
        self.expect(docs, "no stdout lines at all")
        terminals = [i for i, d in enumerate(docs) if d["event"] in TERMINAL]
        self.expect(len(terminals) == 1, f"{len(terminals)} terminal lines (need exactly 1)")
        self.expect(terminals[0] == len(docs) - 1, "lines after the terminal line")
        last = docs[-1]
        want = 0 if last["event"] == "done" else 2
        self.expect(code == want, f"exit {code} does not match terminal '{last['event']}' (want {want})")
        if len(docs) > 1:
            self.expect(docs[0]["event"] == "start", "first line of a started job is not start")
        prog = [d for d in docs if d["event"] == "progress"]
        seqs = [d["eventSeq"] for d in prog]
        self.expect(all(b > a for a, b in zip(seqs, seqs[1:])), "eventSeq not strictly increasing")
        fr = [d["fraction"] for d in prog]
        self.expect(all(b >= a for a, b in zip(fr, fr[1:])), "fraction went backwards")
        for d in docs:
            if d["event"] in ("start", "done", "error"):
                self.expect(d["contract"] == CONTRACT, f"{d['event']} echoes {d['contract']!r}")
        return last

    def typed_error(self, last, code, out):
        self.expect(last["event"] == "error", f"terminal {last['event']} (want error {code}; never fake success)")
        self.doc("error", last["error"])
        self.expect(last["error"]["code"] == code, f"code {last['error']['code']} want {code}")
        self.expect(last["error"]["retryable"] is False, f"{code} marked retryable")
        self.expect("result" not in last, "error line carries result")
        if out is not None:
            self.expect(not (out / "transcription.mid").exists(), "failed job published a .mid")
            self.expect(not (out / "manifest.json").exists(), "failed job published a manifest")
            self.expect(not (out / ".staging").exists() or not any((out / ".staging").rglob("*")),
                        "staging not cleaned on error")

    def files(self, result, output_dir):
        out = Path(output_dir)
        for o in result["outputs"]:
            p = Path(o["path"])
            self.expect(p.is_file(), f"output missing: {p}")
            self.expect(p.stat().st_size == o["bytes"], f"bytes moved: {p}")
            self.expect(media.sha256_file(p) == o["sha256"], f"sha256 moved: {p}")
            if o["format"]["container"] == "midi":
                info = media.read_midi(p)  # raises on ppq != 960
                self.expect(info["ppq"] == 960, f"PPQ != 960: {info['ppq']}")
                self.expect(o["format"]["ppq"] == 960, "declared format.ppq != 960")
                self.expect(info["smfType"] in (0, 1), "bad SMF type")
            elif o["format"]["container"] == "wav":
                media.read_wav(p)
        mp = Path(result["manifestPath"])
        self.expect(mp.is_file(), f"manifest missing: {mp}")
        if "manifestSha256" in result:  # the manifest itself is checked by hash before it is trusted
            self.expect(media.sha256_file(mp) == result["manifestSha256"], "manifest.json sha256 != done line")
            self.expect(mp.stat().st_size == result.get("manifestBytes"), "manifest.json bytes != done line")
        manifest = json.loads(mp.read_text(encoding="utf-8"))
        self.doc("task-manifest", manifest)
        self.expect(manifest["validation"] == "unreviewed", "service claimed approval")
        self.expect(manifest["synthetic"] is True, "simulator output not marked synthetic")
        by_path = {o["path"]: o for o in manifest["outputs"]}
        for o in result["outputs"]:
            self.expect(o["path"] in by_path, f"output not in manifest: {o['path']}")
            self.expect(by_path[o["path"]]["sha256"] == o["sha256"], "manifest hash differs")
        self.expect(not list(out.rglob("*.part")), "staging .part left behind")
        self.expect(not (out / ".staging").exists() or not any((out / ".staging").rglob("*")),
                    ".staging not cleaned")

    def manifest(self, root):
        errors = make_manifest.verify(Path(root))
        if errors:
            raise CheckFailed(f"manifest: {errors[:5]}")

    def rejects(self, fn):
        """True iff fn raises CheckFailed (or a typed media error) — how canaries are detected."""
        try:
            fn()
            return False
        except (CheckFailed, media.MediaError):
            return True


class NoopVerifier:
    """No-op control: every check 'passes'. Canaries must then go undetected."""

    def doc(self, name, document):
        pass

    def expect(self, cond, msg):
        pass

    def stream(self, docs, code):
        return docs[-1] if docs else {"event": "none"}

    def typed_error(self, last, code, out):
        pass

    def files(self, result, output_dir):
        pass

    def manifest(self, root):
        pass

    def rejects(self, fn):
        return False


class Gate:
    def __init__(self, noop=False, keep=False):
        self.reg = Registry(SCHEMA_DIR)
        self.v = NoopVerifier() if noop else Verifier(self.reg)
        self.noop = noop
        self.keep = keep
        self.work = Path(tempfile.mkdtemp(prefix="aicore-s1b-gate-"))
        self.transcripts = []
        self.scenarios = []
        self.current = ""
        self.hashes = {}
        self.counts = {"mutations": 0, "mutationDetections": 0, "jobs": 0}

    # ---- infrastructure ----

    def sim(self, args, stdin_text=None, fault=""):
        """Run one simulator process (the only transport). Returns (exit, [docs]); every line recorded."""
        cmd = [PY, str(SIM_DIR / "service_sim.py")] + args
        if fault:
            cmd += ["--fault", fault]
        self.counts["jobs"] += 1
        p = subprocess.run(cmd, input=stdin_text, capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=180)
        docs = []
        for line in p.stdout.splitlines():
            if not line.strip():
                continue
            self.transcripts.append({"scenario": self.current, "stream": "stdout", "line": line})
            try:
                docs.append(json.loads(line))
            except ValueError:
                raise CheckFailed(f"stdout line is not JSON (contract broken): {line[:120]!r}")
            if not line.isascii():
                raise CheckFailed("stdout line is not ASCII (CONTRACT.md §2.2)")
        for line in p.stderr.splitlines():
            if line.strip():
                self.transcripts.append({"scenario": self.current, "stream": "stderr", "line": line})
        return p.returncode, docs

    def job(self, body, fault=""):
        return self.sim(["job"], json.dumps(body, ensure_ascii=True) + "\n", fault)

    def fixture(self, name, seconds=8.0, bpm=145.0, rate=44100, hits_per_beat=1, hit_seconds=0.12,
                decay=0.03, alternate=False):
        mix, _ = media.synth_mix(seconds, rate, bpm, 42, hits_per_beat, hit_seconds, decay, alternate)
        data = media.wav_bytes_pcm16(mix, rate)
        p = self.work / name
        p.write_bytes(data)
        sha = media.sha256_file(p)
        self.hashes[name] = sha
        return p, sha, len(data)

    def request_body(self, rid, mix, outdir, sha, nbytes, **over):
        params = {"ppq": 960, "quantize": "1/16", "parts": ["drums", "bass", "other"],
                  "kickNote": 36, "bassMono": True, "bpm": 145.0, "bpmSource": "provided",
                  "leadFilter": True, "splitOnOnsets": True, "includeStems": True,
                  "startSeconds": 0, "lengthSeconds": 8, "seed": 42}
        params.update(over.pop("params", {}))
        body = {"schemaVersion": "1.0", "requestId": rid, "profile": "midi", "operation": "transcribe",
                "params": params, "inputs": [{"role": "mix", "path": str(mix), "sha256": sha, "bytes": nbytes}],
                "outputDir": str(outdir), "synthetic": True}
        body.update(over)
        return body

    def outdir(self, name):
        d = self.work / name
        d.mkdir(parents=True, exist_ok=True)
        return d

    def midi_notes(self, result):
        mid = next(o for o in result["outputs"] if o["role"] == "midi")
        info = media.read_midi(Path(mid["path"]), with_notes=True)
        return mid, info

    # ---- bookkeeping ----

    def scenario(self, sid, name, fn, kind="positive"):
        self.current = sid
        t0 = time.monotonic()
        try:
            detail = fn() or ""
            verdict = "pass"
        except (CheckFailed, media.MediaError) as e:
            detail, verdict = f"{type(e).__name__}: {e}", "fail"
        except Exception as e:
            detail, verdict = f"UNEXPECTED {type(e).__name__}: {e}", "fail"
        self.scenarios.append({"id": sid, "name": name, "kind": kind, "verdict": verdict,
                               "detail": str(detail)[:600], "seconds": round(time.monotonic() - t0, 3)})
        mark = "OK " if verdict == "pass" else "RED"
        print(f"[{mark}] {sid} {name} ({kind}) {('- ' + str(detail)[:200]) if detail else ''}")

    def canary(self, sid, name, fn):
        self.current = sid
        t0 = time.monotonic()
        try:
            detected, detail = fn()
        except Exception as e:
            detected, detail = False, f"canary error {type(e).__name__}: {e}"
        verdict = "pass" if detected else "fail"
        self.scenarios.append({"id": sid, "name": name, "kind": "canary", "verdict": verdict,
                               "detail": str(detail)[:600], "seconds": round(time.monotonic() - t0, 3)})
        mark = "OK " if detected else "RED"
        print(f"[{mark}] {sid} {name} (canary) detected={detected} {('- ' + str(detail)[:200]) if detail else ''}")

    # ---- scenarios ----

    def s01(self):
        code, docs = self.sim(["hello"])
        self.v.expect(code == 0 and len(docs) == 1, f"hello exit {code}, {len(docs)} lines (want 0, 1)")
        doc = docs[0]
        self.v.doc("service-hello", doc)
        self.v.expect(doc["contract"] == CONTRACT, "wrong contract id")
        self.v.expect(doc["ppq"] == 960, f"hello ppq {doc['ppq']}")
        self.v.expect("1/32" in doc["quantizeGrid"], "1/32 not announced")
        self.v.expect(doc["authRequired"] is False, "auth required on a local subprocess")
        self.v.expect(any(p["name"] == "midi" and "transcribe" in p["operations"] for p in doc["profiles"]),
                      "midi/transcribe not announced")
        return f"contract={doc['contract']} ppq={doc['ppq']} grid={doc['quantizeGrid']}"

    def s02(self):
        mix, sha, nb = self.fixture("happy.wav")
        out = self.outdir("happy-out")
        code, docs = self.job(self.request_body("req-happy", mix, out, sha, nb))
        last = self.v.stream(docs, code)
        self.v.expect(last["event"] == "done", f"terminal {last['event']}: {last.get('error')}")
        phases = [d["phase"] for d in docs if d["event"] == "progress"]
        for ph in PHASES:
            self.v.expect(ph in phases, f"phase {ph} never observed")
        self.v.expect(last["synthetic"] is True and last["result"]["validation"] == "unreviewed",
                      "not synthetic/unreviewed")
        self.v.files(last["result"], out)
        mid, info = self.midi_notes(last["result"])
        drums = [n for n in info["noteEvents"] if n["channel"] == DRUM_CH]
        bass = [n for n in info["noteEvents"] if n["channel"] == BASS_CH]
        hits = media.hit_schedule(8.0, 145.0, 1)
        bassx = media.bass_schedule(8.0, 145.0, 42)
        self.v.expect(len(drums) == len(hits), f"drum notes {len(drums)} != fixture hits {len(hits)}")
        self.v.expect(len(bass) == len(bassx), f"bass notes {len(bass)} != fixture bass {len(bassx)}")
        self.hashes["happy.mid"] = mid["sha256"]
        return (f"exit 0; {len(docs)} lines; ppq={info['ppq']} drums={len(drums)}/{len(hits)} "
                f"bass={len(bass)}/{len(bassx)} mid_sha={mid['sha256'][:16]}")

    def _error_after_progress(self, fault, code_want, tag):
        mix, sha, nb = self.fixture(f"{tag}.wav")
        out = self.outdir(f"{tag}-out")
        code, docs = self.job(self.request_body(f"req-{tag}", mix, out, sha, nb), fault=fault)
        last = self.v.stream(docs, code)
        self.v.expect(docs[0]["event"] == "start", "typed error path did not start the job")
        nprog = sum(1 for d in docs if d["event"] == "progress")
        self.v.expect(nprog >= 1, "no progress before the error (path must be request->progress->error)")
        self.v.typed_error(last, code_want, out)
        self.v.expect(not any(d["event"] == "done" for d in docs), "done emitted on a failed job")
        return f"start + {nprog} progress -> error {code_want} retryable=false, exit {code}, no outputs, staging clean"

    def s03(self):
        return self._error_after_progress("backend-failed", "BACKEND_FAILED", "backend")

    def s04(self):
        return self._error_after_progress("oom", "OUT_OF_MEMORY", "oom")

    def s05(self):
        # 1/64 roll (25.86 ms apart at 145 BPM), alternating timbre, quantized at 1/32 (120 ticks)
        secs = 4.0
        mix, sha, nb = self.fixture("roll64.wav", seconds=secs, hits_per_beat=16, hit_seconds=0.02,
                                    decay=0.003, alternate=True)
        out = self.outdir("roll-out")
        body = self.request_body("req-roll", mix, out, sha, nb,
                                 params={"quantize": "1/32", "lengthSeconds": secs, "includeStems": False})
        code, docs = self.job(body)
        last = self.v.stream(docs, code)
        self.v.expect(last["event"] == "done", f"terminal {last['event']}: {last.get('error')}")
        self.v.files(last["result"], out)
        _, info = self.midi_notes(last["result"])
        drums = [n for n in info["noteEvents"] if n["channel"] == DRUM_CH]
        hits = media.hit_schedule(secs, 145.0, 16)
        self.v.expect(len(drums) == len(hits), f"roll lost events: {len(drums)} drum notes != {len(hits)} hits")
        keys = [(n["start"], n["pitch"]) for n in drums]
        self.v.expect(len(set(keys)) == len(keys), "two roll hits collapsed onto the same (tick, pitch)")
        phase = drums[0]["start"] % 120 if drums else 0
        self.v.expect(all(n["start"] % 120 == phase for n in drums), "drum start not on the 1/32 grid (120 ticks)")
        by_tick = {}
        for n in drums:
            by_tick.setdefault(n["start"], set()).add(n["pitch"])
        shared = sum(1 for p in by_tick.values() if len(p) >= 2)
        self.v.expect(shared >= 1, "no 1/32 slot carries two different pitches (roll pairs not proven)")
        self.v.expect(last["result"]["metrics"]["mergedEvents"] == 0, "simulator reports merged events")
        self.v.expect(last["result"]["metrics"]["gridTicks"] == 120, "gridTicks != 120 for 1/32")
        return (f"1/64 roll -> 1/32: {len(drums)}/{len(hits)} hits kept, unique (tick,pitch), "
                f"{shared} slots with 2 pitches, grid 120 ticks")

    def s06(self):
        # notes ~11 ms long (≈25 ticks at 145 BPM) quantized at 1/32 must stay short
        secs = 4.0
        mix, sha, nb = self.fixture("short.wav", seconds=secs, hits_per_beat=4, hit_seconds=0.011,
                                    decay=0.002)
        out = self.outdir("short-out")
        body = self.request_body("req-short", mix, out, sha, nb,
                                 params={"quantize": "1/32", "lengthSeconds": secs, "includeStems": False})
        code, docs = self.job(body)
        last = self.v.stream(docs, code)
        self.v.expect(last["event"] == "done", f"terminal {last['event']}: {last.get('error')}")
        self.v.files(last["result"], out)
        _, info = self.midi_notes(last["result"])
        durs = [n["dur"] for n in info["noteEvents"] if n["channel"] == DRUM_CH]
        hits = media.hit_schedule(secs, 145.0, 4)
        self.v.expect(len(durs) == len(hits), f"short hits lost: {len(durs)} != {len(hits)}")
        self.v.expect(durs and max(durs) < 120, f"short notes stretched to the grid: max {max(durs or [0])} ticks")
        return f"{len(durs)} notes, durations {min(durs or [0])}..{max(durs or [0])} ticks (< 120 = 1/32)"

    def s07(self):
        out = self.outdir("missing-out")
        body = self.request_body("req-missing", self.work / "does-not-exist.wav", out, "0" * 64, 100)
        code, docs = self.job(body)
        last = self.v.stream(docs, code)
        self.v.typed_error(last, "BROKEN_REFERENCE", out)
        return f"error BROKEN_REFERENCE, exit {code} (red, never skip)"

    def s08(self):
        mix, sha, nb = self.fixture("hash.wav")
        out = self.outdir("hash-out")
        code, docs = self.job(self.request_body("req-hash", mix, out, "0" * 64, nb))
        last = self.v.stream(docs, code)
        self.v.typed_error(last, "HASH_MISMATCH", out)
        return f"error HASH_MISMATCH, exit {code}"

    def s09(self):
        mix, sha, nb = self.fixture("limits.wav")
        out = self.outdir("limits-out")
        cases = []
        cases.append(("length 601", self.request_body("req-lim1", mix, out, sha, nb, params={"lengthSeconds": 601}),
                      "SCHEMA_INVALID"))
        hi, _ = media.synth_mix(1.0, 200000, 145.0)
        hp = self.work / "hirate.wav"
        hp.write_bytes(media.wav_bytes_pcm16(hi, 200000))
        cases.append(("rate 200k", self.request_body("req-lim2", hp, out, media.sha256_file(hp), hp.stat().st_size,
                                                     params={"lengthSeconds": 1}), "UNSUPPORTED_FORMAT"))
        cases.append(("beyond duration", self.request_body("req-lim3", mix, out, sha, nb,
                                                           params={"startSeconds": 7, "lengthSeconds": 8}),
                      "LIMIT_EXCEEDED"))
        for label, body, want in cases:
            code, docs = self.job(body)
            last = self.v.stream(docs, code)
            self.v.expect(last["event"] == "error" and last["error"]["code"] == want,
                          f"{label}: got {last.get('error', {}).get('code', last['event'])} want {want}")
            self.v.typed_error(last, want, None)
        return "601s->SCHEMA_INVALID; 200kHz->UNSUPPORTED_FORMAT; beyond duration->LIMIT_EXCEEDED"

    def s10(self):
        mix, sha, nb = self.fixture("schema.wav")
        out = self.outdir("schema-out")
        base = self.request_body("req-schema", mix, out, sha, nb)
        cases = []

        def mut(label, fn, code):
            b = json.loads(json.dumps(base))
            fn(b)
            b["requestId"] = "req-schema-" + "".join(c if c.isalnum() else "-" for c in label)
            cases.append((label, b, code))

        mut("missing outputDir", lambda b: b.pop("outputDir"), "SCHEMA_INVALID")
        mut("extra field", lambda b: b.__setitem__("unexpected", 1), "SCHEMA_INVALID")
        mut("schemaVersion 2.0", lambda b: b.__setitem__("schemaVersion", "2.0"), "UNSUPPORTED_SCHEMA")
        mut("operation remix", lambda b: b.__setitem__("operation", "remix"), "SCHEMA_INVALID")
        mut("ppq 480", lambda b: b["params"].__setitem__("ppq", 480), "SCHEMA_INVALID")
        mut("quantize 1/64", lambda b: b["params"].__setitem__("quantize", "1/64"), "SCHEMA_INVALID")
        # S1 fields that left the v1 with S1b (CONTRACT.md §4/§13): must be refused, not silently ignored
        mut("S1 cancelToken", lambda b: b.__setitem__("cancelToken", "tok"), "SCHEMA_INVALID")
        mut("S1 cancelFile", lambda b: b.__setitem__("cancelFile", str(self.work / "c")), "SCHEMA_INVALID")
        mut("S1 parentPid", lambda b: b.__setitem__("parentPid", 1234), "SCHEMA_INVALID")
        mut("S1 deadlineSeconds", lambda b: b.__setitem__("deadlineSeconds", 60), "SCHEMA_INVALID")
        for label, b, want in cases:
            code, docs = self.job(b)
            last = self.v.stream(docs, code)
            got = last.get("error", {}).get("code", last["event"])
            self.v.expect(got == want, f"{label}: got {got} want {want}")
            self.v.typed_error(last, want, None)
        for label, raw, want in (("empty stdin", "\n", "INVALID_JSON"), ("not JSON", "{nope\n", "INVALID_JSON"),
                                 ("JSON array", "[1]\n", "SCHEMA_INVALID")):
            code, docs = self.sim(["job"], raw)
            last = self.v.stream(docs, code)
            got = last.get("error", {}).get("code", last["event"])
            self.v.expect(got == want, f"{label}: got {got} want {want}")
            cases.append((label, None, want))
        return f"{len(cases)} invalid requests -> typed error + exit 2 (incl. 4 S1 fields now refused)"

    def s11(self):
        # PPQ 480 refused at both ends (CONTRACT.md §6): writer and reader
        refused_write = False
        try:
            media.midi_bytes(145.0, [(0, 120, 36, 100, 9)], ppq=480)
        except media.MediaError as e:
            refused_write = e.code == "UNSUPPORTED_FORMAT"
        self.v.expect(refused_write, "writer produced a ppq 480 file")
        good = media.midi_bytes(145.0, [(0, 120, 36, 100, 9)])
        p960 = self.work / "ok960.mid"
        p960.write_bytes(good)
        self.v.expect(media.read_midi(p960)["ppq"] == 960, "960 round trip failed")
        p480 = self.work / "bad480.mid"
        p480.write_bytes(good[:12] + (480).to_bytes(2, "big") + good[14:])
        refused_read = False
        try:
            media.read_midi(p480)
        except media.MediaError as e:
            refused_read = e.code == "UNSUPPORTED_FORMAT"
        self.v.expect(refused_read, "reader imported a ppq 480 file")
        self.v.expect(media.grid_ticks("1/32") == 120 and media.grid_ticks("1/16") == 240, "grid ticks wrong")
        return "writer refuses 480; reader refuses 480 (UNSUPPORTED_FORMAT); 960 round-trips; 1/32=120, 1/16=240"

    def s12(self):
        mix, sha, nb = self.fixture("fake.wav")
        out = self.outdir("fake-out")
        code, docs = self.job(self.request_body("req-fake", mix, out, sha, nb), fault="fake-success")
        if not docs or docs[-1]["event"] != "done":
            return False, f"sim did not fake success — canary not exercised"
        detected = self.v.rejects(lambda: self.v.files(docs[-1]["result"], out))
        return detected, "sim claimed done without .mid on disk; file/hash checks rejected it"

    def s13(self):
        mix, sha, nb = self.fixture("wrongsha.wav")
        out = self.outdir("wrongsha-out")
        code, docs = self.job(self.request_body("req-wrongsha", mix, out, sha, nb), fault="wrong-sha")
        if not docs or docs[-1]["event"] != "done":
            return False, "sim did not report done — canary not exercised"
        detected = self.v.rejects(lambda: self.v.files(docs[-1]["result"], out))
        return detected, ".mid on disk but reported sha256 is wrong; sha check rejected it"

    def s14(self):
        # the reference client (contracts/sim/client.py) is EXECUTED against the simulator: with a /2 hello it
        # must not start a job; with the right hello it submits and gets the .mid (audit 2026-11-16: S14 used to
        # imitate the client's rule inside the gate)
        def argv(fault):
            return lambda sub: [PY, str(SIM_DIR / "service_sim.py"), sub] + (["--fault", fault] if fault else [])

        code, docs = self.sim(["hello"], fault="bad-contract")
        self.v.expect(docs and docs[0].get("contract") == "aicore.service/2", "fault not active")
        self.v.expect(self.v.rejects(lambda: self.v.doc("service-hello", docs[0])),
                      "schema accepted hello with contract /2")
        mix, sha, nb = self.fixture("client.wav")
        bad_out = self.outdir("client-bad")
        r = client.submit(argv("bad-contract"), self.request_body("req-client-bad", mix, bad_out, sha, nb))
        self.v.expect(not r["submitted"], f"client submitted to a wrong contract major: {r}")
        self.v.expect(not any(bad_out.iterdir()), "a refused submission wrote into outputDir")
        good_out = self.outdir("client-good")
        r2 = client.submit(argv(""), self.request_body("req-client-good", mix, good_out, sha, nb,
                                                       params={"includeStems": False}))
        self.v.expect(r2["submitted"], f"client refused a good service: {r2['reason']}")
        last = self.v.stream(r2["docs"], r2["exit"])
        self.v.expect(last["event"] == "done", f"client job ended {last['event']}")
        self.v.files(last["result"], good_out)
        return f"client vs /2 hello -> not submitted ({r['reason']}); vs /1 -> submitted, done"

    def s15(self):
        mix, sha, nb = self.fixture("det.wav")
        shas = []
        for i in (1, 2):
            out = self.outdir(f"det-out{i}")
            code, docs = self.job(self.request_body(f"req-det{i}", mix, out, sha, nb, params={"includeStems": False}))
            last = self.v.stream(docs, code)
            self.v.expect(last["event"] == "done", f"run {i} {last['event']}")
            self.v.files(last["result"], out)
            shas.append(next(o for o in last["result"]["outputs"] if o["role"] == "midi")["sha256"])
        self.v.expect(shas[0] == shas[1], "same input + params gave different .mid bytes")
        mix2, sha2, nb2 = self.fixture("det150.wav", bpm=150.0)
        out = self.outdir("det-out3")
        code, docs = self.job(self.request_body("req-det3", mix2, out, sha2, nb2,
                                                params={"bpm": 150.0, "includeStems": False}))
        last = self.v.stream(docs, code)
        sha3 = next(o for o in last["result"]["outputs"] if o["role"] == "midi")["sha256"]
        self.v.expect(sha3 != shas[0], "different input produced identical .mid hash")
        return f"same request twice -> same sha {shas[0][:16]}; 150 BPM input -> {sha3[:16]}"

    def s16(self):
        self.v.manifest(CONTRACTS)
        scratch = self.work / "contracts-copy"
        shutil.copytree(CONTRACTS, scratch, ignore=shutil.ignore_patterns("__pycache__", "evidence",
                                                                          ".staging-manifest"))
        # each tamper on its own copy, so one cannot hide behind another (audit 2026-11-16: cli.md was uncovered)
        def append_space(rel):
            return lambda d: (d / rel).write_bytes((d / rel).read_bytes() + b" ")

        tampers = [("schema byte", append_space("schemas/v1/midi-params.schema.json")),
                   ("deleted example", lambda d: (d / "examples" / "hello.response.json").unlink()),
                   ("cli.md byte", append_space("cli.md")),
                   ("CONTRACT.md byte", append_space("CONTRACT.md"))]
        caught = []
        for i, (label, fn) in enumerate(tampers):
            copy = self.work / f"contracts-copy-{i}"
            shutil.copytree(scratch, copy)
            fn(copy)
            if self.v.rejects(lambda c=copy: self.v.manifest(c)):
                caught.append(label)
        # counts are checked too: a manifest whose counts lie is red even when every file hash matches
        lie = self.work / "contracts-copy-counts"
        shutil.copytree(scratch, lie)
        mdoc = json.loads((lie / "manifest.json").read_text(encoding="utf-8"))
        mdoc["counts"]["examples"] += 1
        (lie / "manifest.json").write_text(json.dumps(mdoc), encoding="utf-8")
        if self.v.rejects(lambda: self.v.manifest(lie)):
            caught.append("wrong counts")
        detected = len(caught) == len(tampers) + 1
        return detected, f"real manifest OK; caught {len(caught)}/{len(tampers) + 1}: {caught}"

    def s17(self):
        example = json.loads((EXAMPLES / "midi-task.request.json").read_text(encoding="utf-8"))
        self.v.doc("service-task-request", example)
        muts = []
        m = json.loads(json.dumps(example)); del m["requestId"]; muts.append(("drop required requestId", m))
        m = json.loads(json.dumps(example)); m["params"] = []; muts.append(("params wrong type", m))
        m = json.loads(json.dumps(example)); m["extra"] = 1; muts.append(("additional property", m))
        m = json.loads(json.dumps(example)); m["params"]["ppq"] = 480; muts.append(("const ppq 480", m))
        m = json.loads(json.dumps(example)); m["params"]["quantize"] = "1/64"; muts.append(("enum quantize 1/64", m))
        m = json.loads(json.dumps(example)); m["requestId"] = "bad id!"; muts.append(("pattern requestId", m))
        m = json.loads(json.dumps(example)); m["params"]["kickNote"] = 200; muts.append(("range kickNote", m))
        m = json.loads(json.dumps(example)); m["params"]["seed"] = 2 ** 32; muts.append(("seed over 32 bits", m))
        m = json.loads(json.dumps(example)); m["schemaVersion"] = "2.0"; muts.append(("schemaVersion 2.0", m))
        m = json.loads(json.dumps(example)); m["inputs"] = []; muts.append(("minItems inputs", m))
        m = json.loads(json.dumps(example)); m["inputs"][0]["role"] = "vocals"; muts.append(("contains mix", m))
        m = json.loads(json.dumps(example)); m["outputDir"] = "relative/path"; muts.append(("absolutePath", m))
        m = json.loads(json.dumps(example)); m["params"]["bpm"] = True; muts.append(("bool is not number", m))
        m = json.loads(json.dumps(example)); m["cancelToken"] = "t"; muts.append(("deferred cancelToken", m))
        m = json.loads(json.dumps(example)); m["params"]["bpm"] = float("nan"); muts.append(("NaN is not a number", m))
        m = json.loads(json.dumps(example)); m["params"]["bpm"] = float("inf"); muts.append(("Infinity is not a number", m))
        m = json.loads(json.dumps(example)); m["params"]["kickNote"] = 36.5; muts.append(("36.5 is not an integer", m))
        # and the other direction: draft 2020-12 counts 36.0 as an integer (a validator refusing it is wrong too)
        m = json.loads(json.dumps(example)); m["params"]["kickNote"] = 36.0
        self.v.doc("service-task-request", m)
        detections = 0
        for label, m in muts:
            ok = self.v.rejects(lambda m=m: self.v.doc("service-task-request", m))
            detections += 1 if ok else 0
            self.counts["mutations"] += 1
        self.counts["mutationDetections"] += detections
        bad_dir = self.work / "bad-schemas"
        bad_dir.mkdir(exist_ok=True)
        (bad_dir / "rogue.schema.json").write_text(json.dumps(
            {"$schema": "https://json-schema.org/draft/2020-12/schema",
             "$id": "https://aicore.local/schemas/v1/rogue.schema.json", "banana": True}), encoding="utf-8")
        fail_closed = False
        try:
            validation_errors(Registry(bad_dir), "rogue", {})
        except UnsupportedKeyword:
            fail_closed = True
        self.v.expect(fail_closed, "unknown keyword silently ignored (fake green risk)")
        detected = detections == len(muts) and fail_closed
        return detected, f"{detections}/{len(muts)} mutations rejected; unknown keyword raises (fail-closed)"

    EXAMPLE_SCHEMAS = {"hello.response.json": "service-hello",
                       "midi-task.request.json": "service-task-request",
                       "midi-task.roll32.request.json": "service-task-request",
                       "render.request.json": "service-task-request",
                       "render-audio.request.json": "service-task-request",
                       "error.response.json": "error",
                       "task-manifest.json": "task-manifest",
                       "jsonl-error.json": "service-event",
                       "lora-dataset.request.json": "service-task-request",
                       "lora-train.request.json": "service-task-request"}
    EXAMPLE_STREAMS = {"jsonl-transcript.jsonl": "done", "midi-task.failed.jsonl": "error",
                       "lora-train.failed.jsonl": "error"}

    def s18(self):
        files = sorted(p.name for p in EXAMPLES.iterdir() if p.is_file() and p.name != "README.md")
        unmapped = [f for f in files if f not in self.EXAMPLE_SCHEMAS and f not in self.EXAMPLE_STREAMS]
        self.v.expect(not unmapped, f"examples without a schema mapping: {unmapped}")
        n = 0
        for fname, schema in self.EXAMPLE_SCHEMAS.items():
            doc = json.loads((EXAMPLES / fname).read_text(encoding="utf-8"))
            self.v.doc(schema, doc)
            n += 1
        for fname, terminal in self.EXAMPLE_STREAMS.items():
            docs = [json.loads(ln) for ln in (EXAMPLES / fname).read_text(encoding="utf-8").splitlines()
                    if ln.strip()]
            last = self.v.stream(docs, 0 if terminal == "done" else 2)
            self.v.expect(last["event"] == terminal, f"{fname} ends with {last['event']} want {terminal}")
            n += len(docs)
        req = json.loads((EXAMPLES / "midi-task.roll32.request.json").read_text(encoding="utf-8"))
        self.v.expect(req["params"]["quantize"] == "1/32" and req["params"]["ppq"] == 960, "roll example not 1/32@960")
        return f"{n} synthetic example documents validated ({len(files)} files, 0 unmapped)"

    DEFERRED = ["T1 — HTTP loopback", "Idempotency", "Reconciliation", "Heartbeat", "Cancellation",
                "Parent-pid", "VERSION_CONFLICT", "deadlineSeconds", "BUSY", "UNAUTHENTICATED", "TRANSPORT_UNKNOWN"]

    def s19(self):
        text = (CONTRACTS / "CONTRACT.md").read_text(encoding="utf-8")
        self.v.expect("## 13. Deferred" in text, "CONTRACT.md lost §13 (deferred list)")
        sec = text.split("## 13. Deferred", 1)[-1]
        missing = [k for k in self.DEFERRED if k not in sec]
        self.v.expect(not missing, f"deferred items not documented in §13: {missing}")
        return f"{len(self.DEFERRED)} deferred items documented in CONTRACT.md §13 (deferred, not deleted)"

    def s20(self):
        # profile render: exactly one input, either midi + an instrument or audio + effects only (audit P1).
        midi = json.loads((EXAMPLES / "render.request.json").read_text(encoding="utf-8"))
        audio = json.loads((EXAMPLES / "render-audio.request.json").read_text(encoding="utf-8"))
        accepted = []
        for label, d in (("midi + plugin", midi), ("audio + effectRoles", audio)):
            self.v.doc("service-task-request", d)
            accepted.append(label)
        for label, fn in (("midi + role", lambda m: (m["params"].pop("plugin"), m["params"].__setitem__("role", "bass"))),
                          ("midi + engine sampler", lambda m: (m["params"].pop("plugin"), m["params"].update(
                              engine="sampler", kit=[{"note": 36, "sample": "D:/k/kick.wav"}]))),
                          ("midi + engine sfizz", lambda m: (m["params"].pop("plugin"), m["params"].update(
                              engine="sfizz", sfz="D:/k/drums.sfz"))),
                          ("midi + engine vita", lambda m: (m["params"].pop("plugin"), m["params"].__setitem__("engine", "vita")))):
            m = json.loads(json.dumps(midi))
            fn(m)
            self.v.doc("service-task-request", m)
            accepted.append(label)
        a = json.loads(json.dumps(audio))
        a["params"].pop("effectRoles")
        a["params"]["effects"] = [{"plugin": "C:/Program Files/Common Files/VST3/fx.vst3"}]
        self.v.doc("service-task-request", a)
        accepted.append("audio + effects")
        muts = []

        def mut(label, base, fn):
            m = json.loads(json.dumps(base))
            fn(m)
            muts.append((label, m))

        mut("audio + plugin", audio, lambda m: m["params"].__setitem__("plugin", "C:/x/Synth.vst3"))
        mut("audio + role", audio, lambda m: m["params"].__setitem__("role", "bass"))
        mut("audio + engine sampler", audio, lambda m: m["params"].__setitem__("engine", "sampler"))
        mut("audio without effects", audio, lambda m: m["params"].pop("effectRoles"))
        mut("audio + empty effects", audio, lambda m: (m["params"].pop("effectRoles"), m["params"].__setitem__("effects", [])))
        mut("midi without instrument", midi, lambda m: m["params"].pop("plugin"))
        mut("midi + plugin + role", midi, lambda m: m["params"].__setitem__("role", "bass"))
        mut("two inputs", midi, lambda m: m["inputs"].append(audio["inputs"][0]))
        mut("input role mix", midi, lambda m: m["inputs"][0].__setitem__("role", "mix"))
        mut("engine sampler without kit", midi, lambda m: (m["params"].pop("plugin"), m["params"].__setitem__("engine", "sampler")))
        mut("kit without engine sampler", midi, lambda m: m["params"].__setitem__("kit", [{"note": 36, "sample": "D:/k/a.wav"}]))
        mut("sfz without engine sfizz", midi, lambda m: m["params"].__setitem__("sfz", "D:/k/a.sfz"))
        detections = 0
        for label, m in muts:
            ok = self.v.rejects(lambda m=m: self.v.doc("service-task-request", m))
            detections += 1 if ok else 0
            self.counts["mutations"] += 1
        self.counts["mutationDetections"] += detections
        return (detections == len(muts),
                f"{len(accepted)} valid render shapes accepted; {detections}/{len(muts)} invalid shapes rejected")

    def s21(self):
        # the done line carries the manifest's sha256; a manifest edited after done (still valid JSON, same
        # output hashes) must be caught (audit 2026-11-16: the result gave no verifiable manifest hash)
        mix, sha, nb = self.fixture("mtamper.wav")
        out = self.outdir("mtamper-out")
        code, docs = self.job(self.request_body("req-mtamper", mix, out, sha, nb, params={"includeStems": False}))
        if not docs or docs[-1]["event"] != "done":
            return False, "sim did not report done, canary not exercised"
        result = docs[-1]["result"]
        self.v.files(result, out)
        mp = Path(result["manifestPath"])
        mp.write_bytes(mp.read_bytes().replace(b'"validation": "unreviewed"', b'"validation":  "unreviewed"'))
        detected = self.v.rejects(lambda: self.v.files(result, out))
        return detected, "manifest.json re-spaced after done (same JSON, same output hashes) -> caught by manifestSha256"

    def _wav_padded(self, name, total_bytes):
        """A valid PCM16 mono WAV of exactly total_bytes: 8 s of the fixture, then silence."""
        mix, _ = media.synth_mix(8.0, 44100, 145.0)
        head = media.wav_bytes_pcm16(mix, 44100)
        payload = head[44:] + b"\0" * (total_bytes - len(head))
        data = (b"RIFF" + (36 + len(payload)).to_bytes(4, "little") + b"WAVE" + head[12:40]
                + len(payload).to_bytes(4, "little") + payload)
        p = self.work / name
        p.write_bytes(data)
        return p, media.sha256_file(p), len(data)

    def s22(self):
        # caps and output integrity (audit 2026-11-16 P2)
        mf = json.loads((SCHEMA_DIR / "media-format.schema.json").read_text(encoding="utf-8"))["$defs"]
        wav_cap, midi_cap = mf["wavFormat"]["properties"]["bytes"]["maximum"], mf["midiFormat"]["properties"]["bytes"]["maximum"]
        code, docs = self.sim(["hello"])
        lim = docs[0]["limits"]
        self.v.expect(lim["maxInputBytes"] == wav_cap == media.MAX_WAV_BYTES,
                      f"WAV cap drift: hello {lim['maxInputBytes']}, schema {wav_cap}, media {media.MAX_WAV_BYTES}")
        self.v.expect(media.MAX_MIDI_BYTES == midi_cap, f"MIDI cap drift: media {media.MAX_MIDI_BYTES} schema {midi_cap}")
        self.v.expect(lim["maxResponseBytes"] == media.MAX_LINE_BYTES == 2 * 1024 * 1024, "line cap drift")
        # a 9 MiB WAV is inside the contract: the old 8 MiB simulator cap refused it
        big, bsha, bnb = self._wav_padded("nine-mib.wav", 9 * 1024 * 1024)
        out = self.outdir("nine-mib-out")
        code, docs = self.job(self.request_body("req-9mib", big, out, bsha, bnb,
                                                params={"lengthSeconds": 8, "includeStems": False}))
        last = self.v.stream(docs, code)
        self.v.expect(last["event"] == "done", f"9 MiB WAV refused: {last.get('error')}")
        self.v.files(last["result"], out)
        # reusing an outputDir that holds a published job: refused, the earlier files untouched
        before = {p.name: media.sha256_file(p) for p in out.iterdir() if p.is_file()}
        code, docs = self.job(self.request_body("req-reuse", big, out, bsha, bnb,
                                                params={"lengthSeconds": 8, "includeStems": False}))
        last = self.v.stream(docs, code)
        self.v.typed_error(last, "OUTPUT_EXISTS", None)
        after = {p.name: media.sha256_file(p) for p in out.iterdir() if p.is_file()}
        self.v.expect(before == after and before, "a refused job changed the earlier job's files")
        # a failure while publishing leaves no final file behind
        mix, sha, nb = self.fixture("late.wav")
        lout = self.outdir("late-out")
        code, docs = self.job(self.request_body("req-late", mix, lout, sha, nb), fault="late-fail")
        last = self.v.stream(docs, code)
        self.v.typed_error(last, "BACKEND_FAILED", lout)
        left = sorted(p.name for p in lout.rglob("*") if p.is_file())
        self.v.expect(not left, f"a failed job left files in outputDir: {left}")
        return (f"caps WAV {wav_cap} / MIDI {midi_cap} / line {media.MAX_LINE_BYTES} agree; 9 MiB WAV done; "
                f"reused outputDir -> OUTPUT_EXISTS, earlier files intact; late failure -> nothing published")

    def s23(self):
        # parsing and request validation (audit 2026-11-16 P2)
        mix, sha, nb = self.fixture("parse.wav")
        cases = []
        # another input with a wrong declared sha256 (only the mix used to be hashed)
        extra = self.work / "extra.bin"
        extra.write_bytes(b"not the declared bytes")
        out = self.outdir("parse-extra")
        b = self.request_body("req-extra", mix, out, sha, nb)
        b["inputs"].append({"role": "reference", "path": str(extra), "sha256": "0" * 64})
        cases.append(("extra input wrong sha", b, None, "HASH_MISMATCH"))
        # a data chunk that claims more bytes than the file holds
        data = mix.read_bytes()
        trunc = self.work / "truncated.wav"
        trunc.write_bytes(data[:-1000])
        cases.append(("truncated data chunk", self.request_body("req-trunc", trunc, self.outdir("parse-trunc"),
                                                                 media.sha256_file(trunc), len(data) - 1000),
                      None, "UNSUPPORTED_FORMAT"))
        # an invalid requestId must not be echoed into the error line (that line would break the schema)
        cases.append(("invalid requestId", self.request_body("bad id!", mix, self.outdir("parse-id"), sha, nb),
                      None, "SCHEMA_INVALID"))
        # NaN is not JSON
        nan = json.dumps(self.request_body("req-nan", mix, self.outdir("parse-nan"), sha, nb)).replace(
            '"bpm": 145.0', '"bpm": NaN')
        cases.append(("bpm NaN", None, nan + "\n", "INVALID_JSON"))
        # a request line over 2 MiB
        huge = self.request_body("req-huge", mix, self.outdir("parse-huge"), sha, nb)
        huge["params"]["pad"] = "x" * (2 * 1024 * 1024)
        cases.append(("line over 2 MiB", None, json.dumps(huge) + "\n", "LIMIT_EXCEEDED"))
        for label, body, raw, want in cases:
            code, docs = self.job(body) if raw is None else self.sim(["job"], raw)
            last = self.v.stream(docs, code)  # validates every line, the error line included
            got = last.get("error", {}).get("code", last["event"])
            self.v.expect(got == want, f"{label}: got {got} want {want}")
        # 36.0 is an integer for JSON Schema: accepted, and the job runs with it
        out = self.outdir("parse-int")
        ok = json.dumps(self.request_body("req-int", mix, out, sha, nb, params={"includeStems": False})).replace(
            '"kickNote": 36', '"kickNote": 36.0')
        code, docs = self.sim(["job"], ok + "\n")
        last = self.v.stream(docs, code)
        self.v.expect(last["event"] == "done", f"kickNote 36.0 refused: {last.get('error')}")
        self.v.files(last["result"], out)
        return f"{len(cases)} malformed requests -> typed errors with valid lines; kickNote 36.0 -> done"

    # ---- run ----

    def run(self):
        print(f"S1b gate — contract {CONTRACT} — python {sys.version.split()[0]} — "
              f"mode={'NOOP-CONTROL' if self.noop else 'FULL'}")
        print(f"workdir: {self.work}")
        self.scenario("S01", "hello: ppq 960, grid to 1/32, no auth", self.s01)
        self.scenario("S02", "happy path: request -> progress -> .mid PPQ 960 + manifest", self.s02)
        self.scenario("S03", "happy path: request -> progress -> typed error BACKEND_FAILED", self.s03)
        self.scenario("S04", "happy path: request -> progress -> typed error OUT_OF_MEMORY", self.s04)
        self.scenario("S05", "1/64 roll quantized at 1/32 loses nothing", self.s05)
        self.scenario("S06", "notes shorter than the grid stay short", self.s06)
        self.scenario("S07", "missing reference -> red typed error", self.s07, kind="failure-path")
        self.scenario("S08", "hash mismatch -> red typed error", self.s08, kind="failure-path")
        self.scenario("S09", "limits/format -> red typed errors", self.s09, kind="failure-path")
        self.scenario("S10", "schema invalid / S1 fields / bad stdin -> typed", self.s10, kind="failure-path")
        self.scenario("S11", "PPQ 480 refused by writer and reader", self.s11)
        self.canary("S12", "fake-success sabotage must be caught", self.s12)
        self.canary("S13", "wrong-sha sabotage must be caught", self.s13)
        self.scenario("S14", "wrong contract major refused before submit", self.s14)
        self.scenario("S15", "determinism + content moves hash", self.s15)
        self.canary("S16", "manifest tamper/missing must be caught", self.s16)
        self.canary("S17", "validator mutations must be rejected", self.s17)
        self.scenario("S18", "all synthetic examples validate (none unmapped)", self.s18)
        self.scenario("S19", "deferred items documented, not deleted", self.s19)
        self.canary("S20", "render: midi+instrument XOR audio+effects (valid shapes pass, mixes rejected)", self.s20)
        self.canary("S21", "manifest edited after done must be caught (manifestSha256)", self.s21)
        self.scenario("S22", "caps agree; 9 MiB WAV ok; reused outputDir refused; late failure publishes nothing", self.s22, kind="failure-path")
        self.scenario("S23", "extra-input hash, truncated chunk, bad id, NaN, 2 MiB line -> typed; 36.0 is an integer", self.s23, kind="failure-path")
        passed = sum(1 for s in self.scenarios if s["verdict"] == "pass")
        failed = len(self.scenarios) - passed
        canaries = [s for s in self.scenarios if s["kind"] == "canary"]
        cdet = sum(1 for s in canaries if s["verdict"] == "pass")
        report = {"contract": CONTRACT, "slice": "S1b", "mode": "noop-control" if self.noop else "full",
                  "startedAt": now(), "python": sys.version.split()[0], "scenarios": self.scenarios,
                  "counts": {"scenarios": len(self.scenarios), "passed": passed, "failed": failed,
                             "canaries": len(canaries), "canariesDetected": cdet,
                             "mutations": self.counts["mutations"],
                             "mutationDetections": self.counts["mutationDetections"],
                             "simProcesses": self.counts["jobs"], "transcriptLines": len(self.transcripts)},
                  "hashes": self.hashes, "transcripts": self.transcripts}
        rp = self.work / "gate-report.json"
        rp.write_text(json.dumps(report, ensure_ascii=True, indent=2), encoding="utf-8")
        print("-" * 72)
        print(f"scenarios {passed}/{len(self.scenarios)} pass | canaries detected {cdet}/{len(canaries)} | "
              f"mutations {self.counts['mutationDetections']}/{self.counts['mutations']} | "
              f"sim processes {self.counts['jobs']} | transcript lines {len(self.transcripts)}")
        print(f"report: {rp}")
        if self.noop:
            print("NOOP CONTROL: checks were no-ops; canaries NOT detected -> gate RED (it can fail). "
                  "This exit 1 is the REQUIRED proof, not a regression.")
        if not self.keep:
            shutil.rmtree(self.work, ignore_errors=True)
            print("workdir cleaned")
        else:
            print(f"workdir kept: {self.work}")
        return 0 if failed == 0 else 1


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description="S1b contract gate")
    ap.add_argument("--noop", action="store_true", help="no-op control: checks disabled, must exit RED")
    ap.add_argument("--keep", action="store_true", help="keep temp workdir")
    args = ap.parse_args(argv)
    return Gate(noop=args.noop, keep=args.keep).run()


if __name__ == "__main__":
    sys.exit(main())
