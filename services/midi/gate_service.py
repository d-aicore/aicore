"""S2 contract gate: the REAL audio->MIDI service against `aicore.service/1` (S1b).

Uses the same load-bearing checks as the contract gate (contracts/sim/gate.py `Verifier`): every stdout
line validated, exactly one terminal line, exit code matches it, outputs exist and match their sha256,
the .mid reads back at PPQ 960, manifest validated, staging cleaned, errors typed.

  python gate_service.py           # exit 0 green
  python gate_service.py --noop    # checks become no-ops -> canaries undetected -> exit 1 (required proof)

Runs Demucs on CPU at BELOW_NORMAL priority (a 6 s window of a Core template render, ~10 s per job).
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO / "contracts" / "sim"))
import media
from gate import CheckFailed, NoopVerifier, Verifier
from jsonschema_mini import Registry

SERVICE = HERE / "aicore_midi_service.py"
RENDER = Path(os.environ.get("AICORE_S2_GATE_WAV", r"D:\AI Dev\flamingo-teste\templates\hitech-t1-v1.wav"))
BELOW_NORMAL = 0x00004000


class ServiceVerifier(Verifier):
    """Verifier.files minus the simulator-only rule (`synthetic: true`): a real job must say false."""

    def files(self, result, output_dir):
        out = Path(output_dir)
        for o in result["outputs"]:
            p = Path(o["path"])
            self.expect(p.is_file(), f"output missing: {p}")
            self.expect(p.stat().st_size == o["bytes"], f"bytes moved: {p}")
            self.expect(media.sha256_file(p) == o["sha256"], f"sha256 moved: {p}")
            if o["format"]["container"] == "midi":
                info = media.read_midi(p)
                self.expect(info["ppq"] == 960 and o["format"]["ppq"] == 960, "PPQ != 960")
        mp = Path(result["manifestPath"])
        self.expect(mp.is_file(), f"manifest missing: {mp}")
        manifest = json.loads(mp.read_text(encoding="utf-8"))
        self.doc("task-manifest", manifest)
        self.expect(manifest["validation"] == "unreviewed", "service claimed approval")
        self.expect(manifest["synthetic"] is False, "real service output marked synthetic")
        by_path = {o["path"]: o for o in manifest["outputs"]}
        for o in result["outputs"]:
            self.expect(o["path"] in by_path and by_path[o["path"]]["sha256"] == o["sha256"],
                        f"output not in manifest / hash differs: {o['path']}")
        self.expect(not list(out.rglob("*.part")), "staging .part left behind")
        self.expect(not (out / ".staging").exists(), ".staging not cleaned")


class Gate:
    def __init__(self, noop=False, keep=False):
        self.reg = Registry(REPO / "contracts" / "schemas" / "v1")
        self.v = NoopVerifier() if noop else ServiceVerifier(self.reg)
        self.noop, self.keep = noop, keep
        self.work = Path(tempfile.mkdtemp(prefix="aicore-s2-gate-"))
        self.rows, self.lines = [], []
        self.current = ""

    def svc(self, args, stdin_text=None, env_extra=None):
        env = dict(os.environ, **(env_extra or {}))
        p = subprocess.run([sys.executable, str(SERVICE)] + args, input=stdin_text, capture_output=True,
                           text=True, encoding="utf-8", errors="replace", env=env, timeout=900,
                           creationflags=BELOW_NORMAL if os.name == "nt" else 0)
        docs = []
        for ln in p.stdout.splitlines():
            if not ln.strip():
                continue
            self.lines.append({"scenario": self.current, "line": ln})
            try:
                docs.append(json.loads(ln))
            except ValueError:
                raise CheckFailed(f"non-JSON stdout line (contract broken): {ln[:120]!r}")
        return p.returncode, docs

    def body(self, rid, out, **over):
        params = {"ppq": 960, "quantize": "1/32", "parts": ["drums", "bass", "other"], "kickNote": 36,
                  "bassMono": True, "bpm": 210.0, "bpmSource": "provided", "leadFilter": True,
                  "splitOnOnsets": True, "includeStems": True, "startSeconds": 61.7, "lengthSeconds": 6,
                  "seed": 42}
        params.update(over.pop("params", {}))
        b = {"schemaVersion": "1.0", "requestId": rid, "profile": "midi", "operation": "transcribe",
             "params": params, "inputs": [{"role": "mix", "path": str(RENDER)}], "outputDir": str(out)}
        b.update(over)
        return b

    def out(self, name):
        d = self.work / name
        d.mkdir(parents=True, exist_ok=True)
        return d

    def job(self, body, env_extra=None):
        return self.svc(["job"], json.dumps(body) + "\n", env_extra)

    def scenario(self, sid, name, fn, kind="positive"):
        self.current = sid
        t0 = time.monotonic()
        try:
            detail, verdict = fn() or "", "pass"
        except (CheckFailed, media.MediaError) as e:
            detail, verdict = f"{type(e).__name__}: {e}", "fail"
        except Exception as e:
            detail, verdict = f"UNEXPECTED {type(e).__name__}: {e}", "fail"
        if kind == "canary" and verdict == "pass":
            detected, detail = detail
            verdict = "pass" if detected else "fail"
        self.rows.append({"id": sid, "name": name, "kind": kind, "verdict": verdict, "detail": str(detail)[:600],
                          "seconds": round(time.monotonic() - t0, 2)})
        print(f"[{'OK ' if verdict == 'pass' else 'RED'}] {sid} {name} ({kind}) - {str(detail)[:220]}")

    # ---- scenarios ----

    def s01(self):
        code, docs = self.svc(["hello"])
        self.v.expect(code == 0 and len(docs) == 1, f"hello exit {code} lines {len(docs)}")
        self.v.doc("service-hello", docs[0])
        self.v.expect(docs[0]["ppq"] == 960 and "1/32" in docs[0]["quantizeGrid"], "hello ppq/grid")
        return f"{docs[0]['service']['name']} {docs[0]['service']['version']} ppq 960 grid to 1/32"

    def s02(self):
        out = self.out("happy")
        code, docs = self.job(self.body("s2-gate-happy", out))
        last = self.v.stream(docs, code)
        self.v.expect(last["event"] == "done", f"terminal {last['event']}: {last.get('error')}")
        phases = {d["phase"] for d in docs if d["event"] == "progress"}
        self.v.expect(phases == {"read", "separate", "transcribe", "quantize", "write"}, f"phases {sorted(phases)}")
        self.v.files(last["result"], out)
        mid = next(o for o in last["result"]["outputs"] if o["role"] == "midi")
        info = media.read_midi(Path(mid["path"]), with_notes=True)
        starts = [n["start"] for n in info["noteEvents"]]
        self.v.expect(all(s % 120 == 0 for s in starts), "note starts off the 1/32 grid (120 ticks)")
        durs = [n["dur"] for n in info["noteEvents"]]
        off_grid = sum(1 for d in durs if d % 120)
        self.v.expect(off_grid > 0, "every length is a whole grid step: lengths are not coming from the audio")
        roles = sorted(o["role"] for o in last["result"]["outputs"])
        self.happy_sha = mid["sha256"]
        self.happy_mid = Path(mid["path"])
        self.happy_result, self.happy_out = last["result"], out
        return (f"exit 0, {len(docs)} lines, outputs {roles}, {len(starts)} notes all on 120-tick grid, "
                f"{off_grid} lengths not multiple of 120 (measured), sha {mid['sha256'][:16]}")

    def s03(self):
        # typed error on the happy path: request -> progress -> error (Demucs env missing)
        out = self.out("backend")
        code, docs = self.job(self.body("s2-gate-backend", out),
                              {"AICORE_STEMS_PY": str(self.work / "no-such-python.exe")})
        last = self.v.stream(docs, code)
        n = sum(1 for d in docs if d["event"] == "progress")
        self.v.expect(docs[0]["event"] == "start" and n >= 1, "error did not come after start+progress")
        self.v.typed_error(last, "BACKEND_FAILED", out)
        self.v.expect(not any(out.iterdir()), f"failed job left files: {[p.name for p in out.iterdir()]}")
        return f"start + {n} progress -> error BACKEND_FAILED, exit {code}, outputDir empty"

    def s04(self):
        out = self.out("errors")
        cases = [
            ("missing input", self.body("s2-gate-e1", out, inputs=[{"role": "mix", "path": str(self.work / "nope.wav")}]), "BROKEN_REFERENCE"),
            ("hash mismatch", self.body("s2-gate-e2", out, inputs=[{"role": "mix", "path": str(RENDER), "sha256": "0" * 64}]), "HASH_MISMATCH"),
            ("ppq 480", self.body("s2-gate-e3", out, params={"ppq": 480}), "SCHEMA_INVALID"),
            ("grid 1/64", self.body("s2-gate-e4", out, params={"quantize": "1/64"}), "SCHEMA_INVALID"),
            ("S1 cancelToken", self.body("s2-gate-e5", out, cancelToken="x"), "SCHEMA_INVALID"),
            ("S1 parentPid", self.body("s2-gate-e6", out, parentPid=1), "SCHEMA_INVALID"),
            ("outputDir missing", self.body("s2-gate-e7", self.work / "no-dir"), "BROKEN_REFERENCE"),
        ]
        notwav = self.work / "not-a.wav"
        notwav.write_bytes(b"this is not audio" * 10)
        cases.append(("not a WAV", self.body("s2-gate-e8", out, inputs=[{"role": "mix", "path": str(notwav)}]), "UNSUPPORTED_FORMAT"))
        for label, b, want in cases:
            code, docs = self.job(b)
            last = self.v.stream(docs, code)
            got = last.get("error", {}).get("code", last["event"])
            self.v.expect(got == want, f"{label}: got {got} want {want}")
            self.v.typed_error(last, want, None)
        for label, raw in (("empty stdin", "\n"), ("not JSON", "{x\n")):
            code, docs = self.svc(["job"], raw)
            last = self.v.stream(docs, code)
            self.v.expect(last.get("error", {}).get("code") == "INVALID_JSON", f"{label}: {last}")
        return f"{len(cases) + 2} bad requests -> typed error + exit 2"

    def s05(self):
        out = self.out("again")
        code, docs = self.job(self.body("s2-gate-again", out))
        last = self.v.stream(docs, code)
        mid = next(o for o in last["result"]["outputs"] if o["role"] == "midi")
        self.v.expect(mid["sha256"] == self.happy_sha, f"same request, different .mid: {mid['sha256'][:16]} vs {self.happy_sha[:16]}")
        out2 = self.out("grid16")
        code, docs = self.job(self.body("s2-gate-g16", out2, params={"quantize": "1/16", "includeStems": False}))
        last = self.v.stream(docs, code)
        mid16 = next(o for o in last["result"]["outputs"] if o["role"] == "midi")
        self.v.expect(mid16["sha256"] != self.happy_sha, "grid 1/16 and 1/32 gave the same .mid")
        info = media.read_midi(Path(mid16["path"]), with_notes=True)
        self.v.expect(all(n["start"] % 240 == 0 for n in info["noteEvents"]), "1/16 starts off the 240-tick grid")
        return f"same request -> same sha {mid['sha256'][:16]}; 1/16 -> {mid16['sha256'][:16]} on 240-tick grid"

    def s06(self):
        # canary: a published .mid whose bytes no longer match the reported sha256 must be rejected
        bad = self.work / "tampered"
        shutil.copytree(self.happy_out, bad)
        res = json.loads(json.dumps(self.happy_result).replace(str(self.happy_out).replace("\\", "\\\\"),
                                                               str(bad).replace("\\", "\\\\")))
        p = bad / "transcription.mid"
        data = bytearray(p.read_bytes())
        data[-5] ^= 0x01
        p.write_bytes(bytes(data))
        return self.v.rejects(lambda: self.v.files(res, bad)), "flipped one byte of the published .mid"

    def s07(self):
        # canary: a stdout line that is not a contract event must be rejected
        bad = [{"event": "start"}, {"note": "Predicting MIDI for x.wav"}]
        return self.v.rejects(lambda: self.v.stream(bad, 0)), "library print on stdout (the bug this gate caught)"

    def run(self):
        print(f"S2 service gate — {SERVICE.name} — python {sys.version.split()[0]} — "
              f"mode={'NOOP-CONTROL' if self.noop else 'FULL'}\nfixture: {RENDER} (61.7 s +6 s)")
        self.scenario("S01", "hello", self.s01)
        self.scenario("S02", "happy path: request -> progress -> .mid PPQ 960 on 1/32", self.s02)
        self.scenario("S03", "happy path: request -> progress -> typed error", self.s03)
        self.scenario("S04", "bad requests -> typed errors", self.s04, "failure-path")
        self.scenario("S05", "seeded determinism + grid moves the bytes", self.s05)
        self.scenario("S06", "tampered .mid must be caught", self.s06, "canary")
        self.scenario("S07", "stdout pollution must be caught", self.s07, "canary")
        passed = sum(r["verdict"] == "pass" for r in self.rows)
        can = [r for r in self.rows if r["kind"] == "canary"]
        rep = self.work / "s2-gate-report.json"
        rep.write_text(json.dumps({"mode": "noop" if self.noop else "full", "rows": self.rows, "lines": self.lines},
                                  indent=1), encoding="utf-8")
        print("-" * 72)
        print(f"scenarios {passed}/{len(self.rows)} pass | canaries detected "
              f"{sum(r['verdict'] == 'pass' for r in can)}/{len(can)} | stdout lines {len(self.lines)}")
        if self.noop:
            print("NOOP CONTROL: exit 1 is the REQUIRED proof that this gate can fail.")
        if self.keep:
            print(f"workdir kept: {self.work}")
        else:
            shutil.rmtree(self.work, ignore_errors=True)
        return 0 if passed == len(self.rows) else 1


def main():
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser()
    ap.add_argument("--noop", action="store_true")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    return Gate(a.noop, a.keep).run()


if __name__ == "__main__":
    sys.exit(main())
