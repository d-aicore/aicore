"""Contract gate: the REAL lora service (memory -> dataset -> train) against `aicore.service/1`.

Same load-bearing checks as the contract gate (contracts/sim/gate.py `Verifier`): every stdout line validated,
exactly one terminal line, exit code matches it, outputs exist and match their sha256, manifest validated,
staging cleaned, errors typed. Plus the dataset rules: every example in exactly one split, no family across
splits, examples.jsonl == dataset manifest.

  python gate_service.py           # exit 0 green
  python gate_service.py --noop    # checks become no-ops -> canaries undetected -> exit 1 (required proof)

Run it with the AICORE interpreter (D:\\AICore\\python\\Scripts\\python.exe): the service imports the AICORE code.
CPU only, a few seconds. Fixture: a synthetic 20-episode Memory package (make_fixture.py).
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
sys.path.insert(0, str(HERE))
import media
from gate import CheckFailed, NoopVerifier, Verifier
from jsonschema_mini import Registry
import make_fixture

SERVICE = HERE / "aicore_lora_service.py"
RECIPE = HERE / "fixtures" / "qwen3-4b-qlora-probe.recipe.json"
BELOW_NORMAL = 0x00004000
DATASET_PHASES = {"read", "curate", "split", "write"}


class LoraVerifier(Verifier):
    """Verifier.files with the lora outputs (json / jsonl) and `synthetic` as the request said."""

    def files(self, result, output_dir, synthetic=False):
        out = Path(output_dir)
        for o in result["outputs"]:
            p = Path(o["path"])
            self.expect(p.is_file(), f"output missing: {p}")
            self.expect(p.stat().st_size == o["bytes"], f"bytes moved: {p}")
            self.expect(media.sha256_file(p) == o["sha256"], f"sha256 moved: {p}")
            if o["format"]["container"] == "jsonl":
                rows = [ln for ln in p.read_text(encoding="utf-8").splitlines() if ln.strip()]
                self.expect(len(rows) == o["format"]["records"], f"records {len(rows)} != declared")
                for ln in rows:
                    json.loads(ln)
            elif o["format"]["container"] == "json":
                json.loads(p.read_text(encoding="utf-8"))
        mp = Path(result["manifestPath"])
        self.expect(mp.is_file(), f"manifest missing: {mp}")
        manifest = json.loads(mp.read_text(encoding="utf-8"))
        self.doc("task-manifest", manifest)
        self.expect(manifest["validation"] == "unreviewed", "service claimed approval")
        self.expect(manifest["synthetic"] is synthetic, f"manifest synthetic {manifest['synthetic']} != {synthetic}")
        by_path = {o["path"]: o for o in manifest["outputs"]}
        for o in result["outputs"]:
            self.expect(o["path"] in by_path and by_path[o["path"]]["sha256"] == o["sha256"],
                        f"output not in manifest / hash differs: {o['path']}")
        self.expect(not list(out.rglob("*.part")), "staging .part left behind")
        self.expect(not (out / ".staging").exists(), ".staging not cleaned")

    def splits(self, manifest_path, examples_path):
        manifest = json.loads(Path(manifest_path).read_text(encoding="utf-8"))
        rows = [json.loads(ln) for ln in Path(examples_path).read_text(encoding="utf-8").splitlines() if ln.strip()]
        listed = {(e["exampleId"], e["version"]): e for e in manifest["examples"]}
        got = {(r["example"]["exampleId"], r["example"]["version"]): r["split"] for r in rows}
        self.expect(set(got) == set(listed), "examples.jsonl and dataset manifest list different examples")
        self.expect(all(listed[k]["split"] == s for k, s in got.items()), "split differs from the manifest")
        fam = {}
        for e in manifest["examples"]:
            fam.setdefault(e["groupId"], set()).add(e["split"])
        self.expect(all(len(s) == 1 for s in fam.values()), "a family spans two splits")
        counts = {s: sum(1 for v in got.values() if v == s) for s in ("train", "validation", "test")}
        self.expect(all(counts.values()), f"an empty split: {counts}")
        self.expect(all(r["example"]["review"]["status"] == "approved" for r in rows), "unapproved example")
        return manifest, counts


class LoraNoop(NoopVerifier):
    def files(self, result, output_dir, synthetic=False):
        pass

    def splits(self, manifest_path, examples_path):
        return {"datasetHash": None}, {}


class Gate:
    def __init__(self, noop=False, keep=False):
        self.reg = Registry(REPO / "contracts" / "schemas" / "v1")
        self.v = LoraNoop() if noop else LoraVerifier(self.reg)
        self.noop, self.keep = noop, keep
        self.work = Path(tempfile.mkdtemp(prefix="aicore-lora-gate-"))
        self.rows, self.lines = [], []
        self.current = ""
        self.package = make_fixture.build(self.work, 20, "syn-lora-gate")

    def svc(self, args, stdin_text=None, env_extra=None):
        env = dict(os.environ, **(env_extra or {}))
        p = subprocess.run([sys.executable, str(SERVICE)] + args, input=stdin_text, capture_output=True,
                           text=True, encoding="utf-8", errors="replace", env=env, timeout=600,
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

    def out(self, name):
        d = self.work / name
        d.mkdir(parents=True, exist_ok=True)
        return d

    def ds_body(self, rid, out, synthetic=True, **over):
        params = {"datasetId": "syn-lora", "approvedBy": "syn-owner", "attestedBy": "syn-owner"}
        if synthetic:
            params["allowSyntheticSources"] = True
        params.update(over.pop("params", {}))
        b = {"schemaVersion": "1.0", "requestId": rid, "profile": "lora", "operation": "dataset", "params": params,
             "inputs": [{"role": "memory_package", "path": str(self.package)}], "outputDir": str(out),
             "synthetic": synthetic}
        b.update(over)
        return b

    def tr_body(self, rid, out, synthetic, **over):
        b = {"schemaVersion": "1.0", "requestId": rid, "profile": "lora", "operation": "train", "params": {},
             "inputs": [{"role": "dataset_manifest", "path": str(self.ds_manifest)},
                        {"role": "examples", "path": str(self.ds_examples)},
                        {"role": "recipe", "path": str(self.recipe)}], "outputDir": str(out), "synthetic": synthetic}
        b.update(over)
        return b

    def job(self, body, env_extra=None):
        return self.svc(["job"], json.dumps(body) + "\n", env_extra)

    def scenario(self, sid, name, fn, kind="positive"):
        self.current = sid
        t0 = time.monotonic()
        try:
            detail, verdict = fn() or "", "pass"
        except CheckFailed as e:
            detail, verdict = f"{type(e).__name__}: {e}", "fail"
        except Exception as e:
            detail, verdict = f"UNEXPECTED {type(e).__name__}: {e}", "fail"
        if kind == "canary" and verdict == "pass":
            detected, detail = detail
            verdict = "pass" if detected else "fail"
        self.rows.append({"id": sid, "name": name, "kind": kind, "verdict": verdict, "detail": str(detail)[:600],
                          "seconds": round(time.monotonic() - t0, 2)})
        print(f"[{'OK ' if verdict == 'pass' else 'RED'}] {sid} {name} ({kind}) - {str(detail)[:240]}")

    # ---- scenarios ----

    def s01(self):
        code, docs = self.svc(["hello"])
        self.v.expect(code == 0 and len(docs) == 1, f"hello exit {code} lines {len(docs)}")
        self.v.doc("service-hello", docs[0])
        prof = docs[0]["profiles"]
        self.v.expect(prof == [{"name": "lora", "operations": ["dataset", "train"]}], f"profiles {prof}")
        return f"{docs[0]['service']['name']} {docs[0]['service']['version']} lora: dataset, train"

    def s02(self):
        out = self.out("dataset")
        code, docs = self.job(self.ds_body("lora-gate-ds", out))
        last = self.v.stream(docs, code)
        self.v.expect(last["event"] == "done", f"terminal {last['event']}: {last.get('error')}")
        phases = {d["phase"] for d in docs if d["event"] == "progress"}
        self.v.expect(phases == DATASET_PHASES, f"phases {sorted(phases)}")
        self.v.files(last["result"], out, synthetic=True)
        by_role = {o["role"]: Path(o["path"]) for o in last["result"]["outputs"]}
        self.v.expect(sorted(by_role) == ["dataset_manifest", "examples", "report"], f"roles {sorted(by_role)}")
        self.ds_manifest, self.ds_examples = by_role.get("dataset_manifest"), by_role.get("examples")
        self.ds_result, self.ds_out = last["result"], out
        manifest, counts = self.v.splits(self.ds_manifest, self.ds_examples)
        self.ds_hash = manifest["datasetHash"]
        # the C06 recipe names its dataset (datasetRef): the gate's recipe points at the dataset it just built
        recipe = json.loads(RECIPE.read_text(encoding="utf-8"))
        recipe["datasetRef"] = manifest["datasetId"]
        self.recipe = self.work / "recipe.json"
        self.recipe.write_text(json.dumps(recipe, indent=1), encoding="utf-8")
        return f"exit 0, {len(docs)} lines, 20 episodes -> splits {counts}, datasetHash {str(self.ds_hash)[:16]}"

    def s03(self):
        # typed error on the happy path: a real train request reads + plans, then says the trainer is missing
        out = self.out("train-real")
        code, docs = self.job(self.tr_body("lora-gate-tr-real", out, synthetic=False))
        last = self.v.stream(docs, code)
        n = sum(1 for d in docs if d["event"] == "progress")
        self.v.expect(docs[0]["event"] == "start" and n >= 2, "error did not come after start + read + plan")
        self.v.typed_error(last, "BACKEND_FAILED", out)
        self.v.expect(last["error"].get("details", {}).get("phase") == "train", "error not in phase train")
        self.v.expect(not any(out.iterdir()), f"failed job left files: {[p.name for p in out.iterdir()]}")
        return f"start + {n} progress -> error BACKEND_FAILED (no real trainer), exit {code}, outputDir empty"

    def s04(self):
        out = self.out("train-mock")
        code, docs = self.job(self.tr_body("lora-gate-tr-mock", out, synthetic=True))
        last = self.v.stream(docs, code)
        self.v.expect(last["event"] == "done", f"terminal {last['event']}: {last.get('error')}")
        self.v.files(last["result"], out, synthetic=True)
        m = last["result"]["metrics"]
        self.v.expect(m.get("trainer") == "mock" and m.get("stepsDone") == m.get("plannedSteps"), f"metrics {m}")
        rep = json.loads(Path(last["result"]["outputs"][0]["path"]).read_text(encoding="utf-8"))
        self.v.expect(rep["synthetic"] is True and rep["lastCheckpoint"]["complete"] is True, "report not synthetic/ckpt")
        self.v.expect(rep["losses"][-1] < rep["losses"][0], "mock loss did not fall")
        parts = rep.get("datasetFingerprintParts", {})
        self.v.expect(parts.get("examples") == media.sha256_file(self.ds_examples)
                      and parts.get("manifest") == media.sha256_file(self.ds_manifest),
                      "the dataset fingerprint does not cover examples.jsonl")
        self.v.expect(rep["fingerprints"]["dataset"] != parts.get("manifest"),
                      "the dataset fingerprint is the manifest hash alone")
        return f"mock trainer {m.get('stepsDone')} step(s), loss {m.get('firstLoss')} -> {m.get('lastLoss')}, synthetic"

    def s05(self):
        out = self.out("errors")
        notzip = self.work / "not-a.zip"
        notzip.write_bytes(b"not a package" * 10)
        cases = [
            ("missing package", self.ds_body("lg-e1", out, inputs=[{"role": "memory_package", "path": str(self.work / "nope.zip")}]), "BROKEN_REFERENCE"),
            ("hash mismatch", self.ds_body("lg-e2", out, inputs=[{"role": "memory_package", "path": str(self.package), "sha256": "0" * 64}]), "HASH_MISMATCH"),
            ("not a ZIP", self.ds_body("lg-e3", out, inputs=[{"role": "memory_package", "path": str(notzip)}]), "UNSUPPORTED_FORMAT"),
            ("no approvedBy", self.ds_body("lg-e4", out, params={"approvedBy": None}), "SCHEMA_INVALID"),
            ("unknown param", self.ds_body("lg-e5", out, params={"lr": 1}), "SCHEMA_INVALID"),
            ("synthetic unlock on a real request", self.ds_body("lg-e6", out, synthetic=False, params={"allowSyntheticSources": True}), "SCHEMA_INVALID"),
            ("synthetic sources never make a real dataset", self.ds_body("lg-e7", out, synthetic=False), "INSUFFICIENT_DATA"),
            ("operation not offered", self.ds_body("lg-e8", out, operation="export"), "UNSUPPORTED_OPERATION"),
            ("S1 cancelToken", self.ds_body("lg-e9", out, cancelToken="x"), "SCHEMA_INVALID"),
            ("outputDir missing", self.ds_body("lg-e10", self.work / "no-dir"), "BROKEN_REFERENCE"),
            ("train without recipe", self.tr_body("lg-e11", out, True, inputs=[{"role": "dataset_manifest", "path": str(self.ds_manifest)}, {"role": "examples", "path": str(self.ds_examples)}]), "SCHEMA_INVALID"),
        ]
        short = self.work / "short.jsonl"
        lines = self.ds_examples.read_text(encoding="utf-8").splitlines()
        short.write_text("\n".join(lines[1:]) + "\n", encoding="utf-8")
        cases.append(("train with an example missing", self.tr_body("lg-e12", out, True, inputs=[
            {"role": "dataset_manifest", "path": str(self.ds_manifest)}, {"role": "examples", "path": str(short)},
            {"role": "recipe", "path": str(self.recipe)}]), "BROKEN_REFERENCE"))
        cases.append(("AICORE source missing", self.tr_body("lg-e13", out, True), "BACKEND_FAILED"))
        # audit 2026-11-16: recipe for another dataset, a repeated example, an invalid example, a reused outputDir
        cases.append(("recipe for another dataset", self.tr_body("lg-e14", out, True, inputs=[
            {"role": "dataset_manifest", "path": str(self.ds_manifest)}, {"role": "examples", "path": str(self.ds_examples)},
            {"role": "recipe", "path": str(RECIPE)}]), "BROKEN_REFERENCE"))
        dup = self.work / "dup.jsonl"
        dup.write_text("\n".join(lines + lines[:1]) + "\n", encoding="utf-8")
        cases.append(("repeated example", self.tr_body("lg-e15", out, True, inputs=[
            {"role": "dataset_manifest", "path": str(self.ds_manifest)}, {"role": "examples", "path": str(dup)},
            {"role": "recipe", "path": str(self.recipe)}]), "SCHEMA_INVALID"))
        bad_row = json.loads(lines[0])
        bad_row["example"].pop("targetMessage")  # a C05 required field
        invalid = self.work / "invalid.jsonl"
        invalid.write_text("\n".join([json.dumps(bad_row)] + lines[1:]) + "\n", encoding="utf-8")
        cases.append(("invalid example", self.tr_body("lg-e16", out, True, inputs=[
            {"role": "dataset_manifest", "path": str(self.ds_manifest)}, {"role": "examples", "path": str(invalid)},
            {"role": "recipe", "path": str(self.recipe)}]), "SCHEMA_INVALID"))
        cases.append(("reused outputDir", self.ds_body("lg-e17", self.ds_out), "OUTPUT_EXISTS"))
        for label, b, want in cases:
            if b.get("params", {}).get("approvedBy", "") is None:
                del b["params"]["approvedBy"]
            env = {"AICORE_PY_SRC": str(self.work / "no-aicore")} if label == "AICORE source missing" else None
            code, docs = self.job(b, env)
            last = self.v.stream(docs, code)
            got = last.get("error", {}).get("code", last["event"])
            self.v.expect(got == want, f"{label}: got {got} want {want}")
            self.v.typed_error(last, want, None)
        self.v.expect(not (out / "manifest.json").exists() and not (out / ".staging").exists(), "errors left files")
        for label, raw in (("empty stdin", "\n"), ("not JSON", "{x\n")):
            code, docs = self.svc(["job"], raw)
            last = self.v.stream(docs, code)
            self.v.expect(last.get("error", {}).get("code") == "INVALID_JSON", f"{label}: {last}")
        return f"{len(cases) + 2} bad requests -> typed error + exit 2"

    def s06(self):
        out = self.out("dataset-again")
        code, docs = self.job(self.ds_body("lora-gate-ds2", out))
        last = self.v.stream(docs, code)
        self.v.expect(last["event"] == "done", f"terminal {last['event']}")
        by_role = {o["role"]: Path(o["path"]) for o in last["result"]["outputs"]}
        manifest, _ = self.v.splits(by_role.get("dataset_manifest"), by_role.get("examples"))
        self.v.expect(manifest["datasetHash"] == self.ds_hash, "same memory, different dataset (splits not seeded)")
        return f"same package -> same datasetHash {str(self.ds_hash)[:16]}"

    def s07(self):
        bad = self.work / "tampered"
        shutil.copytree(self.ds_out, bad)
        res = json.loads(json.dumps(self.ds_result).replace(str(self.ds_out).replace("\\", "\\\\"),
                                                            str(bad).replace("\\", "\\\\")))
        p = bad / "examples.jsonl"
        data = bytearray(p.read_bytes())
        data[10] ^= 0x01
        p.write_bytes(bytes(data))
        return self.v.rejects(lambda: self.v.files(res, bad, synthetic=True)), "flipped one byte of examples.jsonl"

    def s08(self):
        bad = [{"event": "start"}, {"note": "a library printed this"}]
        return self.v.rejects(lambda: self.v.stream(bad, 0)), "non-contract line on stdout"

    def run(self):
        print(f"lora service gate — {SERVICE.name} — python {sys.version.split()[0]} — "
              f"mode={'NOOP-CONTROL' if self.noop else 'FULL'}\nfixture: {self.package} (20 synthetic episodes)")
        self.scenario("S01", "hello", self.s01)
        self.scenario("S02", "dataset: memory package -> curated examples -> group splits", self.s02)
        self.scenario("S03", "train (real): request -> progress -> typed error", self.s03)
        self.scenario("S04", "train (synthetic): mock trainer, verified checkpoint", self.s04)
        self.scenario("S05", "bad requests -> typed errors", self.s05, "failure-path")
        self.scenario("S06", "same memory -> same dataset", self.s06)
        self.scenario("S07", "tampered examples.jsonl must be caught", self.s07, "canary")
        self.scenario("S08", "stdout pollution must be caught", self.s08, "canary")
        passed = sum(r["verdict"] == "pass" for r in self.rows)
        can = [r for r in self.rows if r["kind"] == "canary"]
        rep = self.work / "lora-gate-report.json"
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
