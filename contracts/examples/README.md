# SYNTHETIC examples — the `aicore.service/1` contract (S1b)

**Every file in this folder is synthetic.** None is real audio, a real transcription, weights or a model result.
Paths (`D:/AICORE-synthetic/...`), IDs (`syn-*`) and timestamps are made up. The `sha256` values are real hashes of
the marker strings `"SYNTHETIC-EXAMPLE:<file-name>"` (not of existing files). The simulator marks everything it
produces `synthetic: true` and `validation: "unreviewed"`.

S1b: the examples live **only** on the JSON-lines subprocess transport. S1's HTTP state documents
(`midi-task.queued/running/done/failed/cancelled.json`, `cancel.request/response.json`, `jsonl-cancelled.json`) left
with the contract — they were tied to T1, which is deferred (`CONTRACT.md` §13). The error case did **not** leave: it
now lives as the `midi-task.failed.jsonl` transcript.

The gate (`sim/gate.py`) validates **each example** against its schema — an example that does not validate is red,
not skip. The table below is generated from the folder, not copied: the gate fails if a file appears without a
mapping.

| File | Schema | What it proves |
|---|---|---|
| `hello.response.json` | `service-hello` | capabilities: `ppq` 960, grid down to `1/32`, `authRequired` false, one profile |
| `midi-task.request.json` | `service-task-request` | happy request: PPQ **960**, grid `1/16`, one `mix` input |
| `midi-task.roll32.request.json` | `service-task-request` | roll request: grid **`1/32`** (120 ticks at 960) |
| `jsonl-transcript.jsonl` | `service-event` (each line) | `start` → 5× `progress` (monotonic fraction) → `done` with outputs at PPQ 960 |
| `midi-task.failed.jsonl` | `service-event` (each line) | **typed error on the happy path**: `start` → 2× `progress` → `error` `BACKEND_FAILED` (`retryable` false), no `done`, no outputs. Replaces S1's `midi-task.failed.json` (which was an HTTP state) |
| `render.request.json` | `service-task-request` | **render** profile (S5): a PPQ 960 `.mid` → one VST3 instrument → `render.wav`, offline |
| `jsonl-error.json` | `service-event` (`error` line) | terminal error line with a typed code + `retryable` |
| `error.response.json` | `error` | a lone error object (`BROKEN_REFERENCE`) |
| `task-manifest.json` | `task-manifest` | runtime manifest: sha256 per file, `format.ppq` **960**, pipeline metrics |
| `lora-dataset.request.json` | `service-task-request` | `lora`/`dataset` profile: memory package (C04) → dataset with splits; synthetic sources only with `synthetic: true` |
| `lora-train.request.json` | `service-task-request` | `lora`/`train` profile: manifest + examples + recipe |
| `lora-train.failed.jsonl` | `service-event` (each line) | **typed error on the training happy path**: `start` → `read` → `train` (plan) → `error` `BACKEND_FAILED` (real trainer still to wire), no outputs |
