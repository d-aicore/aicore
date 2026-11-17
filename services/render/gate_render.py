"""S5 gate: the native `aicore-render` (VST3 -> WAV, offline) against `aicore.service/1`, profile render.

Same load-bearing checks as the contract gate (contracts/sim/gate.py Verifier): every stdout line
validated, one terminal line, exit code matches, outputs exist and match sha256, manifest validated,
staging cleaned, errors typed. Plus: the WAV reads back with the declared format and is not silent.

  python services/render/gate_render.py [--exe build/app/aicore-render_artefacts/Release/aicore-render.exe]
  python services/render/gate_render.py --noop     # must exit 1

Fixture: a 16-note PPQ 960 MIDI written here with the contract's own writer (media.midi_bytes).
Plugins: the instruments installed in C:\\Program Files\\Common Files\\VST3 (Serum 2, Kick 3); a VST3
effect (no MIDI input) is used for the UNSUPPORTED_OPERATION case.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
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
from jsonschema_mini import Registry, validation_errors

VST3 = Path(os.environ.get("AICORE_VST3_DIR", r"C:\Program Files\Common Files\VST3"))
INSTRUMENTS = ["Serum2.vst3", "Kick 3.vst3"]
EFFECT = os.environ.get("AICORE_S5_EFFECT", str(VST3 / "Hysteresis.vst3"))


def wav_info(path):
    """(format tag, channels, rate, bits, frames, peak) of a float32/PCM WAV, stdlib only."""
    data = Path(path).read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise CheckFailed("render.wav is not RIFF/WAVE")
    pos, fmt, raw = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if len(body) != size:
            raise CheckFailed(f"render.wav chunk {cid!r} is truncated ({len(body)} of {size} bytes)")
        if cid == b"fmt ":
            tag, ch, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            if tag == 0xFFFE:
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, ch, rate, bits)
        elif cid == b"data":
            raw = body
        pos += 8 + size + (size & 1)
    if fmt is None or raw is None:
        raise CheckFailed("render.wav has no fmt or no data chunk")
    tag, ch, rate, bits = fmt
    frames = len(raw) // (ch * bits // 8)
    peak = 0.0
    if tag == 3 and bits == 32:
        import array
        a = array.array("f")
        a.frombytes(raw[: frames * ch * 4])
        peak = max((abs(x) for x in a), default=0.0)
    return {"tag": tag, "channels": ch, "rate": rate, "bits": bits, "frames": frames, "peak": peak}


class RenderVerifier(Verifier):
    def files(self, result, output_dir):
        out = Path(output_dir)
        root = out.resolve()
        for o in result["outputs"] + [{"path": result["manifestPath"]}]:
            # every published path is inside outputDir (contract §6: the service writes only there)
            self.expect(Path(o["path"]).resolve().parent == root, f"published outside outputDir: {o['path']}")
        if "manifestSha256" in result:
            mp0 = Path(result["manifestPath"])
            self.expect(media.sha256_file(mp0) == result["manifestSha256"], "manifest.json sha256 != done line")
        for o in result["outputs"]:
            p = Path(o["path"])
            self.expect(p.is_file(), f"output missing: {p}")
            self.expect(p.stat().st_size == o["bytes"], f"bytes moved: {p}")
            self.expect(media.sha256_file(p) == o["sha256"], f"sha256 moved: {p}")
            if o["role"] == "audio":
                w = wav_info(p)
                f = o["format"]
                self.expect(w["tag"] == 3 and w["bits"] == 32 and f["encoding"] == "float32", "WAV is not float32")
                self.expect(w["channels"] == f["channels"] == 2, "WAV is not stereo")
                self.expect(w["rate"] == f["sampleRate"], "WAV rate != declared")
                self.expect(abs(w["frames"] / w["rate"] - f["durationSeconds"]) < 0.01, "WAV length != declared")
                self.expect(w["peak"] > 1e-6, "published WAV is silent")
        mp = Path(result["manifestPath"])
        self.expect(mp.is_file(), "manifest missing")
        manifest = json.loads(mp.read_text(encoding="utf-8"))
        self.doc("task-manifest", manifest)
        self.expect(manifest["validation"] == "unreviewed", "service claimed approval")
        by = {o["path"]: o["sha256"] for o in manifest["outputs"]}
        for o in result["outputs"]:
            self.expect(by.get(o["path"]) == o["sha256"], f"manifest disagrees on {o['path']}")
        self.expect(not (out / ".staging").exists(), ".staging not cleaned")


class Gate:
    def __init__(self, exe, noop=False, keep=False):
        self.exe = Path(exe)
        self.reg = Registry(REPO / "contracts" / "schemas" / "v1")
        self.v = NoopVerifier() if noop else RenderVerifier(self.reg)
        self.noop, self.keep = noop, keep
        self.work = Path(tempfile.mkdtemp(prefix="aicore-s5-gate-"))
        self.rows, self.lines, self.current = [], [], ""
        self.mid = self.work / "fixture-960.mid"
        notes = [(i * 960, 480, 36 + (12 if i % 2 else 0), 110, 0) for i in range(16)]
        self.mid.write_bytes(media.midi_bytes(145.0, notes))
        # AICore's own settings (never the Core's): the gate runs the real `scan` into a temp settings
        # dir and sets roles through the CLI the Definicoes page uses.
        self.settings = self.work / "settings"
        self.settings.mkdir()
        # Host mechanics are proved with instruments installed on this PC (Serum 2, Kick 3) through a TEST
        # supported list; the owner's real list (exe dir) is checked in S11/S12 with the plugins it names.
        self.test_supported = self.work / "supported-test.json"
        self.test_supported.write_text(json.dumps({"version": 1, "folders": [], "plugins": [
            {"name": "Serum 2", "file": "Serum2.vst3", "roles": ["bass", "lead", "pad", "pluck"]},
            {"name": "Kick 3", "file": "Kick 3.vst3", "roles": ["drums"]},
            {"name": "ValhallaRoom", "file": "ValhallaRoom.vst3", "roles": ["reverb"]},
            {"name": "Hysteresis", "file": "Hysteresis.vst3", "roles": ["delay"]}]}), encoding="utf-8")
        self.env = dict(os.environ, AICORE_SETTINGS_DIR=str(self.settings), AICORE_SUPPORTED_PLUGINS=str(self.test_supported))
        for k in ("AICORE_CORE_PLUGINS", "AICORE_CORE_ROLES"):
            self.env.pop(k, None)

    def run_exe(self, args, stdin_text=None, env=None):
        p = subprocess.run([str(self.exe)] + args, input=stdin_text, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=300, env=env or self.env)
        docs = []
        for ln in p.stdout.splitlines():
            # contract §2.2: one JSON object per line, ASCII; an empty or non-ASCII line breaks it (audit 2026-11-16)
            if not ln.strip():
                raise CheckFailed("empty stdout line (contract: one JSON object per line)")
            if not ln.isascii():
                raise CheckFailed(f"non-ASCII stdout line: {ln[:120]!r}")
            self.lines.append({"scenario": self.current, "line": ln})
            try:
                docs.append(json.loads(ln))
            except ValueError:
                raise CheckFailed(f"non-JSON stdout line: {ln[:120]!r}")
        return p.returncode, docs

    def body(self, rid, out, plugin="Serum2.vst3", midi=None, **over):
        params = {"sampleRate": 44100, "blockSize": 512, "tailSeconds": 1}
        if plugin:
            params["plugin"] = str(VST3 / plugin) if not os.path.isabs(plugin) else plugin
        params.update(over.pop("params", {}))
        b = {"schemaVersion": "1.0", "requestId": rid, "profile": "render", "operation": "render",
             "params": params, "inputs": [{"role": "midi", "path": str(midi or self.mid)}], "outputDir": str(out)}
        b.update(over)
        return b

    def job(self, body, env=None):
        code, docs = self.run_exe(["job"], json.dumps(body) + "\n", env)
        # audit P1: a request the exe renders must also be valid for the published schema, or a client that
        # validates first would refuse a flow the gate calls green (the audio -> effects mode was exactly that)
        if docs and docs[-1].get("event") == "done":
            errs = validation_errors(self.reg, "service-task-request", body)
            self.v.expect(not errs, f"exe rendered a request the schema rejects: {errs[:3]}")
        return code, docs

    def out(self, name):
        d = self.work / name
        d.mkdir(parents=True, exist_ok=True)
        return d

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

    def s01(self):
        code, docs = self.run_exe(["hello"])
        self.v.expect(code == 0 and len(docs) == 1, f"hello exit {code}")
        self.v.doc("service-hello", docs[0])
        self.v.expect(docs[0]["profiles"][0]["name"] == "render", "render not announced")
        self.v.expect(".modified" not in docs[0]["service"]["version"] or self.allow_modified,
                      f"binary built from a dirty tree: {docs[0]['service']['version']}")
        return f"{docs[0]['service']['name']} {docs[0]['service']['version']}"

    def happy(self, plugin, tag):
        out = self.out(tag)
        code, docs = self.job(self.body(f"s5-{tag}", out, plugin))
        last = self.v.stream(docs, code)
        self.v.expect(last["event"] == "done", f"{plugin}: terminal {last['event']}: {last.get('error')}")
        phases = [d["phase"] for d in docs if d["event"] == "progress"]
        for ph in ("read", "load", "render", "write"):
            self.v.expect(ph in phases, f"phase {ph} missing")
        self.v.files(last["result"], out)
        m = last["result"]["metrics"]
        wav = next(o for o in last["result"]["outputs"] if o["role"] == "audio")
        return last, wav, m

    def s02(self):
        last, wav, m = self.happy("Serum2.vst3", "serum")
        self.serum_sha, self.serum_result, self.serum_out = wav["sha256"], last["result"], self.work / "serum"
        return f"{m['plugin']} {m['pluginVersion']}: {m['seconds']:.2f}s, peak {m['peakDbfs']:.1f} dBFS, rms {m['rmsDbfs']:.1f} dBFS, sha {wav['sha256'][:16]}"

    def s03(self):
        last, wav, m = self.happy("Kick 3.vst3", "kick3")
        return f"{m['plugin']} {m['pluginVersion']}: peak {m['peakDbfs']:.1f} dBFS (silent before the message-thread warm-up)"

    def s04(self):
        out = self.out("again")
        code, docs = self.job(self.body("s5-again", out))
        last = self.v.stream(docs, code)
        wav = next(o for o in last["result"]["outputs"] if o["role"] == "audio")
        same = wav["sha256"] == self.serum_sha
        return f"same request twice: {'identical' if same else 'DIFFERENT'} WAV ({wav['sha256'][:16]} vs {self.serum_sha[:16]}) [reported, plugin-dependent]"

    def s05(self):
        # typed error on the happy path: request -> progress -> error. A MIDI at PPQ 480 fails in 'read'.
        good = self.mid.read_bytes()
        m480 = self.work / "fixture-480.mid"
        m480.write_bytes(good[:12] + (480).to_bytes(2, "big") + good[14:])
        out = self.out("ppq480")
        code, docs = self.job(self.body("s5-480", out, midi=m480))
        last = self.v.stream(docs, code)
        n = sum(1 for d in docs if d["event"] == "progress")
        self.v.expect(docs[0]["event"] == "start" and n >= 1, "error not after start+progress")
        self.v.typed_error(last, "UNSUPPORTED_FORMAT", out)
        self.v.expect(not any(out.iterdir()), "failed render left files")
        return f"start + {n} progress -> error UNSUPPORTED_FORMAT (PPQ 480), exit {code}, outputDir empty"

    def s06(self):
        out = self.out("errors")
        nomidi = self.work / "not-a.mid"
        nomidi.write_bytes(b"not midi at all" * 4)
        cases = [
            ("missing midi", self.body("s5-e1", out, midi=self.work / "nope.mid"), "BROKEN_REFERENCE"),
            ("missing plugin", self.body("s5-e2", out, plugin=str(VST3 / "NoSuchSynth.vst3")), "BROKEN_REFERENCE"),
            ("not a .vst3", self.body("s5-e3", out, plugin=r"C:\Windows\System32\kernel32.dll"), "SCHEMA_INVALID"),
            ("hash mismatch", self.body("s5-e4", out, inputs=[{"role": "midi", "path": str(self.mid), "sha256": "0" * 64}]), "HASH_MISMATCH"),
            ("rate 22050", self.body("s5-e5", out, params={"sampleRate": 22050}), "SCHEMA_INVALID"),
            ("S1 cancelToken", self.body("s5-e6", out, cancelToken="x"), "SCHEMA_INVALID"),
            ("midi profile", dict(self.body("s5-e7", out), profile="midi", operation="transcribe"), "UNSUPPORTED_OPERATION"),
            ("not a MIDI", self.body("s5-e8", out, midi=nomidi), "UNSUPPORTED_FORMAT"),
            ("outputDir missing", self.body("s5-e9", self.work / "no-dir"), "BROKEN_REFERENCE"),
        ]
        cases += [
            ("effect as instrument", self.body("s5-e10", out, plugin=EFFECT), "UNSUPPORTED_OPERATION"),
            ("not in AICore list", self.body("s5-e11", out, plugin="Tactic.vst3"), "UNSUPPORTED_OPERATION", self.env_without("Tactic")),
            ("quarantined", self.body("s5-e16", out, plugin=r"kiloHearts\kHs Group.vst3"), "UNSUPPORTED_OPERATION"),
            ("role with no plugin", self.body("s5-e12", out, plugin=None, params={"role": "pad"}), "UNSUPPORTED_OPERATION"),
            ("plugin and role", self.body("s5-e13", out, params={"role": "bass"}), "SCHEMA_INVALID"),
            ("instrument as effect", self.body("s5-e14", out, params={"effects": [{"plugin": str(VST3 / "Kick 3.vst3")}]}), "UNSUPPORTED_OPERATION"),
            ("bad base64 state", self.body("s5-e15", out, params={"pluginStateBase64": "%%%%not-base64%%%%"}), "SCHEMA_INVALID"),
        ]
        for case in cases:
            label, b, want = case[:3]
            code, docs = self.job(b, case[3] if len(case) > 3 else None)
            last = self.v.stream(docs, code)
            got = last.get("error", {}).get("code", last["event"])
            self.v.expect(got == want, f"{label}: got {got} want {want}")
            self.v.typed_error(last, want, None)
        for label, raw in (("empty stdin", "\n"), ("not JSON", "{x\n")):
            code, docs = self.run_exe(["job"], raw)
            last = self.v.stream(docs, code)
            self.v.expect(last.get("error", {}).get("code") == "INVALID_JSON", f"{label}: {last}")
        return f"{len(cases) + 2} bad requests -> typed error + exit 2"

    def env_without(self, name):
        alt = self.work / f"settings-without-{name}"
        alt.mkdir(exist_ok=True)
        lst = json.loads((self.settings / "plugins.json").read_text(encoding="utf-8"))
        lst["plugins"] = [p for p in lst["plugins"] if p["name"] != name]
        (alt / "plugins.json").write_text(json.dumps(lst), encoding="utf-8")
        shutil.copy(self.settings / "roles.json", alt / "roles.json")
        return dict(self.env, AICORE_SETTINGS_DIR=str(alt))

    def s09(self):
        # the AI path: instrument and effect chosen by ROLE through AICore's own roles.json
        out = self.out("roles")
        code, docs = self.job(self.body("s5-roles", out, plugin=None, params={"role": "bass", "effectRoles": ["reverb"]}))
        last = self.v.stream(docs, code)
        self.v.expect(last["event"] == "done", f"terminal {last['event']}: {last.get('error')}")
        self.v.files(last["result"], out)
        m = last["result"]["metrics"]
        self.v.expect(m["role"] == "bass" and m["effectRoles"] == ["reverb"] and len(m["effects"]) == 1, f"roles not applied: {m}")
        wav = next(o for o in last["result"]["outputs"] if o["role"] == "audio")
        self.v.expect(wav["sha256"] != self.serum_sha, "reverb role did not change the audio")
        return f"role bass -> {m['plugin']}, effectRoles reverb -> {m['effects'][0]['plugin']}; peak {m['peakDbfs']:.1f} dBFS, differs from dry"

    def s00(self):
        code, docs = self.run_exe(["scan"])
        self.v.expect(code == 0 and docs and "plugins" in docs[-1], f"scan exit {code}: {docs[-1] if docs else None}")
        s = docs[-1]
        lst = json.loads((self.settings / "plugins.json").read_text(encoding="utf-8"))
        names = {p["name"]: p for p in lst["plugins"]}
        self.v.expect(names.get("Serum 2", {}).get("isInstrument") is True, "Serum 2 not found as instrument")
        self.v.expect(names.get("ValhallaRoom", {}).get("isInstrument") is False, "ValhallaRoom not found as effect")
        self.v.expect(not any("Core" in str(Path(f)) and "AppData" in str(f) for f in lst["folders"]), "scan read the Core's folders")
        sup = json.loads(self.test_supported.read_text(encoding="utf-8"))
        allowed = {Path(x["file"]).name.lower() for x in sup["plugins"]}
        self.v.expect(all(Path(p["file"]).name.lower() in allowed for p in lst["plugins"]),
                      "scan listed a plugin that is not in supported-plugins.json")
        self.v.expect(s["ignored"] > 0, "scan ignored nothing: it is not filtering by the supported list")
        for role, plug in (("bass", "Serum2.vst3"), ("drums", "Kick 3.vst3"), ("reverb", r"ValhallaDSP\ValhallaRoom.vst3")):
            code, docs = self.run_exe(["roles", "set", role, str(VST3 / plug)])
            self.v.expect(code == 0 and "roles" in docs[-1], f"roles set {role}: {docs}")
        bad = []
        for role, plug, want in (("bass", r"ValhallaDSP\ValhallaRoom.vst3", "UNSUPPORTED_OPERATION"),
                                 ("reverb", "Serum2.vst3", "UNSUPPORTED_OPERATION"),
                                 ("lead", "NoSuchSynth.vst3", "UNSUPPORTED_OPERATION"),
                                 ("lead", "Kick 3.vst3", "UNSUPPORTED_OPERATION"),
                                 ("lead", r"kiloHearts\Phase Plant.vst3", "UNSUPPORTED_OPERATION"),
                                 ("guitar", "Serum2.vst3", "SCHEMA_INVALID")):
            code, docs = self.run_exe(["roles", "set", role, str(VST3 / plug)])
            got = docs[-1].get("error", {}).get("code") if docs else None
            self.v.expect(code == 2 and got == want, f"roles set {role} {plug}: got {got} want {want}")
            bad.append(role)
        inst = sorted(p["name"] for p in lst["plugins"] if p["isInstrument"])
        return (f"scan (supported list only): {s['files']} files, {s['ignored']} other .vst3 ignored, {s['plugins']} plugins "
                f"({s['instruments']} instruments: {inst}), {s['quarantined']} quarantined, missing {s['missing']}; "
                f"roles set bass/drums/reverb; 6 wrong role choices refused")

    def s11(self):
        # the owner's real list (supported-plugins.json next to the exe) on this PC
        real = self.work / "settings-real"
        real.mkdir(exist_ok=True)
        env = dict(os.environ, AICORE_SETTINGS_DIR=str(real))
        env.pop("AICORE_SUPPORTED_PLUGINS", None)
        code, docs = self.run_exe(["scan"], env=env)
        self.v.expect(code == 0 and docs and "plugins" in docs[-1], f"scan exit {code}")
        s = docs[-1]
        lst = json.loads((real / "plugins.json").read_text(encoding="utf-8"))
        names = sorted(p["name"] for p in lst["plugins"])
        for need in ("Vital", "Airwindows Consolidated", "Dragonfly Room Reverb"):
            self.v.expect(need in names, f"{need} not found by the scan of the real list")
        for role, name in (("reverb", "Dragonfly Room Reverb"), ("drive", "Airwindows Consolidated")):
            f = next(p["file"] for p in lst["plugins"] if p["name"] == name)
            code, docs = self.run_exe(["roles", "set", role, f], env=env)
            self.v.expect(code == 0, f"roles set {role} {name}: {docs}")
        self.real_env = env
        vital = next(p["file"] for p in lst["plugins"] if p["name"] == "Vital")
        code, docs = self.run_exe(["roles", "set", "bass", vital], env=env)
        self.v.expect(code == 0, f"roles set bass Vital: {docs}")
        out = self.out("vital")
        code, docs = self.job({"schemaVersion": "1.0", "requestId": "s5-vital", "profile": "render", "operation": "render",
                               "params": {"role": "bass", "effectRoles": ["drive", "reverb"], "sampleRate": 44100,
                                          "blockSize": 512, "tailSeconds": 1},
                               "inputs": [{"role": "midi", "path": str(self.mid)}], "outputDir": str(out)}, env)
        last = self.v.stream(docs, code)
        self.v.expect(last["event"] == "done", f"Vital by role: {last.get('error')}")
        self.v.files(last["result"], out)
        m = last["result"]["metrics"]
        return (f"real list: {names}; missing {s['missing']}; {s['ignored']} other .vst3 ignored; "
                f"role bass -> {m['plugin']} {m['pluginVersion']} + {[e['plugin'] for e in m['effects']]}: peak {m['peakDbfs']:.1f} dBFS")

    def s12(self):
        # Vita (python) renders the .mid dry; aicore-render applies drive + reverb by role to that WAV
        vpy = Path(os.environ.get("AICORE_VITA_PY", r"D:\AICore\plugins\vita-venv\Scripts\python.exe"))
        self.v.expect(vpy.is_file(), f"Vita venv missing: {vpy}")
        vout = self.out("vita")
        req = {"schemaVersion": "1.0", "requestId": "s5-vita", "profile": "render", "operation": "render",
               "params": {"engine": "vita", "sampleRate": 44100, "blockSize": 512, "tailSeconds": 1},
               "inputs": [{"role": "midi", "path": str(self.mid)}], "outputDir": str(vout)}
        p = subprocess.run([str(vpy), str(REPO / "services" / "vita" / "aicore_vita_service.py"), "job"],
                           input=json.dumps(req) + "\n", capture_output=True, text=True, timeout=300)
        docs = [json.loads(l) for l in p.stdout.splitlines() if l.strip()]
        for d in docs:
            self.lines.append({"scenario": self.current, "line": json.dumps(d)})
        last = self.v.stream(docs, p.returncode)
        self.v.expect(last["event"] == "done", f"vita: {last.get('error')}")
        self.v.files(last["result"], vout)
        dry = next(o for o in last["result"]["outputs"] if o["role"] == "audio")
        fout = self.out("vita-fx")
        code, docs = self.job({"schemaVersion": "1.0", "requestId": "s5-vita-fx", "profile": "render", "operation": "render",
                               "params": {"effectRoles": ["drive", "reverb"], "sampleRate": 44100, "blockSize": 512, "tailSeconds": 2},
                               "inputs": [{"role": "audio", "path": dry["path"]}], "outputDir": str(fout)}, self.real_env)
        last2 = self.v.stream(docs, code)
        self.v.expect(last2["event"] == "done", f"fx on vita: {last2.get('error')}")
        self.v.files(last2["result"], fout)
        wet = next(o for o in last2["result"]["outputs"] if o["role"] == "audio")
        self.v.expect(wet["sha256"] != dry["sha256"], "effects did not change the audio")
        m1, m2 = last["result"]["metrics"], last2["result"]["metrics"]
        # typed error on the vita path: a 480 PPQ .mid
        good = self.mid.read_bytes()
        m480 = self.work / "vita-480.mid"
        m480.write_bytes(good[:12] + (480).to_bytes(2, "big") + good[14:])
        eout = self.out("vita-err")
        req["inputs"] = [{"role": "midi", "path": str(m480)}]
        req["outputDir"] = str(eout)
        p = subprocess.run([str(vpy), str(REPO / "services" / "vita" / "aicore_vita_service.py"), "job"],
                           input=json.dumps(req) + "\n", capture_output=True, text=True, timeout=300)
        docs = [json.loads(l) for l in p.stdout.splitlines() if l.strip()]
        last3 = self.v.stream(docs, p.returncode)
        self.v.typed_error(last3, "UNSUPPORTED_FORMAT", eout)
        # audio mode reads WAV only, and a truncated data chunk is refused, not rendered shorter (audit 2026-11-16)
        drybytes = Path(dry["path"]).read_bytes()
        for label, data in (("truncated WAV", drybytes[:len(drybytes) // 2]),
                            ("not a WAV", b"FORM" + drybytes[4:])):
            bad = self.work / f"audio-{label.replace(' ', '-')}.wav"
            bad.write_bytes(data)
            bout = self.out(f"fx-{label.replace(' ', '-')}")
            code, docs = self.job({"schemaVersion": "1.0", "requestId": "s5-fx-bad", "profile": "render", "operation": "render",
                                   "params": {"effectRoles": ["drive"], "sampleRate": 44100, "blockSize": 512, "tailSeconds": 0},
                                   "inputs": [{"role": "audio", "path": str(bad)}], "outputDir": str(bout)}, self.real_env)
            self.v.typed_error(self.v.stream(docs, code), "UNSUPPORTED_FORMAT", bout)
        return (f"Vita {m1['vitaVersion']}: {m1['notes']} notes, peak {m1['peakDbfs']:.1f} dBFS -> "
                f"{[e['plugin'] for e in m2['effects']]}: peak {m2['peakDbfs']:.1f} dBFS; PPQ 480 -> UNSUPPORTED_FORMAT")

    def s13(self):
        # AICore's own sampler (drums from samples; never the Core's): kit of one-shot WAVs per note
        import math, random
        sr = 44100
        kick = [0.9 * math.exp(-i / (0.08 * sr)) * math.sin(2 * math.pi * (50 + 150 * math.exp(-i / (0.01 * sr))) * i / sr)
                for i in range(int(0.4 * sr))]
        rnd = random.Random(1)
        hat = [0.5 * math.exp(-i / (0.02 * sr)) * (rnd.random() * 2 - 1) for i in range(int(0.15 * sr))]
        kp, hp = self.work / "kick.wav", self.work / "hat.wav"
        kp.write_bytes(media.wav_bytes_pcm16(kick, sr))
        hp.write_bytes(media.wav_bytes_pcm16(hat, sr))
        dm = self.work / "drums.mid"
        dm.write_bytes(media.midi_bytes(145.0, [(i * 480, 240, 36 if i % 2 == 0 else 48, 110, 9) for i in range(16)]))
        kit = [{"note": 36, "sample": str(kp)}, {"note": 48, "sample": str(hp)}]
        shas = []
        for i in (1, 2):
            out = self.out(f"sampler{i}")
            code, docs = self.job({"schemaVersion": "1.0", "requestId": f"s5-smp{i}", "profile": "render", "operation": "render",
                                   "params": {"engine": "sampler", "kit": kit, "sampleRate": 44100, "blockSize": 512, "tailSeconds": 1},
                                   "inputs": [{"role": "midi", "path": str(dm)}], "outputDir": str(out)})
            last = self.v.stream(docs, code)
            self.v.expect(last["event"] == "done", f"sampler: {last.get('error')}")
            self.v.files(last["result"], out)
            shas.append(next(o for o in last["result"]["outputs"] if o["role"] == "audio")["sha256"])
            m = last["result"]["metrics"]
        self.v.expect(shas[0] == shas[1], "sampler not deterministic")
        eout = self.out("sampler-err")
        bad = [("missing sample", [{"note": 36, "sample": str(self.work / "nope.wav")}], "BROKEN_REFERENCE"),
               ("duplicate note", kit + [{"note": 36, "sample": str(hp)}], "SCHEMA_INVALID"),
               ("not a wav", [{"note": 36, "sample": str(self.mid)}], "SCHEMA_INVALID")]
        for label, k, want in bad:
            code, docs = self.job({"schemaVersion": "1.0", "requestId": "s5-smp-e", "profile": "render", "operation": "render",
                                   "params": {"engine": "sampler", "kit": k, "sampleRate": 44100, "blockSize": 512, "tailSeconds": 1},
                                   "inputs": [{"role": "midi", "path": str(dm)}], "outputDir": str(eout)})
            last = self.v.stream(docs, code)
            got = last.get("error", {}).get("code", last["event"])
            self.v.expect(got == want, f"{label}: got {got} want {want}")
        return (f"kit of 2 samples, 16 hits: peak {m['peakDbfs']:.1f} dBFS, same sha twice ({shas[0][:16]}); "
                f"3 bad kits -> typed errors")

    def s07(self):
        bad = self.work / "tampered"
        shutil.copytree(self.serum_out, bad)
        res = json.loads(json.dumps(self.serum_result).replace(str(self.serum_out).replace("\\", "\\\\"),
                                                               str(bad).replace("\\", "\\\\")))
        p = bad / "render.wav"
        data = bytearray(p.read_bytes())
        data[-9] ^= 0x01
        p.write_bytes(bytes(data))
        return self.v.rejects(lambda: self.v.files(res, bad)), "flipped one byte of render.wav"

    def s08(self):
        bad = self.work / "silent"
        shutil.copytree(self.serum_out, bad)
        res = json.loads(json.dumps(self.serum_result).replace(str(self.serum_out).replace("\\", "\\\\"),
                                                               str(bad).replace("\\", "\\\\")))
        p = bad / "render.wav"
        data = p.read_bytes()
        idx = data.index(b"data") + 8
        silent = data[:idx] + bytes(len(data) - idx)
        p.write_bytes(silent)
        for o in res["outputs"]:  # a consistent sabotage: hash re-declared, only silence is wrong
            if o["role"] == "audio":
                o["sha256"] = media.sha256_file(p)
        mp = Path(res["manifestPath"])
        man = json.loads(mp.read_text(encoding="utf-8"))
        for o in man["outputs"]:
            if o["role"] == "audio":
                o["sha256"] = media.sha256_file(p)
        mp.write_text(json.dumps(man), encoding="utf-8")
        return self.v.rejects(lambda: self.v.files(res, bad)), "silent WAV with matching hashes"

    def run(self):
        print(f"S5 render gate — {self.exe} — mode={'NOOP-CONTROL' if self.noop else 'FULL'}")
        self.scenario("S00", "own scan of the supported list + roles (no Core files)", self.s00)
        self.scenario("S01", "hello", self.s01)
        self.scenario("S02", "happy path: .mid -> Serum 2 -> render.wav", self.s02)
        self.scenario("S03", "happy path: .mid -> Kick 3 -> render.wav", self.s03)
        self.scenario("S04", "repeatability (reported)", self.s04)
        self.scenario("S05", "happy path: request -> progress -> typed error", self.s05)
        self.scenario("S06", "bad requests -> typed errors", self.s06, "failure-path")
        self.scenario("S09", "AI path: instrument + effect by role (AICore list)", self.s09)
        self.scenario("S11", "owner's real list: Vital + Airwindows + Dragonfly, synth role by Vital", self.s11)
        self.scenario("S12", "Vita .mid -> WAV, then drive + reverb by role", self.s12)
        self.scenario("S13", "own sampler: drums from a kit of samples", self.s13)
        self.scenario("S07", "tampered WAV must be caught", self.s07, "canary")
        self.scenario("S08", "silent WAV must be caught", self.s08, "canary")
        passed = sum(r["verdict"] == "pass" for r in self.rows)
        can = [r for r in self.rows if r["kind"] == "canary"]
        (self.work / "s5-gate-report.json").write_text(json.dumps({"rows": self.rows, "lines": self.lines}, indent=1),
                                                       encoding="utf-8")
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

    allow_modified = False


def main():
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(REPO / "build" / "app" / "aicore-render_artefacts" / "Release" / "aicore-render.exe"))
    ap.add_argument("--noop", action="store_true")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--allow-modified", action="store_true", help="accept a binary built from a dirty tree (dev only)")
    a = ap.parse_args()
    g = Gate(a.exe, a.noop, a.keep)
    g.allow_modified = a.allow_modified
    return g.run()


if __name__ == "__main__":
    sys.exit(main())
