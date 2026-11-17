# contracts/ — AICore-C's JUCE↔Python contract (S1, revised in S1b)

The local `aicore.service/1` contract between the native JUCE/C++ app (client) and the Python services (legacy ACE,
audio→MIDI S2, stems, Flamingo, LoRA). **Spec: [CONTRACT.md](CONTRACT.md).**

**S1b (2026-11-16):** one single transport (a JSON-lines subprocess), `.mid` at **PPQ 960**, grid down to **1/32**,
and the happy path trimmed of idempotency/reconciliation/heartbeat/cancel/watchdog/version negotiation — the full
list with reasons in [CONTRACT.md §13](CONTRACT.md). Integrity was **not** trimmed: atomic writes, sha256 per file and
a fail-closed validator stay mandatory.

- `schemas/v1/` — 9 JSON schemas draft 2020-12: 2 vendored byte-identical from AICORE v1 (`common`, `error`)
  + 7 of this slice. Namespace `https://aicore.local/schemas/v1/`. Left with the deferred transport:
  `service-task-status`, `service-progress`, `service-cancel-request`, `service-cancel-response`.
- `examples/` — **synthetic** request/transcript/error/manifest (see the README there); the gate validates them all.
- `sim/` — a stdlib-only CPU simulator + a gate with proof of failure (see [sim/README.md](sim/README.md)).
- `manifest.json` — sha256 of each contract file (generated/checked by `sim/make_manifest.py`).
- `evidence/` — captured output of the gate runs (proof, not generated at runtime).
- `cli.md` — the native app's CLI contract (`--version` / `--self-test` / `--grab`), slice S0.

The native app **never** imports torch/CUDA, never embeds models, never runs inference in the UI process (see
`AGENTS.md` → "ML boundaries"). The contracts/schemas are reused from the AICORE repo, not reinvented.

Quick gate (Python 3.10+, stdlib, no GPU/models/network):

```
python contracts/sim/make_manifest.py verify
python contracts/sim/gate.py            # green: full matrix + canaries rejected
python contracts/sim/gate.py --noop     # RED: the no-op control proves the gate can fail
```

S1 state: the contract is defined and exercised against the simulator. The real MIDI service (S2) and the render
profile (S5) speak it; the native ACE client (S3) and an independent `jsonschema` cross-check are still to
implement/measure — see CONTRACT.md §12.
