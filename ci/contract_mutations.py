"""Proof that each new contract check fails when its fix is undone (Diogo's CI rule).

For each mutation: copy contracts/ to a temp dir, undo one fix, regenerate the manifest there, run
gate.py and expect the named scenario to go RED. Prints one line per mutation; exit 1 if any survives.
"""
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SRC = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "contracts"

MUTS = [
    ("WAV cap back to 8 MiB", "sim/media.py", "MAX_WAV_BYTES = 128 * 1024 * 1024", "MAX_WAV_BYTES = 8 * 1024 * 1024", "S22"),
    ("no rollback on failure", "sim/service_sim.py", "                final.unlink()", "                pass", "S22"),
    ("outputDir reuse allowed", "sim/service_sim.py", "    if taken:\n", "    if False:\n", "S22"),
    ("only the mix is hashed", "sim/service_sim.py", '        if "sha256" in ref and actual != ref["sha256"]:',
     '        if ref["role"] == "mix" and "sha256" in ref and actual != ref["sha256"]:', "S23"),
    ("truncated chunk accepted", "sim/media.py", "        if len(body) != size:\n            raise MediaError(\"UNSUPPORTED_FORMAT\",",
     "        if False:\n            raise MediaError(\"UNSUPPORTED_FORMAT\",", "S23"),
    ("invalid id echoed", "sim/service_sim.py", '        return value\n    return "unknown"', '        return value\n    return value', "S23"),
    ("NaN parsed", "sim/service_sim.py", "request = media.loads_request(raw.decode(\"utf-8\"))", "request = json.loads(raw.decode(\"utf-8\"))", "S23"),
    ("validator accepts NaN (type check)", "sim/jsonschema_mini.py", "    return not isinstance(v, float) or math.isfinite(v)", "    return True", "S17"),
    ("validator refuses 36.0", "sim/jsonschema_mini.py", "            return math.isfinite(value) and value.is_integer()", "            return False", "S17"),
    ("no manifestSha256", "sim/service_sim.py", '"manifestSha256": media.sha256_bytes(manifest_data), ', "", "S21"),
    ("manifest skips cli.md", "sim/make_manifest.py", 'out += [p.name for p in sorted(root.glob("*.md")) if p.is_file()]',
     'out += [n for n in ("CONTRACT.md", "README.md") if (root / n).is_file()]', "S16"),
    ("render audio mode refused", "schemas/v1/service-task-request.schema.json", '"const": "audio"', '"const": "audiox"', "S20"),
    ("client submits to any contract", "sim/client.py",
     ('if h.get("contract") != CONTRACT or', 'if validation_errors(reg, "service-hello", h):'),
     ("if False and", "if False:"), "S14"),
    ("seed has no 32-bit cap", "schemas/v1/midi-params.schema.json", '"maximum": 4294967295,', "", "S17"),
    ("render audio takes a plugin", "schemas/v1/service-task-request.schema.json", '"plugin": false,', '"plugin": true,', "S20"),
]

survivors = 0
for label, rel, old, new, sid in MUTS:
    work = Path(tempfile.mkdtemp(prefix="aicore-mut-"))
    dst = work / "contracts"
    shutil.copytree(SRC, dst, ignore=shutil.ignore_patterns("__pycache__", "evidence"))
    f = dst / rel
    text = f.read_text(encoding="utf-8")
    olds, news = (old, new) if isinstance(old, tuple) else ((old,), (new,))  # several edits = one mutation
    bad = [o for o in olds if text.count(o) != 1]
    if bad:
        print(f"SETUP  {label}: pattern not found exactly once: {bad}")
        survivors += 1
        continue
    for o, n in zip(olds, news):
        text = text.replace(o, n)
    f.write_text(text, encoding="utf-8", newline="\n")
    subprocess.run([sys.executable, str(dst / "sim" / "make_manifest.py"), "generate", "--base-commit", "0" * 40],
                   capture_output=True, text=True)
    p = subprocess.run([sys.executable, str(dst / "sim" / "gate.py")], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=900)
    red = [ln for ln in p.stdout.splitlines() if ln.startswith("[RED]")]
    hit = any(re.match(rf"\[RED\] {sid}\b", ln) for ln in red)
    print(f"{'KILLED' if hit and p.returncode else 'SURVIVED'} {label}: exit {p.returncode}, red {[ln.split()[1] for ln in red]}")
    survivors += 0 if (hit and p.returncode) else 1
    shutil.rmtree(work, ignore_errors=True)
print(f"{len(MUTS) - survivors}/{len(MUTS)} mutations killed")
sys.exit(1 if survivors else 0)
