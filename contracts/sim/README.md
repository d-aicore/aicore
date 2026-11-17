# sim/ — versioned simulator + contract gate (S1b)

**CPU, Python 3.10+ stdlib only. No models, no GPU, no weights, no external network, no installed deps.** The
simulator exercises `aicore.service/1` (CONTRACT.md) with a deterministic stand-in of the MIDI service (S2): onsets by
envelope + a fixed synthetic scale → a real SMF1 `.mid` at **PPQ 960**. It is not a transcriber; everything it makes
is marked `synthetic: true`, `validation: "unreviewed"`.

S1b (owner decision, 2026-11-16): **one** transport (a JSON-lines subprocess), PPQ **960**, grid down to **1/32**,
happy path `request → progress → .mid → typed error`. HTTP, cancel, parent-pid, idempotency, heartbeat and version
negotiation are **deferred** (CONTRACT.md §13), not deleted.

## Files

- `service_sim.py` — `hello` (one capabilities line), `job` (one request line on stdin →
  `start`/`progress`…/`done`|`error` on stdout, exit 0/2) and `fixture` (a synthetic WAV with rolls/short notes).
- `media.py` — WAV PCM16/24/32+float32/64 (core_stems-style reading), SMF1 MIDI at **PPQ 960** written by hand and
  read back (refuses ppq ≠ 960 at both ends), grid `1/1`…`1/32`/`off`, sha256, atomic staging + `os.replace` writes.
- `jsonschema_mini.py` — a **fail-closed** draft 2020-12 validator: an unknown keyword → exception, never ignore.
  Correctness proven by mutations in the gate (S17), not by trust.
- `gate.py` — a matrix of 19 scenarios (4 canaries) + no-op control; records every stdout/stderr line.
- `make_manifest.py` — generates/checks `contracts/manifest.json` (sha256 per file; missing/moved → red).

## Exact commands (from the repo root)

```
python contracts/sim/make_manifest.py generate     # (re)generates manifest.json
python contracts/sim/make_manifest.py verify       # exit 0 ok / 1 red
python contracts/sim/gate.py                       # full matrix → exit 0 green
python contracts/sim/gate.py --noop                # no-op control → exit 1 RED (mandatory proof)
python contracts/sim/gate.py --keep                # keeps the temporary workdir for inspection
```

## What the gate proves

- **Happy path (positives):** hello (`ppq` 960, grid down to 1/32, no auth); happy job → PPQ 960 `.mid` + stems +
  sha256 manifest, read/separate/transcribe/quantize/write phases, strict `eventSeq`, monotonic `fraction`, exactly
  one terminal line, coherent exit; **typed error after progress** (`BACKEND_FAILED`, `OUT_OF_MEMORY`: `start` →
  `progress` → `error`, exit 2, no outputs, clean staging); a 1/64 roll quantised to 1/32 without losing any event or
  collapsing (tick, pitch); notes shorter than the grid stay short; PPQ 480 refused by the writer and the reader;
  determinism + content moves the sha; the client refuses a wrong contract major before submitting; every example
  validates (none without a mapping); §13 still documents each deferred item.
- **Typed failures (the job goes red, never skip or a fake success):** missing reference → `BROKEN_REFERENCE`; wrong
  sha → `HASH_MISMATCH`; limits/format → `SCHEMA_INVALID`/`UNSUPPORTED_FORMAT`/`LIMIT_EXCEEDED`; invalid schema,
  ppq 480, grid 1/64 and the S1 fields that left (`cancelToken`, `cancelFile`, `parentPid`, `deadlineSeconds`) →
  `SCHEMA_INVALID`; empty/non-JSON stdin → `INVALID_JSON`.
- **Canaries (the gate MUST reject them):** `fake-success` (done without a .mid); `wrong-sha` (done with a sha256 that
  does not match); a tampered manifest / deleted file; 13 schema mutations + an unknown keyword raises (fail-closed).
- **No-op control (`--noop`):** checks replaced by no-ops → the canaries are no longer rejected → the gate exits
  **red (exit 1)**. Proof that the gate can fail (AGENTS gate 5). The `--noop` exit 1 is the EXPECTED result, not a
  regression.

## S1 → S1b map (nothing leaves in silence)

| S1 | S1b |
|---|---|
| S01 HTTP hello | S01 hello (subprocess) |
| S02 happy job T1 / S15 happy T2 | S02 |
| S09 BACKEND_FAILED / S10 OOM | S03 / S04 (now after progress, part of the happy path) |
| S05 missing ref / S18 T2 missing ref | S07 |
| S06 hash / S07 limits / S08 schema | S08 / S09 / S10 (+ S1 fields refused) |
| S11 fake-success canary | S12 |
| S13 contract refusal | S14 |
| S03 idempotency ("content moves the sha") | S15 determinism + sha only; idempotency deferred (§13) |
| S19 manifest / S20 mutations / S21 examples | S16 / S17 / S18 |
| S04 T1 cancel, S12 timeout+reconcile, S14 ignore-cancel canary, S16 cancel-file, S17 parent-pid | **deferred** (§13); S19 proves they stay documented; the `wrong-sha` canary (S13) takes the place of `ignore-cancel` |
| — | new: S05 1/32 roll, S06 short notes, S11 PPQ 480 refused |

## Faults (test hooks, never a real service behaviour)

`backend-failed`, `oom`, `fake-success`, `wrong-sha` (job) and `bad-contract` (hello/job) — through `--fault` or the
`AICORE_SIM_FAULT` env. Documented in CONTRACT.md §11.

## Evidence

Captured output of the real runs: `contracts/evidence/gate-run.log`. The full `gate-report.json` (with every line of
every process) is made in each run's temporary workdir; use `--keep` to keep it.
