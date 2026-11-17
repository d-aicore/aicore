"""Synthetic Memory package (C04 ZIP) for the lora service gate.

`fixtures/elastic-bass.events.jsonl` is a byte copy of the AICORE test fixture
(`D:\\AIcore-new\\tests\\studio\\fixtures`, @500bec8, sha256 00a3ce43...): one synthetic episode of 12 events.
Like AICORE's `tests/studio/test_dataset.py::episode`, each copy gets its own ids and a distinct text, so
`plan_dataset` sees N independent families. The package says `synthetic: true`: nothing here was said by Diogo.

  python make_fixture.py OUT_DIR [--episodes 20] [--package-id syn-lora-gate]

Writes OUT_DIR/<package-id>.zip and prints its path. Needs `aicore_contracts` (AICORE_PY_SRC, as the service).
"""
from __future__ import annotations

import argparse
import copy
import json
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
AICORE_SRC = Path(os.environ.get("AICORE_PY_SRC", r"D:\AICore\app"))
sys.path.insert(0, str(AICORE_SRC / "packages" / "contracts" / "src"))
from aicore_contracts.package import jsonl, write_package  # noqa: E402

EVENTS = [json.loads(line) for line in (HERE / "fixtures" / "elastic-bass.events.jsonl").read_text(
    encoding="utf-8").splitlines() if line.strip()]
WORDS = ["kick", "hats", "pad", "lead", "snare", "riser", "sub", "arp", "drone", "clap", "tom", "fx", "vox",
         "perc", "bell", "pluck", "acid", "growl", "stab", "noise", "chord", "keys", "flute", "sitar"]
POLICY = {"schemaVersion": "1.0", "policyId": "syn-policy", "version": 1, "grantor": "syn-local-owner",
          "subjectRefs": ["syn-episode"], "allowedPurposes": ["store", "retrieval", "memory_export", "curation",
                                                               "training"],
          "allowedDestinations": ["local:aicore", "local:aicore-studio"], "validFrom": "2026-11-04T10:00:00Z",
          "validUntil": None, "offlineMode": "snapshot_allowed", "maxProofAgeSeconds": None,
          "knownRevocationEpoch": 1, "status": "active"}


def episode(i: int) -> list[dict]:
    word = WORDS[i % len(WORDS)] + str(i)
    out = []
    for ev in copy.deepcopy(EVENTS):
        ev["eventId"] = f"e{i}-{ev['eventId']}"
        ev["episodeId"] = f"ep-{i}"
        if ev["attemptId"]:
            ev["attemptId"] = f"{ev['attemptId']}-{i}"
        for ref in ev["causalRefs"]:
            ref["eventId"] = f"e{i}-{ref['eventId']}"
        p = ev["payload"]
        if "toolCallId" in p:
            p["toolCallId"] = f"{p['toolCallId']}-{i}"
        if ev["kind"] == "feedback" and p.get("targetAttemptId"):
            p["targetAttemptId"] = f"{p['targetAttemptId']}-{i}"
        if ev["kind"] in ("request", "feedback", "assistant_message"):
            p["text"] = f"{p['text']} sobre {word} variante {word} numero {i * 7919}"
        out.append(ev)
    return out


PACKAGE_ID = re.compile(r"[A-Za-z0-9._:-]{1,128}")


def package_path(out_dir: Path, package_id: str) -> Path:
    """OUT_DIR/<package-id>.zip, and never anywhere else (audit 2026-11-16: '../x' or 'C:/x' escaped OUT_DIR)."""
    if not PACKAGE_ID.fullmatch(package_id) or package_id in (".", ".."):
        raise ValueError(f"package-id must match [A-Za-z0-9._:-]{{1,128}}: {package_id!r}")
    root = Path(out_dir).resolve()
    path = (root / f"{package_id}.zip").resolve()
    if path.parent != root:
        raise ValueError(f"package-id leaves the output folder: {package_id!r}")
    return path


def build(out_dir: Path, episodes: int, package_id: str) -> Path:
    path = package_path(out_dir, package_id)
    events =[ev for i in range(episodes) for ev in episode(i)]
    manifest = {"schemaVersion": "1.0", "packageId": package_id, "synthetic": True,
                "createdAt": "2026-11-16T00:00:00Z",
                "producer": {"id": "aicore-memory", "name": "AICORE Memory", "version": "0.1.0"},
                "snapshot": {"storeGeneration": "syn-store", "revision": 1, "policyEpoch": 1,
                             "capturedThroughSeq": len(events)},
                "scope": {"profileId": "syn-profile", "projectId": "syn-project", "sessionId": None},
                "declaredOriginAssurance": "recorder_attested", "policies": ["syn-policy"],
                "counts": {"events": len(events)}, "lineageRoots": ["syn-store"], "redactions": [],
                "exportPurpose": "curation", "limitations": []}
    write_package(path, manifest, {"events.jsonl": jsonl(events), "policies.jsonl": jsonl([POLICY]),
                                   "revocations.jsonl": jsonl([])})
    return path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out_dir")
    ap.add_argument("--episodes", type=int, default=20)
    ap.add_argument("--package-id", default="syn-lora-gate")
    a = ap.parse_args()
    print(build(Path(a.out_dir), a.episodes, a.package_id))
    return 0


if __name__ == "__main__":
    sys.exit(main())
