"""AICore-C — memory -> dataset -> LoRA service, contract `aicore.service/1` (profile `lora`).

One transport (CONTRACT.md §2): one JSON request line on stdin, one JSON object per line on stdout
(`start` -> `progress`* -> `done` | `error`), logs on stderr, exit 0 only after `done`, 2 after `error`.

  python aicore_lora_service.py hello
  python aicore_lora_service.py job  < request.jsonl

Operations (CONTRACT.md §5c):

- `dataset`: Memory packages (C04 ZIP, role `memory_package`) -> a throwaway Studio project in staging
  -> one curation example per episode (target = the episode's last assistant message) -> review by the
  person named in the request -> `plan_dataset` (dedup, families, group splits) -> `dataset-manifest.json`,
  `examples.jsonl`, `report.json`, `manifest.json`.
- `train`: `dataset-manifest.json` + `examples.jsonl` + a C06 recipe -> the step plan of the worker. The
  real QLoRA trainer is NOT connected yet (the AICORE worker only has `MockTrainer`): a real request ends in
  `BACKEND_FAILED` after it has read and planned, never in a fake adapter. A `synthetic: true` request runs
  `MockTrainer` end to end (checkpoints written and verified in staging) and publishes `train-report.json`
  marked synthetic, to prove the plumbing.

The logic is the AICORE Python code, imported, not copied (READ-ONLY source, AGENTS.md): `aicore_contracts`,
`aicore_studio.project`/`dataset` and `aicore_worker.train`. `AICORE_PY_SRC` (default `D:\\AICore\\app`,
identical to `D:\\AIcore-new` @ 500bec8) says where; the sha256 of every imported module file goes into the
report so a result says which code made it. CPU only, BELOW_NORMAL priority, no downloads, no GPU.
Everything published is `validation: "unreviewed"`.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
SIM = REPO / "contracts" / "sim"
sys.path.insert(0, str(SIM))
import media  # contract helpers: sha256_file
from jsonschema_mini import Registry, validation_errors

AICORE_SRC = Path(os.environ.get("AICORE_PY_SRC", r"D:\AICore\app"))
for _sub in ("packages/contracts/src", "apps/studio/src", "workers/trainer/src"):
    _p = AICORE_SRC / _sub
    if _p.is_dir() and str(_p) not in sys.path:
        sys.path.insert(0, str(_p))

CONTRACT = "aicore.service/1"
SERVICE = {"name": "aicore-lora", "version": "lora-0.1.0"}
SCHEMA_DIR = REPO / "contracts" / "schemas" / "v1"
MAX_PACKAGE_BYTES = 128 * 1024 * 1024
MAX_JSON_BYTES = 2 * 1024 * 1024
MAX_JSONL_BYTES = 128 * 1024 * 1024
LIMITS = {"maxInputBytes": MAX_PACKAGE_BYTES, "maxOutputBytes": MAX_JSONL_BYTES,
          "maxDurationSeconds": 600, "maxResponseBytes": 2 * 1024 * 1024}
RETRYABLE = {"WORKER_UNAVAILABLE"}
OPERATIONS = ("dataset", "train")
ID_RE = re.compile(r"^[A-Za-z0-9._:-]{1,128}$")
BELOW_NORMAL = 0x00004000
TRAINER_MISSING = ("real QLoRA trainer not connected: the AICORE worker only has MockTrainer "
                   "(ESTADO-IMPLEMENTACAO P3); send synthetic:true to run the mock plumbing")
SOURCE_FILES = ("packages/contracts/src/aicore_contracts/package.py", "apps/studio/src/aicore_studio/project.py",
                "apps/studio/src/aicore_studio/dataset.py", "workers/trainer/src/aicore_worker/train.py")


def now():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def log(text):
    sys.stderr.write(f"[aicore-lora] {text}\n")
    sys.stderr.flush()


_PROTO = None


def protect_stdout():
    """stdout = protocol lines only (CONTRACT.md §2.2); anything a library prints goes to stderr."""
    global _PROTO
    sys.stdout.flush()
    fd = os.dup(1)
    os.dup2(2, 1)
    _PROTO = os.fdopen(fd, "w", encoding="ascii", newline="\n", buffering=1)
    sys.stdout = sys.stderr


def emit_line(doc):
    out = _PROTO or sys.stdout
    out.write(json.dumps(doc, ensure_ascii=True, allow_nan=False) + "\n")
    out.flush()


class ContractError(Exception):
    def __init__(self, code, message, details=None):
        super().__init__(message)
        self.code = code
        self.message = str(message)[:500]
        self.details = details or {}

    def to_dict(self, request_id):
        out = {"code": self.code, "message": self.message, "requestId": request_id,
               "retryable": self.code in RETRYABLE}
        if self.details:
            out["details"] = self.details
        return out


def from_aicore(err, phase):
    """An AICORE C00 ContractError keeps its own code (INSUFFICIENT_DATA, LEAKAGE_DETECTED, ...)."""
    details = {"phase": phase}
    extra = getattr(err, "details", None)
    if isinstance(extra, dict):
        details.update({k: v for k, v in extra.items() if k != "phase"})
    return ContractError(getattr(err, "code", "BACKEND_FAILED"), str(err), details)


def lower_priority():
    if os.name == "nt":
        try:
            import ctypes
            ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), BELOW_NORMAL)
        except Exception:  # noqa: BLE001 - best effort
            pass


def load_aicore():
    """Import the AICORE modules; a missing source is a reported backend failure, never a crash."""
    try:
        import aicore_contracts
        from aicore_studio import dataset, project
        from aicore_worker import train
    except Exception as e:  # noqa: BLE001
        raise ContractError("BACKEND_FAILED", f"AICORE Python source not importable: {type(e).__name__}: {e}",
                            {"phase": "read", "aicoreSrc": str(AICORE_SRC)})
    return aicore_contracts, project, dataset, train


def source_hashes():
    out = {}
    for rel in SOURCE_FILES:
        p = AICORE_SRC / rel
        out[rel] = media.sha256_file(p) if p.is_file() else None
    return {"root": str(AICORE_SRC), "files": out}


def canonical(doc):
    return json.dumps(doc, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def sha_bytes(data):
    return hashlib.sha256(data).hexdigest()


class Job:
    PHASES = {"dataset": ("read", "curate", "split", "write"), "train": ("read", "train", "write")}

    def __init__(self, request, task_id, checked=None):
        self.request = request
        self.checked = checked or {}
        self.task_id = task_id
        self.request_id = request["requestId"]
        self.operation = request["operation"]
        self.params = request["params"]
        self.synthetic = bool(request.get("synthetic", False))
        self.output_dir = Path(request["outputDir"])
        self.staging = self.output_dir / ".staging" / task_id
        self.seq = 0
        self.last_fraction = 0.0
        self.published = []
        self.started = time.time() - 1

    def progress(self, phase, fraction, message=None):
        fraction = max(self.last_fraction, min(1.0, fraction))
        self.last_fraction = fraction
        self.seq += 1
        doc = {"event": "progress", "schemaVersion": "1.0", "eventSeq": self.seq, "taskId": self.task_id,
               "phase": phase, "fraction": round(fraction, 4), "step": int(round(fraction * 100)),
               "totalSteps": 100, "at": now()}
        if message:
            doc["message"] = message[:200]
        emit_line(doc)

    def inputs(self, role):
        return [Path(r["path"]) for r in self.request["inputs"] if r["role"] == role]

    def input_bytes(self, role):
        """The bytes validate() hashed (one read): what is checked is what is used."""
        return self.checked[role]["data"]

    # ---- publish (staging + os.replace, CONTRACT.md §6) ----

    def publish(self, name, data, role, fmt):
        part = self.staging / f"{name}.part"
        with open(part, "wb") as fh:
            fh.write(data)
            fh.flush()
            os.fsync(fh.fileno())
        final = self.output_dir / name
        os.replace(part, final)
        self.published.append(final)
        return {"role": role, "path": str(final), "bytes": len(data), "sha256": sha_bytes(data), "format": fmt}

    def finish(self, outputs, metrics):
        for o in outputs:  # re-read what is on disk: done points at verified files only
            if media.sha256_file(Path(o["path"])) != o["sha256"]:
                raise ContractError("BACKEND_FAILED", f"published file changed: {o['path']}")
        manifest = {"schemaVersion": "1.0", "contract": CONTRACT, "taskId": self.task_id,
                    "requestId": self.request_id, "profile": "lora", "operation": self.operation,
                    "createdAt": now(), "service": dict(SERVICE), "params": self.params,
                    "inputs": [{"role": r["role"], "path": r["path"],
                                "bytes": self.checked[r["role"]]["bytes"] if r["role"] in self.checked else Path(r["path"]).stat().st_size,
                                "sha256": self.checked[r["role"]]["sha256"] if r["role"] in self.checked else media.sha256_file(Path(r["path"]))}
                               for r in self.request["inputs"]],
                    "outputs": outputs, "metrics": metrics, "validation": "unreviewed",
                    "synthetic": self.synthetic}
        data = (json.dumps(manifest, ensure_ascii=True, indent=1) + "\n").encode("ascii")
        self.publish("manifest.json", data, "manifest", {"container": "json", "bytes": len(data)})
        self.clean()
        manifest_sha, manifest_len = sha_bytes(data), len(data)
        emit_line({"event": "done", "schemaVersion": "1.0", "contract": CONTRACT, "taskId": self.task_id,
                   "requestId": self.request_id,
                   "result": {"outputs": outputs, "manifestPath": str(self.output_dir / "manifest.json"),
                              "metrics": metrics, "validation": "unreviewed",
                              "manifestSha256": manifest_sha, "manifestBytes": manifest_len},
                   "synthetic": self.synthetic, "at": now()})
        return 0

    def run(self):
        try:
            self.staging.mkdir(parents=True, exist_ok=True)
            return self.dataset() if self.operation == "dataset" else self.train()
        except ContractError as e:
            return self.fail(e)
        except MemoryError:
            return self.fail(ContractError("OUT_OF_MEMORY", "MemoryError (no retry, no offload)"))
        except Exception as e:  # noqa: BLE001 - reported, never fake success
            code = getattr(e, "code", None)
            if isinstance(code, str) and re.fullmatch(r"[A-Z][A-Z0-9_]{2,63}", code):
                return self.fail(from_aicore(e, "backend"))
            return self.fail(ContractError("BACKEND_FAILED", f"{type(e).__name__}: {e}"))

    def fail(self, err):
        try:
            for p in self.published:
                if p.is_file():
                    p.unlink()
            self.clean()
        except Exception as e:  # noqa: BLE001 - the error line must still go out
            log(f"cleanup after error failed: {e}")
        emit_line({"event": "error", "schemaVersion": "1.0", "contract": CONTRACT, "taskId": self.task_id,
                   "requestId": self.request_id, "error": err.to_dict(self.request_id), "at": now()})
        return 2

    def clean(self):
        shutil.rmtree(self.staging, ignore_errors=True)
        parent = self.staging.parent
        try:
            if parent.name == ".staging" and parent.exists() and not any(parent.iterdir()):
                parent.rmdir()
        except OSError:
            pass
        if self.staging.exists():
            raise ContractError("BACKEND_FAILED", f"could not clean staging {self.staging}")

    # ---- dataset ----

    def dataset(self):
        contracts, project_mod, dataset_mod, _ = load_aicore()
        p = self.params
        self.progress("read", 0.02, "importing memory packages")
        project = project_mod.StudioProject(self.staging / "studio.db")
        try:
            imported = []
            for path in self.inputs("memory_package"):
                try:
                    out = project.import_package(path)
                except contracts.ContractError as e:
                    raise from_aicore(e, "read")
                project.attest_package(out["packageId"], p["attestedBy"])
                imported.append(out)
            unlocked = []
            if p.get("allowSyntheticSources"):  # only with synthetic:true (checked in validate); staging DB only
                unlocked = [r[0] for r in project.db.execute("SELECT package_id FROM packages WHERE synthetic=1")]
                project.db.execute("UPDATE packages SET synthetic=0")
            self.progress("read", 0.2, f"{len(imported)} package(s), {len(project.episodes())} episode(s)")

            episodes = project.episodes()
            built, skipped = [], []
            for i, ep in enumerate(episodes, 1):
                events = project.timeline(ep["episodeId"])
                targets = [e for e in events if e["kind"] == "assistant_message"]
                if not targets:
                    skipped.append({"episodeId": ep["episodeId"], "reason": "no assistant message"})
                    continue
                ex_id = "ex-" + sha_bytes(ep["episodeId"].encode("utf-8"))[:16]
                try:
                    ex = project.build_example(ex_id, ep["episodeId"], targets[-1]["eventId"])
                    project.review(ex_id, ex["version"], "approved", p["approvedBy"])
                    built.append(ex_id)
                except contracts.ContractError as e:
                    skipped.append({"episodeId": ep["episodeId"], "reason": f"{e.code}: {e}"[:300]})
                if i % max(1, len(episodes) // 10) == 0:
                    self.progress("curate", 0.2 + 0.4 * i / max(1, len(episodes)))
            self.progress("curate", 0.6, f"{len(built)} example(s), {len(skipped)} episode(s) skipped")

            try:
                manifest = dataset_mod.plan_dataset(project, p["datasetId"], seed=p.get("seed", 20261104),
                                                    allow_small=bool(p.get("allowSmall", False)))
            except contracts.ContractError as e:
                raise from_aicore(e, "split")
            splits = {}
            for entry in manifest["examples"]:
                splits[entry["split"]] = splits.get(entry["split"], 0) + 1
            self.progress("split", 0.8, " ".join(f"{k}={v}" for k, v in sorted(splits.items())))

            split_of = {e["exampleId"]: e["split"] for e in manifest["examples"]}
            lines = []
            for entry in manifest["examples"]:
                ex = project.example(entry["exampleId"], entry["version"])
                lines.append(json.dumps({"split": split_of[ex["exampleId"]], "example": ex},
                                        ensure_ascii=False, sort_keys=True))
            examples_data = ("\n".join(lines) + "\n").encode("utf-8") if lines else b""
            manifest_data = (json.dumps(manifest, ensure_ascii=False, indent=1) + "\n").encode("utf-8")
            if len(manifest_data) > MAX_JSON_BYTES or len(examples_data) > MAX_JSONL_BYTES:
                raise ContractError("LIMIT_EXCEEDED", "dataset outputs exceed the contract caps",
                                    {"phase": "write", "manifestBytes": len(manifest_data),
                                     "examplesBytes": len(examples_data)})
            metrics = {"packages": len(imported), "episodes": len(episodes), "examplesBuilt": len(built),
                       "episodesSkipped": len(skipped), "exclusions": len(manifest["exclusions"]),
                       "splits": splits, "datasetId": manifest["datasetId"], "datasetVersion": manifest["version"],
                       "datasetHash": manifest["datasetHash"], "wallSeconds": round(time.time() - self.started, 3)}
            report = {"schemaVersion": "1.0", "operation": "dataset", "metrics": metrics, "skipped": skipped,
                      "exclusions": manifest["exclusions"], "approvedBy": p["approvedBy"],
                      "attestedBy": p["attestedBy"], "syntheticSourcesUnlocked": unlocked,
                      "aicoreSource": source_hashes(), "synthetic": self.synthetic}
            report_data = (json.dumps(report, ensure_ascii=False, indent=1) + "\n").encode("utf-8")
        finally:
            project.close()
        self.progress("write", 0.9)
        outputs = [
            self.publish("dataset-manifest.json", manifest_data, "dataset_manifest",
                         {"container": "json", "bytes": len(manifest_data)}),
            self.publish("examples.jsonl", examples_data, "examples",
                         {"container": "jsonl", "records": len(lines), "bytes": len(examples_data)}),
            self.publish("report.json", report_data, "report", {"container": "json", "bytes": len(report_data)}),
        ]
        return self.finish(outputs, metrics)

    # ---- train ----

    def train(self):
        contracts, _, _, train_mod = load_aicore()
        self.progress("read", 0.02, "reading dataset and recipe")
        manifest_bytes = self.input_bytes("dataset_manifest")
        examples_bytes = self.input_bytes("examples")
        recipe_bytes = self.input_bytes("recipe")
        try:
            manifest = media.loads_request(manifest_bytes.decode("utf-8"))
            recipe = media.loads_request(recipe_bytes.decode("utf-8"))
        except (ValueError, UnicodeDecodeError) as e:
            raise ContractError("INVALID_JSON", f"dataset manifest or recipe is not JSON: {e}", {"phase": "read"})
        try:
            contracts.validate("dataset-manifest", manifest)
            contracts.validate("recipe", recipe)
        except contracts.ContractError as e:
            raise from_aicore(e, "read")
        # the recipe names the dataset it was written for (C06 datasetRef); training it on another one is refused
        ds_refs = {manifest["datasetId"], f"{manifest['datasetId']}.v{manifest['version']}"}
        if recipe["datasetRef"] not in ds_refs:
            raise ContractError("BROKEN_REFERENCE", "the recipe was written for another dataset",
                                {"phase": "read", "datasetRef": recipe["datasetRef"], "datasetId": manifest["datasetId"],
                                 "datasetVersion": manifest["version"]})
        wanted = {(e["exampleId"], e["version"]): e["split"] for e in manifest["examples"]}
        found = {}
        try:
            example_lines = examples_bytes.decode("utf-8").splitlines()
        except UnicodeDecodeError as e:
            raise ContractError("UNSUPPORTED_FORMAT", f"examples.jsonl is not UTF-8: {e}", {"phase": "read"})
        for n, line in enumerate(example_lines, 1):
            if not line.strip():
                continue
            try:
                row = media.loads_request(line)
                ex = row["example"]
                key = (ex["exampleId"], ex["version"])
            except (ValueError, KeyError, TypeError) as e:
                raise ContractError("SCHEMA_INVALID", f"examples.jsonl line {n} is not an example row: {e}",
                                    {"phase": "read"})
            try:  # every example is a valid C05 curation example, not only its id/version
                contracts.validate("curation-example", ex)
            except contracts.ContractError as e:
                raise ContractError("SCHEMA_INVALID", f"examples.jsonl line {n}: {e}"[:500],
                                    {"phase": "read", "exampleId": ex.get("exampleId")})
            if key in found:
                raise ContractError("SCHEMA_INVALID", f"examples.jsonl line {n} repeats example {key[0]} v{key[1]}",
                                    {"phase": "read", "exampleId": key[0]})
            if key not in wanted or wanted[key] != row.get("split"):
                raise ContractError("BROKEN_REFERENCE", f"examples.jsonl line {n} is not in the dataset manifest",
                                    {"phase": "read", "exampleId": ex.get("exampleId")})
            found[key] = row["split"]
        missing = sorted(k[0] for k in wanted if k not in found)
        if missing:
            raise ContractError("BROKEN_REFERENCE", "examples listed in the manifest are missing",
                                {"phase": "read", "missing": missing[:50]})
        counts = {s: list(found.values()).count(s) for s in ("train", "validation", "test")}
        try:
            total = train_mod.plan_steps(recipe, counts["train"])
        except contracts.ContractError as e:
            raise from_aicore(e, "read")
        self.progress("train", 0.1, f"plan: {total} step(s) over {counts['train']} train example(s)")
        if not self.synthetic:
            raise ContractError("BACKEND_FAILED", TRAINER_MISSING,
                                {"phase": "train", "plannedSteps": total, "trainExamples": counts["train"]})

        # the C07 checkpoint has four fixed fingerprints; "dataset" covers BOTH the manifest and examples.jsonl
        # (audit 2026-11-16: it used to hash the manifest only, so changed example text kept the same print)
        dataset_parts = {"manifest": sha_bytes(manifest_bytes), "examples": sha_bytes(examples_bytes)}
        fingerprints = {"base": sha_bytes(f"profileRef:{recipe['profileRef']}".encode("utf-8")),
                        "dataset": sha_bytes(canonical(dataset_parts)),
                        "recipe": sha_bytes(canonical(recipe)),
                        "lock": media.sha256_file(AICORE_SRC / SOURCE_FILES[3])}
        trainer = train_mod.MockTrainer(recipe)
        store = train_mod.CheckpointStore(self.staging / "checkpoints")
        campaign, attempt = f"c-{self.task_id}", "a-1"
        losses, val, last = [], [], None
        every_ckpt = recipe.get("checkpointEverySteps") or total
        every_eval = recipe.get("evalEverySteps") or total
        for step in range(1, total + 1):
            losses.append(trainer.step()["loss"])
            if step % every_eval == 0 or step == total:
                val.append({"step": step, "loss": trainer.validation_loss()})
            if step % every_ckpt == 0 or step == total:
                last = store.write(trainer, campaign_id=campaign, attempt_id=attempt, step=step,
                                   fingerprints=fingerprints)
                if store.read(campaign, last["checkpointId"]) is None:
                    raise ContractError("BACKEND_FAILED", "checkpoint did not verify after writing",
                                        {"phase": "train", "step": step})
            self.progress("train", 0.1 + 0.8 * step / total, f"step {step}/{total}")
        metrics = {"trainer": "mock", "plannedSteps": total, "stepsDone": total, "splits": counts,
                   "firstLoss": losses[0], "lastLoss": losses[-1],
                   "lastValidationLoss": val[-1]["loss"] if val else None,
                   "checkpoints": len(store.list(campaign)), "wallSeconds": round(time.time() - self.started, 3)}
        report = {"schemaVersion": "1.0", "operation": "train", "metrics": metrics, "losses": losses,
                  "validation": val, "lastCheckpoint": last, "fingerprints": fingerprints,
                  "datasetFingerprintParts": dataset_parts,
                  "note": "MockTrainer: deterministic fake loss, no model, no adapter weights. Plumbing proof only.",
                  "aicoreSource": source_hashes(), "synthetic": True}
        data = (json.dumps(report, ensure_ascii=False, indent=1) + "\n").encode("utf-8")
        self.progress("write", 0.95)
        outputs = [self.publish("train-report.json", data, "report", {"container": "json", "bytes": len(data)})]
        return self.finish(outputs, metrics)


# ---- request validation ----

def _need(params, key, kind, op):
    if key not in params or not isinstance(params[key], kind) or isinstance(params[key], bool) and kind is not bool:
        raise ContractError("SCHEMA_INVALID", f"lora/{op}: params.{key} is required ({kind.__name__})")


def validate(reg, request):
    if request.get("schemaVersion") not in (None, "1.0"):
        raise ContractError("UNSUPPORTED_SCHEMA", "only schemaVersion 1.0 is supported")
    errors = validation_errors(reg, "service-task-request", request)
    if errors:
        raise ContractError("SCHEMA_INVALID", f"service-task-request: {len(errors)} error(s)", {"errors": errors[:20]})
    op = request["operation"]
    if request["profile"] != "lora" or op not in OPERATIONS:
        raise ContractError("UNSUPPORTED_OPERATION", f"{request['profile']}/{op} not offered")
    params = request["params"]
    roles = [r["role"] for r in request["inputs"]]
    if op == "dataset":
        allowed = {"datasetId", "approvedBy", "attestedBy", "seed", "allowSmall", "allowSyntheticSources"}
        for key in ("datasetId", "approvedBy", "attestedBy"):
            _need(params, key, str, op)
        if not ID_RE.match(params["datasetId"]):
            raise ContractError("SCHEMA_INVALID", "params.datasetId must match [A-Za-z0-9._:-]{1,128}")
        if "seed" in params:
            _need(params, "seed", int, op)
            if not 0 <= params["seed"] <= 4294967295:
                raise ContractError("SCHEMA_INVALID", "params.seed must be 0..4294967295 (32-bit)")
        for key in ("allowSmall", "allowSyntheticSources"):
            if key in params:
                _need(params, key, bool, op)
        if params.get("allowSyntheticSources") and not request.get("synthetic"):
            raise ContractError("SCHEMA_INVALID", "allowSyntheticSources needs a synthetic:true request")
        if not roles or set(roles) != {"memory_package"}:
            raise ContractError("SCHEMA_INVALID", "lora/dataset takes only inputs with role memory_package")
    else:
        allowed = set()
        if sorted(roles) != ["dataset_manifest", "examples", "recipe"]:
            raise ContractError("SCHEMA_INVALID", "lora/train takes exactly dataset_manifest, examples and recipe")
    unknown = sorted(set(params) - allowed)
    if unknown:
        raise ContractError("SCHEMA_INVALID", f"lora/{op}: unknown params {unknown}")
    out = Path(request["outputDir"])
    if not out.is_dir():
        raise ContractError("BROKEN_REFERENCE", f"outputDir does not exist: {out}", {"outputDir": str(out)})
    taken = [n for n in ("manifest.json", "dataset-manifest.json", "examples.jsonl", "report.json", "train-report.json")
             if (out / n).exists()]
    if taken:
        raise ContractError("OUTPUT_EXISTS", "outputDir already holds a published job; use a new outputDir",
                            {"existing": taken})
    checked = {}
    for ref in request["inputs"]:
        p = Path(ref["path"])
        if not p.is_file():
            raise ContractError("BROKEN_REFERENCE", f"input does not exist: {p}", {"role": ref["role"], "path": str(p)})
        size = p.stat().st_size
        cap = MAX_JSON_BYTES if ref["role"] in ("dataset_manifest", "recipe") else MAX_PACKAGE_BYTES
        if size > cap:
            raise ContractError("LIMIT_EXCEEDED", f"input is {size} bytes (cap {cap})", {"role": ref["role"]})
        if "bytes" in ref and ref["bytes"] != size:
            raise ContractError("HASH_MISMATCH", f"declared bytes {ref['bytes']} != actual {size}", {"role": ref["role"]})
        if op == "train":  # small JSON inputs: read once, hash those bytes, the job parses the same bytes
            data = p.read_bytes()
            actual = sha_bytes(data)
            checked[ref["role"]] = {"data": data, "bytes": len(data), "sha256": actual}
        else:
            actual = media.sha256_file(p) if "sha256" in ref else None
        if "sha256" in ref and actual != ref["sha256"]:
            raise ContractError("HASH_MISMATCH", f"sha256 mismatch for {p}",
                                {"role": ref["role"], "expected": ref["sha256"], "actual": actual})
        if ref["role"] == "memory_package":
            with open(p, "rb") as fh:
                if fh.read(4) != b"PK\x03\x04":
                    raise ContractError("UNSUPPORTED_FORMAT", "memory_package is not a ZIP (C04 package)",
                                        {"path": str(p)})
    return checked


def job_main():
    reg = Registry(SCHEMA_DIR)
    task_id = "t-" + uuid.uuid4().hex[:12]
    raw = sys.stdin.buffer.readline(LIMITS["maxResponseBytes"] + 1)  # the cap holds while reading

    def fail(err, rid="unknown"):
        emit_line({"event": "error", "schemaVersion": "1.0", "contract": CONTRACT, "taskId": task_id,
                   "requestId": rid, "error": err.to_dict(rid), "at": now()})
        return 2

    if not raw.strip():
        return fail(ContractError("INVALID_JSON", "no request line on stdin"))
    if len(raw) > LIMITS["maxResponseBytes"]:
        return fail(ContractError("LIMIT_EXCEEDED", "request line exceeds 2 MiB"))
    try:
        request = media.loads_request(raw.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as e:
        return fail(ContractError("INVALID_JSON", f"request line is not JSON: {e}"))
    if not isinstance(request, dict):
        return fail(ContractError("SCHEMA_INVALID", "request line must be a JSON object"))
    rid = request.get("requestId") if isinstance(request.get("requestId"), str) and \
        ID_RE.match(request.get("requestId", "")) else "unknown"
    try:
        checked = validate(reg, request)
    except ContractError as e:
        return fail(e, rid)
    except OSError as e:  # unreadable input/outputDir: still a typed JSON error, never a traceback
        return fail(ContractError("BROKEN_REFERENCE", f"{type(e).__name__}: {e}"), rid)
    job = Job(request, task_id, checked)
    emit_line({"event": "start", "schemaVersion": "1.0", "contract": CONTRACT, "taskId": task_id,
               "requestId": rid, "profile": "lora", "operation": request["operation"], "service": dict(SERVICE),
               "synthetic": bool(request.get("synthetic", False)), "at": now()})
    return job.run()


def hello():
    emit_line({"schemaVersion": "1.0", "contract": CONTRACT, "service": {**SERVICE, "pid": os.getpid()},
               "profiles": [{"name": "lora", "operations": list(OPERATIONS)}], "limits": dict(LIMITS),
               "ppq": media.PPQ, "quantizeGrid": ["1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "off"],
               "authRequired": False, "startedAt": now()})
    return 0


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description="AICore-C memory -> dataset -> LoRA service (aicore.service/1)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("hello")
    sub.add_parser("job")
    args = ap.parse_args(argv)
    protect_stdout()
    lower_priority()
    if args.cmd == "hello":
        return hello()
    return job_main()


if __name__ == "__main__":
    sys.exit(main())
