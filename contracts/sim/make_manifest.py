"""Generate / verify contracts/manifest.json — sha256 of every contract file.

Model: ACE adapter-manifest files[] ({path, bytes, sha256}) and the AICORE
package manifests. The manifest itself and generated evidence are excluded.
Verification is fail-closed: a missing file or a moved hash is RED, never skip.

  python make_manifest.py generate            # writes contracts/manifest.json
  python make_manifest.py verify [--dir D]    # exit 0 ok / 1 red (D for canary copies)
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import media
from jsonschema_mini import Registry, validation_errors

CONTRACTS = Path(__file__).resolve().parent.parent
SCHEMA_DIR = CONTRACTS / "schemas" / "v1"
VENDORED_FROM = {"repo": "AICORE (D:/AIcore-new, READ-ONLY)",
                 "commit": "500bec884404ed4745c3cef1e92e4aa4360ee578"}
VENDORED_NAMES = ["common.schema.json", "error.schema.json"]


def now():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def covered_files(root):
    """Deterministic list of contract deliverable files, relative to contracts/."""
    out = []
    # every Markdown file at the contract root (CONTRACT.md, README.md, cli.md, ...): a new doc is covered
    # without editing a list (audit 2026-11-16: cli.md was left out)
    out += [p.name for p in sorted(root.glob("*.md")) if p.is_file()]
    out += [f"schemas/v1/{p.name}" for p in sorted((root / "schemas" / "v1").glob("*.schema.json"))]
    out += [f"examples/{p.name}" for p in sorted((root / "examples").iterdir()) if p.is_file()]
    out += [f"sim/{p.name}" for p in sorted((root / "sim").glob("*.py"))]
    if (root / "sim" / "README.md").is_file():
        out.append("sim/README.md")
    return sorted(out)


def counts_of(root):
    return {"schemas": len(list((root / "schemas" / "v1").glob("*.schema.json"))),
            "examples": len([p for p in (root / "examples").iterdir() if p.is_file()]),
            "sim": len(list((root / "sim").glob("*.py")))}


def build(root, base_commit):
    files = []
    for rel in covered_files(root):
        p = root / rel
        files.append({"path": rel, "bytes": p.stat().st_size, "sha256": media.sha256_file(p)})
    vendored = [{"name": n, "sha256": media.sha256_file(root / "schemas" / "v1" / n)} for n in VENDORED_NAMES]
    counts = counts_of(root)
    return {"schemaVersion": "1.0", "contract": "aicore.service/1", "generatedAt": now(),
            "baseCommit": base_commit, "vendoredFrom": {**VENDORED_FROM, "files": vendored},
            "files": files, "counts": counts}


def verify(root):
    """Return list of errors (empty = ok). Missing file / moved hash / drift = red."""
    errors = []
    mpath = root / "manifest.json"
    if not mpath.is_file():
        return [f"manifest missing: {mpath}"]
    try:
        doc = json.loads(mpath.read_text(encoding="utf-8"))
    except ValueError as e:
        return [f"manifest not JSON: {e}"]
    reg = Registry(SCHEMA_DIR)
    errors += [f"manifest schema: {e}" for e in validation_errors(reg, "contract-manifest", doc)]
    if errors:
        return errors
    listed = set()
    for entry in doc["files"]:
        rel = entry["path"]
        listed.add(rel)
        p = root / rel
        if not p.is_file():
            errors.append(f"MISSING file (red, not skip): {rel}")
            continue
        if p.stat().st_size != entry["bytes"]:
            errors.append(f"BYTES moved: {rel} {p.stat().st_size} != {entry['bytes']}")
        actual = media.sha256_file(p)
        if actual != entry["sha256"]:
            errors.append(f"HASH moved: {rel} {actual} != {entry['sha256']}")
    for rel in covered_files(root):
        if rel not in listed:
            errors.append(f"DRIFT unlisted contract file: {rel}")
    if doc.get("counts") != counts_of(root):
        errors.append(f"COUNTS moved: manifest {doc.get('counts')} != on disk {counts_of(root)}")
    for v in doc["vendoredFrom"]["files"]:
        p = root / "schemas" / "v1" / v["name"]
        if p.is_file() and media.sha256_file(p) != v["sha256"]:
            errors.append(f"VENDORED hash moved: {v['name']}")
    return errors


def main(argv=None):
    ap = argparse.ArgumentParser(description="contract manifest sha256")
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("generate")
    g.add_argument("--base-commit", default="")
    v = sub.add_parser("verify")
    v.add_argument("--dir", default=str(CONTRACTS))
    args = ap.parse_args(argv)
    if args.cmd == "generate":
        base = args.base_commit
        if not base:
            base = subprocess.run(["git", "-C", str(CONTRACTS.parent), "rev-parse", "HEAD"],
                                  capture_output=True, text=True, check=True).stdout.strip()
        doc = build(CONTRACTS, base)
        reg = Registry(SCHEMA_DIR)
        errs = validation_errors(reg, "contract-manifest", doc)
        if errs:
            print("manifest invalid:", errs)
            return 1
        media.atomic_write(CONTRACTS / "manifest.json",
                           json.dumps(doc, ensure_ascii=True, indent=2).encode() + b"\n",
                           CONTRACTS / ".staging-manifest")
        print(f"manifest.json written: {len(doc['files'])} files, baseCommit {base}")
        return 0
    errors = verify(Path(args.dir))
    if errors:
        print(f"MANIFEST RED ({len(errors)} problem(s)):")
        for e in errors:
            print("  -", e)
        return 1
    print("MANIFEST OK: every listed file present, every sha256 matches, no drift")
    return 0


if __name__ == "__main__":
    sys.exit(main())