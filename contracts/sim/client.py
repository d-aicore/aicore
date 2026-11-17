"""Reference client of `aicore.service/1` (CONTRACT.md §3/§9): hello first, submit only when it fits.

The gate runs THIS code against the simulator (S14), so the client rule "refuse before submitting" is
executed, not imitated inside the gate (audit 2026-11-16). Stdlib only.

  submit(argv_for, request) -> {"submitted": bool, "reason": str, "exit": int|None, "docs": [...]}

`argv_for(sub)` returns the command line of the service for "hello" or "job".
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from jsonschema_mini import Registry, validation_errors  # noqa: E402

CONTRACT = "aicore.service/1"
SCHEMA_DIR = Path(__file__).resolve().parent.parent / "schemas" / "v1"


def _lines(stdout):
    return [json.loads(ln) for ln in stdout.splitlines() if ln.strip()]


def submit(argv_for, request, timeout=600):
    reg = Registry(SCHEMA_DIR)
    p = subprocess.run(argv_for("hello"), capture_output=True, text=True, encoding="utf-8", errors="replace",
                       timeout=60)
    try:
        hello = _lines(p.stdout)
    except ValueError:
        return {"submitted": False, "reason": "hello printed a non-JSON line", "exit": None, "docs": []}
    if p.returncode != 0 or len(hello) != 1:
        return {"submitted": False, "reason": f"hello exit {p.returncode}, {len(hello)} line(s)", "exit": None, "docs": []}
    h = hello[0]
    if h.get("contract") != CONTRACT or h.get("schemaVersion") != "1.0":
        return {"submitted": False, "reason": f"service speaks {h.get('contract')!r}, client {CONTRACT!r}",
                "exit": None, "docs": []}
    if validation_errors(reg, "service-hello", h):
        return {"submitted": False, "reason": "hello does not validate", "exit": None, "docs": []}
    offered = {(pr["name"], op) for pr in h["profiles"] for op in pr["operations"]}
    if (request.get("profile"), request.get("operation")) not in offered:
        return {"submitted": False, "reason": "profile/operation not announced", "exit": None, "docs": []}
    errs = validation_errors(reg, "service-task-request", request)
    if errs:
        return {"submitted": False, "reason": f"request does not validate: {errs[:3]}", "exit": None, "docs": []}
    p = subprocess.run(argv_for("job"), input=json.dumps(request, ensure_ascii=True) + "\n", capture_output=True,
                       text=True, encoding="utf-8", errors="replace", timeout=timeout)
    return {"submitted": True, "reason": "", "exit": p.returncode, "docs": _lines(p.stdout)}
