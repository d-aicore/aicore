# CONTRACT — AICore-C native app ↔ Python services (`aicore.service/1`)

Slice S1 · **trimmed for the happy path (S1b revision)** · 2026-11-16 · schemaVersion `1.0` · State:
**defined, exercised against the simulator** (see §11). This document is the binding spec of the local contract
between the JUCE/C++ app (client) and the Python services (backends). S1 wrote the contract with two transports and a
server life cycle; **S1b applies the owner's three decisions of 2026-11-16**: **one single transport** (a JSON-lines
subprocess), `.mid` at **PPQ 960** and a quantisation grid down to **1/32**. Nothing disappears in silence: what was
deferred is in **§13 "Deferred, when it is needed"**, with a reason per item. The integrity invariants (atomic writes,
sha256, fail-closed validator) were **not** trimmed.
Pattern sources: the AICORE repo `D:\AIcore-new` @ `500bec884404ed4745c3cef1e92e4aa4360ee578` (READ-ONLY) and
`D:\AICore\midi-maker\midi_maker.py` (READ-ONLY). Each claim about existing code is [V] (inspected in this checkout);
new decisions are marked as such; what is still to measure is [H].

## 0. Scope and invariants

- Single machine, private use. No browser, no webview, no web UI, no paid APIs. The audio stays on the PC.
- The native app **never** imports torch/CUDA, never embeds models, never runs inference in the UI process.
- Heavy ML (ACE, Flamingo, Demucs, LoRA) and audio→MIDI are **separable services** that speak this contract.
- **A backend failure is always reported as a typed error; never a fake success.** `done` needs files present and
  checked by sha256 (§6). A simulator/trial marks `synthetic: true` and `validation: "unreviewed"`.
- **No silent automatic retry.** Any resend is an explicit decision of the client/user (§7).
- Musical approval is human (owner). No service produces `validation: "approved"` by itself.
- **Never trimmed (integrity):** staging + `os.replace` (§6), sha256 per file in the runtime manifest (§6), a
  fail-closed schema validator — an unknown keyword raises an error, it is never ignored (§11).
- **S1b rules:** a published `.mid` is **SMF1 at PPQ 960** (the resolution of the Core DAW templates); reading or
  writing PPQ ≠ 960 breaks the contract. The grid goes down to **1/32** (120 ticks). Each note's **length** comes from
  the measured audio, never from the grid: a note shorter than the grid stays short.

## 1. Transport (S1b decision: **one single** transport)

| Service | Profile | Transport | Reused model [V] | State |
|---|---|---|---|---|
| ACE-Step v1.5 turbo (generation + ACE LoRA) | `ace` (legacy) | **its own HTTP routes, unchanged** (§10) | `ace_backend.py` | exists; do not break |
| Audio→MIDI (Demucs + Basic Pitch) | `midi` | **JSON-lines subprocess (§2)** | `core_stems.py` | S2 implements |
| Demucs stems | `stems` | JSON-lines subprocess | `core_stems.py` | exists |
| Flamingo (audio→text understanding) | `flamingo` | JSON-lines subprocess | `experiments/audio-listener` | trial; **does not transcribe** |
| Render through a plugin (`.mid` → VST3 → `.wav`) | `render` | JSON-lines subprocess (§5b) | the native app's `juce_audio_processors_headless` | S5 |
| Memory → dataset → text LoRA (Qwen) | `lora` | JSON-lines subprocess (§5c) | `aicore_studio` + `aicore_worker` | `services/lora`: real `dataset`, `train` mock only (real trainer still to wire) |

In the generic envelope there is only **one** transport: subprocess + JSON lines, the pattern already proven by
`core_stems.py` [V l.6–13, 36–46, 113–148, 381–407]. The request comes in **on stdin** as one JSON line; progress and
result go out on **stdout**; the process life cycle is the job life cycle. The versioned HTTP that S1 announced for
the `midi` profile is **deferred (§13)**: `one request → progress → .mid → error` needs no resident server, port,
polling or queue. HTTP still exists **only** in the legacy ACE profile (§10), with its own routes and **without** the
generic envelope.

Consequences the gate checks: nothing in the generic envelope depends on a port, a URL, a `taskId` queried later, or
a process that outlives the client.

## 2. Transport rules (JSON-lines subprocess)

Ported from `core_stems.py` [V l.6–13, 36–46, 381–407] and `audio-listener` [V README]:

1. **Call.** `python service_sim.py job` (the real S2 service has its own; the shape is the same). The client writes
   **exactly one** request line (§4) on stdin and closes stdin. Configuration and paths go in that line; free text
   (prompt, question) goes in a **separate UTF-8 file, never concatenated into shell commands** [V audio-listener
   README].
2. **stdout = one JSON object per line, nothing else.** Logs and diagnostics go to **stderr**. Lines in **ASCII**
   (`ensure_ascii=True`): the Windows pipe code page must not break a path with accents [V core_stems l.37–38]. The
   client parses line by line; **an invalid line = a broken contract** (§7).
3. **Events.** `start` → zero or more `progress` → **exactly one** terminal line: `done` **or** `error`
   (`service-event.schema.json`). There are no `cancelled` or `probe` in v1 (§13).
4. **Exit code.** `0` **only** after `done`; `2` after `error`; any other ending (no terminal line, an exit that does
   not match the last line) is `WORKER_UNAVAILABLE` on the client side — never success (rule §7.1).
5. **Transport limits.** Request line ≤ **2 MiB**, enforced while the line is read (never after holding an
   unbounded line in memory); `NaN`/`Infinity` are not JSON and are refused as `INVALID_JSON`; files as in §6. No chunked, binary streaming, WebSocket or stdin
   after the request.
6. **One process, one job.** Parallelism = parallel processes with different `outputDir`s; there is no queue, so there
   is no `BUSY` (§13). If the process dies halfway, the authority over what was produced is what is in `outputDir` +
   `manifest.json` — which the client checks by sha256 (§6).
7. **Resend.** After `error`/`WORKER_UNAVAILABLE` the client resends **only by explicit decision**, to a new
   `outputDir` (or discarding the previous one): v1 has no idempotency key (§13). A service refuses an `outputDir`
   that already holds a published job (`OUTPUT_EXISTS`), so a resend can never overwrite or, on failure, delete what
   an earlier job published.
8. **No auth, no listening.** `authRequired: false`; the process opens no port or socket. Credentials never in argv,
   stdout or logs.

## 3. Hello / capabilities

`python service_sim.py hello` writes **one** JSON line on stdout, schema `service-hello.schema.json`: `contract`,
`schemaVersion`, `service{name,version}`, `profiles[{name,operations}]`, `limits` (size/length/line caps),
`ppq: 960`, `quantizeGrid` (the accepted grid values), `authRequired: false`, `startedAt`. It is the **only** discovery
point: the client reads it before submitting and decides without starting processes.

- Profile/operation not announced → the client **does not submit** (`UNSUPPORTED_OPERATION` if it does anyway).
- `contract` or `schemaVersion` different from the expected → the client **stops** and shows the difference (§9).
- The hello's `ppq`/`quantizeGrid` confirm the backend writes 960; the client does not accept 480 or grids outside
  the enum (§5).
- There is no heartbeat or server `updatedAt` (§13): §4's progress is what says the job is moving.

## 4. Generic envelope

Contract: `aicore.service/1`; every document carries `schemaVersion: "1.0"` (const, `common.schema.json`).
IDs (`requestId`, `taskId`): `common.schema.json#/$defs/id` (1–128, `[A-Za-z0-9._:-]`). Timestamps: UTC `...Z`.
Paths: **absolute** (`media-format.schema.json#/$defs/absolutePath`), normalised, no control characters; the service
rejects relative paths. The `contract` is echoed in `start`, `done`, `error` and the manifest.

### Request (`service-task-request.schema.json`)

Required: `schemaVersion`, `requestId`, `profile`, `operation`, `params`, `inputs` (≥1 `fileRef` with `role` +
absolute `path`; `sha256`/`bytes` optional but **checked when present**), `outputDir`. Optional: `synthetic` (true in
simulators/trials).

Left v1 with S1b — and since `additionalProperties: false`, a request carrying any of them is rejected with
`SCHEMA_INVALID` (the gate proves it): `cancelToken`, `cancelFile`, `parentPid`, `deadlineSeconds` (reasons in §13).

- `profile: "midi"` ⇒ `operation: "transcribe"`, `params` against `midi-params.schema.json`, `inputs` with exactly one
  `role: "mix"` (WAV).
- `stems`/`flamingo` profiles: `params` is a free object in this version; their specific schemas come in their own
  slices **without breaking v1** (additional optional fields; see §9).
- `profile: "lora"` ⇒ `operation` `dataset` or `train`; `params` and `inputs` checked by the service (§5c).

### States and the terminal line

`start` (= running) → `progress`* → `done` **or** `error`. Rules checked by the gate:

- `done` ⇒ `result.outputs` ≥1, **each file exists at the declared path, matches the announced sha256 and is in
  `manifest.json`** (§6). `done` without files is impossible by construction (§7.1).
- `error` ⇒ a typed `error` object (§7), no `result`, **no final output published**, clean staging.
- There is no `queued` (no queue), `cancelled` (no cancel) or `unknown` (no resident server) — §13.
- Illegal transitions (`progress` after `done`, two terminal lines, `error` without a code) = a broken contract.

### Progress

One line per event: `eventSeq` strictly increasing per job, `phase` from the profile's enum, `fraction` 0..1
**monotonic non-decreasing**, `step`/`totalSteps` **always present**, an integer or `null` when the service does not
count steps (the schema requires the keys), `message` ≤200 optional, `at` UTC. The v1 `phase`
enum kept the phases of this contract's profiles: `read`, `separate`, `transcribe`, `quantize`, `write`,
`load`/`render` of the render profile (§5b) and `curate`/`split`/`train` of the `lora` profile (§5c), all additive.
`probe`/`evaluate`/`export` come back when a profile uses them (§13).

## 5. `midi` profile — audio→MIDI (S2 comes in here)

Pipeline (the shape of the measured baseline `midi_maker.py` [V l.27–233]; the S2 service implements it, the S1
simulator imitates the shape without models): Demucs stems → low-band kick onsets → `kickNote` → Basic Pitch on the
bass/other stems → mono bass → BPM (given/name/kick) → quantisation with the kick's phase → `splitOnOnsets` → lead
filter → `.mid` **SMF type 1 at PPQ 960** [S1b rule]. The baseline writes 480 (`PPQ = 480`, `step_ticks = PPQ // 4`
[V l.27, 216]); **the service does not inherit that value** — the Core DAW templates are written at 960 (owner's
spec), and at 480 rolls and short notes are lost in conversion.

`midi-params.schema.json` (required: `ppq`, `quantize`, `parts`, `kickNote`, `bassMono`):

| Field | Type/limits | Default (the client sends it explicitly) |
|---|---|---|
| `ppq` | const **960** | 960 (not negotiable; 480 → `SCHEMA_INVALID`) |
| `quantize` | `1/1` `1/2` `1/4` `1/8` `1/16` `1/32` `off` | `1/16` |
| `parts` | ⊆ {drums,bass,other,vocals}, ≥1, unique | `["drums","bass","other"]` |
| `kickNote` | 0–127 | 36 |
| `bassMono` | bool | true |
| `bpm` | 40–300 or null | null → detection |
| `bpmSource` | provided/filename/kick | `kick` |
| `leadFilter` | bool | true |
| `splitOnOnsets` | bool | true |
| `includeStems` | bool | false |
| `startSeconds` | 0–600 | 0 |
| `lengthSeconds` | >0–600 | whole length |
| `seed` | ≥0 | 42 |

### Grid (`quantize`) — ticks at PPQ 960

| Grid | Ticks | ms at 145 BPM | Use |
|---|---|---|---|
| `1/1` | 3840 | 413.79 | coarse marking |
| `1/2` | 1920 | 206.90 | — |
| `1/4` | 960 | 103.45 | — |
| `1/8` | 480 | 51.72 | — |
| `1/16` | 240 | 25.86 | **generic default** |
| `1/32` | **120** | 12.93 | **psytrance/hitech rolls** [S1b rule] |
| `off` | 1 | 0.43 | no quantisation (inherited from S1) |

**Placement ≠ length.** The grid decides where the note starts; the `note-off` comes from the length measured in the
audio — what `midi_maker.py` already does [V] and what S1b makes explicit. Consequences checked by the gate:

- a **1/64** roll (25.86 ms at 145 BPM) quantised to **1/32** gives two notes **on the same tick** with different
  pitches: none collapses, none is lost;
- notes measured shorter than the grid (the gate uses ~11 ms = ~25 ticks) are **not** stretched to 120/240 ticks;
- two neighbouring events are never merged: the detector has a 20 ms refractory period, below the spacing of the
  densest roll the gate feeds.

Progress phases: `read → separate → transcribe → quantize → write`. Outputs: `transcription.mid` (role `midi`),
`stem-<part>.wav` (role `stem_<part>`, only with `includeStems`), `report.json` (role `report`, pipeline metrics:
`onsetsDetected`, `notesWritten`, `bpmUsed`, `mergedEvents`, `wallSeconds`), `manifest.json`. There is no
`deadlineSeconds` in v1 (§13).
**Flamingo is not part of this profile** — understanding only, `flamingo` profile [V PLAN/R2].

## 5b. `render` profile — `.mid` → VST3 instrument → `.wav` (S5, additive)

Added on 2026-11-16 without breaking v1 (§9: new enum values + a new schema, gate run again). The native service
`aicore-render` (C++/JUCE, `juce_audio_processors_headless`, VST3 only): the same host the native app carries, with no
Python and no third plugin host (PLAN, S5 candidate).

- Request: `profile: "render"`, `operation: "render"`, **exactly one input, in one of two shapes the schema models as
  mutually exclusive** (`service-task-request.schema.json`, `oneOf`; gate S20): **MIDI mode** = one `role: "midi"`
  (SMF at **PPQ 960**, with a tempo map; another PPQ → `UNSUPPORTED_FORMAT`) played by an instrument (`plugin`,
  `role`, or `engine` `sampler`/`sfizz`/`vita`); **audio mode** = one `role: "audio"` WAV through `effects` and/or
  `effectRoles` only, with no instrument field (`plugin`, `role`, `kit`, `sfz`, `preset`, `tracks`, plugin state)
  and `engine` absent or `vst3`. Clients validate against the schema before sending; the render gate fails if the
  exe renders a request the schema rejects. `params` against `render-params.schema.json`: `plugin` (absolute
  `.vst3` path) **or** `role`, `effects`/`effectRoles` (up to 8/5), `pluginState`/`pluginStateBase64`, `sampleRate`
  44100/48000/96000, `blockSize` 32–4096, `tailSeconds` 0–30, `lengthSeconds` (null = MIDI + tail), `gainDb` -60..12.
- **AICore's own list** (owner, 2026-11-16: *"it can have the same list but not read through the Core"*; *"in the
  program's settings we put search VSTs, plugins, and associate each one"*): every plugin has to be in the list
  AICore makes itself with `aicore-render scan`, which only looks for the plugins AICore specifies in
  `supported-plugins.json` (*"it should look for the ones we specify"*; each tested in a separate process, failures in
  quarantine), in `%APPDATA%\AICore-C\plugins.json`; outside it or in quarantine → `UNSUPPORTED_OPERATION`. **Nothing
  is read from the Core.**
- **By role**: `role` (bass/lead/pad/pluck/drums) chooses the instrument and `effectRoles`
  (reverb/delay/filter/sidechain/drive) the effects from `%APPDATA%\AICore-C\roles.json`, which SETTINGS > PLUGINS
  writes (`aicore-render roles set <role> <plugin>`; an instrument only in an instrument role, an effect only in an
  effect role); a role without a plugin → `UNSUPPORTED_OPERATION`. Alternatively, `plugin` + `effects`.
- State: `pluginStateBase64` is the base64 state from `getStateInformation` (the same format the Core uses);
  `pluginState` is the same in a raw file. Instrument = the Core's rule (Instrument category, or accepts MIDI and has
  no audio input).
- **Audio input (effects only)**: instead of the `.mid`, an input `role: "audio"` (WAV at the same sample rate) goes
  only through the `effects`/`effectRoles` chain; no instrument.
- **`engine: "vita"`** (owner, 2026-11-16: AICore's synth is Vita, Vital's engine in Python): the `.mid` is played by
  the `services/vita` service (venv in `D:\AICore\plugins\vita-venv`), with an optional `.vital` `preset`, and returns
  the dry WAV. The effects come after through `aicore-render` with the audio input. `aicore-render` refuses `engine: "vita"`
  (`UNSUPPORTED_OPERATION`). Vita plays note by note: chords yes, legato and glide no [H].
- **`engine: "sampler"`**: `aicore-render`'s own sampler (not the Core's) plays `params.kit`, with one one-shot WAV per
  MIDI note (1–128). It serves the drums with labelled samples.
- **`engine: "sfizz"`**: `params.sfz` (an absolute `.sfz` kit) is played by the downloaded `sfizz_render`
  (`AICORE_SFIZZ_RENDER`); its dry WAV then goes through `effects`/`effectRoles`.
- Phases: `read` → `load` → `render` → `write`. Outputs: `render.wav` (float32 stereo, role `audio`), `report.json`,
  `manifest.json` (the same rules as §6).
- Errors: missing plugin → `BROKEN_REFERENCE`; does not load → `BACKEND_FAILED`; no MIDI input →
  `UNSUPPORTED_OPERATION`; **digital silence with notes in the input → `BACKEND_FAILED`** (a silent `.wav` is never
  published as a success); NaN/Inf → `BACKEND_FAILED`.
- Start: after `prepareToPlay` the message thread runs `AICORE_RENDER_WARMUP_MS` (1500 ms by default). Measured:
  without it, Kick 3 rendered silence (it loads its samples on the message thread).

## 5c. `lora` profile — memory → dataset → text LoRA

Service `services/lora/aicore_lora_service.py`. The logic is AICORE's Python code **imported, not copied**
(`aicore_contracts`, `aicore_studio.project`/`dataset`, `aicore_worker.train`; root `AICORE_PY_SRC`, by default
`D:\AICore\app`, equal to `D:\AIcore-new` @ 500bec8). `report.json` records the sha256 of each module used.

**`dataset`** — inputs: ≥1 `memory_package` (a C04 ZIP package exported by Memory). Params (checked by the service;
unknown → `SCHEMA_INVALID`): `datasetId` (id), `approvedBy` and `attestedBy` (who authorised; the client only sends
after the owner says yes), `seed` (int, 20261104), `allowSmall` (bool, pipeline tests), `allowSyntheticSources` (bool,
**only** with `synthetic: true`). Pipeline: a throwaway Studio project in staging → imports and attests the packages
→ one example per episode (target = the assistant's last message) → review under the given name → `plan_dataset`
(exact and near-duplicate dedup, families, 80/10/10 splits per family with a seed). Phases
`read → curate → split → write`. Outputs: `dataset-manifest.json` (role `dataset_manifest`, C05), `examples.jsonl`
(role `examples`, `{split, example}` per line), `report.json`, `manifest.json`. Synthetic sources never go into a real
dataset: without `allowSyntheticSources` they are excluded and the job ends in `INSUFFICIENT_DATA`.

**`train`** — inputs: exactly `dataset_manifest`, `examples` and `recipe` (C06). The service checks that the
recipe's `datasetRef` names this dataset (`datasetId` or `datasetId.v<version>`, else `BROKEN_REFERENCE`), that every
line of `examples.jsonl` is a valid C05 curation example, listed in the manifest with the same version and split and
not repeated (`SCHEMA_INVALID`), and computes the worker's step plan. The checkpoint fingerprints cover the
manifest, `examples.jsonl` and the recipe. **The real
QLoRA trainer is not wired yet** (AICORE's worker only has `MockTrainer`): a real request ends in `BACKEND_FAILED`
(`details.phase: "train"`) after reading and planning, never in a fake adapter. With `synthetic: true` it runs the
`MockTrainer` (deterministic fake loss, checkpoints written and checked in staging) and publishes a
`train-report.json` marked synthetic — proof of the plumbing, not of training. Phases `read → train → write`. The real
trainer gets only train/validation; test stays for the evaluation.

Profile gate: `services/lora/gate_service.py` (8 scenarios, 2 canaries, `--noop` red).

## 6. `.mid` / `.wav` files — ownership, formats, atomicity, limits

### Ownership and atomic writes (S1 decision, model `core_stems.write_wav_float` [V l.96–110])

- **Inputs belong to the client.** The service opens them **read-only**; it never writes, moves or deletes inputs.
- **`outputDir` belongs to the client** (made by it). The service writes **only** inside `outputDir`: staging in
  `outputDir/.staging/<taskId>/<name>.part`, fsync, then **`os.replace` (atomic rename)** to the final name in
  `outputDir`. A reader never sees a partial final file.
- `manifest.json` is written **last**, also atomically, after every output has been checked.
- **Who deletes:** the service cleans its `.staging/<taskId>` in any terminal state (`done`/`error`); on `error` it
  **publishes no final outputs** (no renamed `.part`). The **client** deletes `outputDir` when it discards the job
  (the Core model: "the window throws that folder away" [V core_stems l.12–13]). The service never deletes anything
  outside `outputDir`, never deletes inputs, never deletes another `taskId`'s outputs.
- Retention: the client's policy (single machine); the service has no TTL of its own over `outputDir`.

### Formats and limits

| | WAV | MIDI |
|---|---|---|
| Container | RIFF/WAVE; `fmt ` + `data`; WAVE_FORMAT_EXTENSIBLE (0xFFFE) accepted | SMF type 0 or 1 (`MThd`/`MTrk`) |
| Encoding | PCM 16/24/32, float 32/64 (tags 1/3) [V core_stems l.77–92] | — |
| Sample rate | 8000–192000 Hz; output stems at the source's SR | — |
| Channels | 1–2 (mono/stereo; >2 rejected in the midi profile) | tracks ≤64, named |
| PPQ | — | **960 ticks/quarter, const** [S1b rule] |
| Grid | — | `1/1`…`1/32`/`off` (§5); the note length comes from the audio |
| Length | 0.1–600 s (core_stems minimum [V l.308–309]; ACE maximum [V l.109–116]) | ≤600 s, tempo map mandatory |
| Size | ≤ **128 MiB** in and out, stems included (ACE cap [V l.92, 143–144]; the simulator, the hellos and `media-format` use the same number, gate S22) | ≤ **8 MiB** (S1 decision) |
| Output stems | float32 (core_stems model [V l.96–110]) | — |

Data outputs (`lora` profile): `format` `{container: "json", bytes ≤ 2 MiB}` (`reportFormat`) or
`{container: "jsonl", records, bytes ≤ 128 MiB}` (`dataFormat`, additive). One JSON line per record, UTF-8.

A chunk (WAV) or track (MIDI) that declares more bytes than the file holds is `UNSUPPORTED_FORMAT`, never read
as shorter audio. Each input is read **once**: the declared `sha256` is checked on those bytes, the same bytes are
decoded, and the manifest records their hash (no time-of-check gap). Every input with a declared `sha256` is
checked, not only the main one. Violation → `UNSUPPORTED_FORMAT` or `LIMIT_EXCEEDED`, before any heavy work. The client checks the WAV/MIDI **before**
submitting (same rules); the service checks again (defence in depth). PPQ 480 is refused at both ends: the writer
refuses to produce ppq ≠ 960 and the reader refuses to import ppq ≠ 960.

### Runtime manifest (`task-manifest.schema.json`, model `adapter-manifest.files[]` [V])

`{schemaVersion, contract, taskId, requestId, profile, operation, createdAt, service{name,version}, params,
inputs[{role,path,bytes,sha256}], outputs[{role,path,bytes,sha256,format}], metrics, validation, synthetic}`.
Optional `environment` (object): fingerprint of the producing environment (python and library versions,
threads, seed, weight sha256); the same request gives the same sha256 only within one environment.
`validation: "unreviewed"` whenever produced by a service/simulator — the ACE model `trained-unvalidated` /
`musicalValidation: "unreviewed"` [V ace_backend l.172, 248]. `approved` only by a human decision recorded by the
client.

### Why a `done` is credible

The `done` does not claim files: it **points** at them (`outputs[].path` + `bytes` + `sha256` + `format`, and
`result.manifestSha256`/`manifestBytes` for the manifest itself — optional in v1, sent by every service in this repo).
The truth comes from the client **recomputing the sha256 of each output and of `manifest.json`** before importing.
Publishing is all or nothing: outputs are staged first and moved into `outputDir` at the end, manifest last; a
failure after the first move removes what the job already moved (gate S22, `late-fail`). This is what
was not trimmed: the gate has a `fake-success` canary (a `done` line without a `.mid` on disk) and a `wrong-sha`
canary (`done` with a sha256 that does not match); if the verifier lets one through, the gate fails (§11).

## 7. Typed errors (no silent retry)

Error object: **`error.schema.json` v1 reused verbatim** (`code`, `message` ≤500, `requestId`, `retryable`,
`details` optional). C00 codes reused from `errors.py` [V]; new S1 codes marked ✚. **There are no HTTP status codes in
this contract** — the HTTP column left with the deferred transport (§13); `code` + `retryable` reach the client.
`retryable: true` **only** in `WORKER_UNAVAILABLE` (the only retryable C00 [V l.44] that survives in v1) — and even
there the resend is **explicit**, never automatic.

| Code | retryable | Origin | Meaning / client action |
|---|---|---|---|
| `SCHEMA_INVALID` | false | C00 | the request (or a stdout line) does not validate against the schema; fix it, do not resend the same |
| `UNSUPPORTED_SCHEMA` | false | C00 | `schemaVersion` ≠ 1.0 |
| `INVALID_JSON` | false | C00 | stdin/line is not JSON |
| `LIMIT_EXCEEDED` | false | C00 | size/length/line above the §6 caps |
| `BROKEN_REFERENCE` | false | C00 | **a referenced input does not exist / cannot be read** → red, never skip |
| `HASH_MISMATCH` | false | C00 | the input's sha256/bytes do not match the declared ones |
| `UNSUPPORTED_OPERATION` ✚ | false | S1 | operation/profile not announced in the hello (§3) |
| `UNSUPPORTED_FORMAT` ✚ | false | S1 | WAV/MIDI outside the §6 formats (includes a `.mid` at ppq ≠ 960) |
| `BACKEND_FAILED` ✚ | false | S1 | **a backend failure reported, never a fake success**; details with a short cause |
| `OUT_OF_MEMORY` ✚ | false | S1 | CPU/GPU OOM; **no retries, no automatic offload** [V audio-listener]; report and stop |
| `OUTPUT_EXISTS` ✚ | false | 2026-11-16 | `outputDir` already holds a published job (§2.7); resend to a new `outputDir` |
| `WORKER_UNAVAILABLE` | **true** | C00 | the process did not start or ended without a matching terminal line (§2.4); resend **explicitly** |
| `INSUFFICIENT_DATA` | false | C00 (lora) | too few eligible examples for validation/test splits; collect more memory, do not force |
| `LEAKAGE_DETECTED` | false | C00 (lora) | a family mixes examples of different frozen splits; needs a new test record |

Deferred with the server life cycle (reasons in §13): `UNAUTHENTICATED`, `SCOPE_DENIED`, `NOT_FOUND`,
`IDEMPOTENCY_CONFLICT`, `VERSION_CONFLICT`, `BUSY`, `TASK_TIMEOUT`, `CLIENT_TIMEOUT`, `TRANSPORT_UNKNOWN`.

Hard rules:
1. **`done` without checkable files is impossible** (§2/§6). A backend that fails halfway produces
   `BACKEND_FAILED`/`OUT_OF_MEMORY`, never a partial `done`.
2. OOM: report code + `details` (phase, resources), no silent retry/offload; treino-3 and the user's other jobs are
   never touched by a failing service.
3. `details` never holds secrets or raw traces (C00 rule [V errors.py docstring]).
4. A failed job publishes no outputs: files in `outputDir` without `manifest.json` and without a `done` line are the
   leftovers of a failed job, at the client's decision (§6).

## 8. Progress, cleaning and life cycle (the place of cancel/heartbeat)

- **Progress** is the only "alive" channel: `progress` lines (§4) with `phase` + monotonic `fraction`. There is no
  heartbeat, `suspectAfterSeconds` or server `updatedAt` (§13): if the lines stop, the decision is the user's, not a
  timer's.
- **Cancel: it does not exist in the trimmed v1** (S1b decision, §13). No `cancelToken`, no cancel file, no
  `cancelled` line, no `POST …/cancel`. The available action is **killing the process**, and killing it is always
  safe because the service only publishes through `os.replace` after checking: the worst case is an orphan `.part` in
  `outputDir/.staging/<taskId>`, never a half output. Cancel comes back in a slice with a resident service (stopping a
  GPU halfway needs proof that the memory and the device were released — not a schema field).
- **Parent-pid watchdog:** deferred (§13). The one-shot process assumes no resident parent: if the client dies, the
  job ends and writes in the `outputDir` it was given, which the next client discards or imports again through
  `manifest.json`.
- **Cleaning:** `done` and `error` always clean `outputDir/.staging/<taskId>`; the gate confirms the folder keeps no
  `.part` (core_stems model [V l.113–148]).
- **Phases per profile:** midi `read/separate/transcribe/quantize/write`; stems `read/separate/write`; flamingo
  `read/transcribe/write`; lora `dataset`: `read/curate/split/write`, `train`: `read/train/write` (§5c).

## 9. Versioning and sha256 manifests

- **Contract:** `aicore.service/1`; **schemas:** `schemaVersion "1.0"` (const). The hello announces `contract` (§3)
  and each `start`/`done`/`error` echoes it. **There is no negotiation:** an unexpected value → the client stops
  before submitting and shows the difference (the `VERSION_CONFLICT` negotiation was deferred, §13).
- **v1 frozen after the gate.** A breaking change → a `schemas/v2/` folder + contract `/2`; additional **optional**
  fields and new enum values only come in with a manifest revision + the gate run again (the gate validates every
  example against the schemas; counts derive from the source, never copied literals).
- **Additive vs breaking in S1b:** `1/32` in the grid is additive (the enum widens, old files stay valid). `ppq: 960`
  instead of `480` **is breaking** — so it changed `midi-params`' `const`, `midiFormat.ppq` and every example at once,
  instead of widening the enum to accept both. `.mid` files written at 480 before S1b cannot be imported by this
  contract; migrating is a separate tool's task.
- **Vendoring:** `common.schema.json` and `error.schema.json` are **byte-identical** copies of AICORE v1 @ `500bec88…`
  (sha256 in the manifest). They are never edited here; upstream changes mean re-vendoring with a new recorded sha.
  AICORE's 25 v1 schemas [V real count in `packages/contracts/.../schemas/v1/`; the PLAN said 27 — corrected from the
  source] stay the base; this slice's new names do not collide and live in the same namespace
  `https://aicore.local/schemas/v1/` so a joint registry works.
- **Deliverable manifest:** `contracts/manifest.json` (`contract-manifest.schema.json`) lists each contract file with
  `{path, bytes, sha256}` + `baseCommit` + `vendoredFrom` + counts. Generated by `sim/make_manifest.py`; the gate
  recomputes it and **fails red** on a wrong hash or a missing file (not skip).
- **Runtime manifest:** §6. Every published output has its sha256 in the manifest; the client checks before importing.

## 10. Legacy ACE profile — UNCHANGED (compatibility)

The existing ACE-Step server **does not change**. The native app implements the client exactly like `ace_backend.py`
[V @500bec8]. The generic §4 envelope **does not wrap ACE**; it lives beside it.

- Routes [V l.78–246]: `GET /v1/model_inventory` (only `is_loaded: true`), `POST /release_task` (multipart:
  `task_type` cover/repaint/text2music, `prompt`, `lyrics`, `instrumental`, `audio_duration`, `audio_format=wav`,
  `batch_size=1`, `inference_steps=8`, `seed`, `model`, `audio_cover_strength`/`repainting_start/end`, `bpm`,
  `src_audio`), `POST /query_result` `{task_id_list:[...]}` (status **1=done, 2=failed**), `GET /v1/audio` (download
  only from the same host/path, cap 128 MiB), `POST /v1/training/status|start|stop|export`,
  `POST /v1/dataset/scan|preprocess_async`, `GET /v1/dataset/preprocess_status/{task}`, `PUT /v1/dataset/sample/{i}`.
- ACE response envelope: `{code, data, error?}`; `error` present or `code ≠ 200` → failure [V l.73–75]. This is
  **distinct** from the generic §4 envelope — the native adapter translates, it does not mix.
- Deadlines [V]: request 45 s; download 90 s; generation 3600 s; preprocess 7200 s; training 7 days; poll 2 s.
- `TransportUnknown` on 5xx/network/no taskId/stopped locally/timeout — **no automatic resend**; a
  `remoteOperation`/`remoteTaskId` checkpoint for reconciliation [V l.148–157].
- Training: exclusivity (`is_training`), checked ownership (`tensor_dir` must be ours, else `TransportUnknown` "state
  belongs to another training" [V l.229]), frozen dataset checked by sha256 [V l.181–183], exported weights validated
  (safetensors header/payload) + `adapter-manifest.json` `status: "trained-unvalidated"` [V l.243–250].
- Mapping to the native UI's task model: `task_id`→`taskId`; status 1→`done`, 2→`failed`; `result[0].file`→output
  `candidate.wav` (validated download); `musicalValidation: "unreviewed"`→`validation`.

**No-break guarantee:** S1 changes no ACE route/field/cap; the new schemas neither reference nor replace the ACE
flows; the simulator does not implement the ACE server (only the generic envelope + the midi profile). Any future
change to the ACE profile needs its own slice + a compatibility gate.

## 11. Simulator and gate (proof)

`contracts/sim/` — Python 3.10+ **stdlib only**, CPU, no models/GPU/weights/external network. Commands, measured
counts and captured output: `contracts/evidence/gate-run.log` and `contracts/sim/README.md`.

- `service_sim.py hello` — one `service-hello` line on stdout (profiles, limits, `ppq` 960, accepted grids).
- `service_sim.py job` — reads **one** request line from stdin, runs the simulated pipeline, writes
  `start`/`progress`…/`done`|`error` on stdout and exits `0`/`2`. **Only** this transport: there is no HTTP server,
  port, polling, cancel file or watchdog.
- `service_sim.py fixture` — a deterministic synthetic WAV with percussion events spaced as asked
  (`--hits-per-beat` 1/4/8/16 = quarters/1/16/1/32/1/64, `--hit-seconds`, `--decay`, `--alternate`), for the gate to
  feed rolls and notes shorter than the grid. **It is not music or model output**: test material, always `synthetic`.
- Engine: `media.py` writes SMF1 **PPQ 960** by hand and refuses another ppq; placement uses the asked grid
  (`grid_ticks`), the length comes from the measured audio and the drum pitch comes from the audio (`classify_hit`),
  not from the event's index.
- Injectable faults — **test hooks, never a real service behaviour**: `backend-failed`, `oom`, `fake-success` (a
  `done` line without a `.mid` on disk) and `wrong-sha` (a `done`/manifest line with a sha256 that does not match).
  Left S1 with the transport: `hang` and `ignore-cancel` (cancel deferred, §13). `bad-contract` stays, only in
  `hello`/`job`, for the gate to prove the client's rule (refuse before submitting, §3/§9).
- `jsonschema_mini.py` — a **fail-closed** draft-2020-12 validator (unknown keyword → error, never ignore). With no
  local `jsonschema`/`ajv` and no installing deps (task restriction) → correctness is proven by **mutations** in the
  gate (valid documents accepted; each mutation class rejected). A cross-check with an independent library: **not
  run** [H] — for when there is a venv with `jsonschema`.
- `gate.py` — the S1b matrix: happy path at PPQ 960, 1/32 roll, notes shorter than the grid, hello, typed errors (a
  request with S1 fields, a `.mid` at 480, missing reference, swapped hash, limits), manifest integrity, examples
  against schemas, and the **canaries that must give red** (`fake-success`, `wrong-sha`, validator mutations,
  unknown keyword). If any canary passes, the gate fails. `--noop` replaces validator+checks with no-ops: the canaries
  then "pass" → the gate detects it and exits **red** (proof that the gate can fail; a green without numbers
  authorises nothing, AGENTS gate 5). It records **every stdout/stderr line** in `gate-report.json` + a table with
  counts and sha256.

## 12. What this slice does NOT prove [H]

- Parity/quality of the real MIDI service (S2): recall/precision/note F1 vs truth — to measure.
- The real ACE's behaviour against the native client (S3): the §10 profile is a ported spec, not an execution.
- A cross-check of the schemas with an independent `jsonschema`/`ajv` (missing on this machine; no installing).
- A real import of a PPQ 960 `.mid` into the Core DAW: the number 960 comes from the owner (templates written at 960);
  the write→import round with the real service is still to measure.
- The transport with S2's real worker (process start, pipe code page, stdout buffering) — the simulator uses the same
  pattern, the trial with the real Python service is for S2.
- Production caps (2 MiB per line, 128 MiB input, 8 MiB MIDI): inherited from S1 and `ace_backend` [V], to measure on
  the real service.

## 13. Deferred, when it is needed

S1b decision [owner]: one person, one machine, and what is wanted now is `request → progress → .mid → error` with
**one** transport. The mechanisms below leave v1 because **there is nothing for them to protect yet**, not because
they are bad. Nothing disappears in silence: each line says what it was and the trigger for it to come back.

| Deferred item | What it was in S1 | Why it leaves now / when it comes back |
|---|---|---|
| **T1 — HTTP loopback versionado** (versioned HTTP loopback) | A resident server, `GET /v1/hello`, `POST /v1/tasks`, `GET /v1/tasks/{id}`, polling every 2 s | The one-shot subprocess already gives hello, request, progress and result; a port + HTTP client + polling are weight with no beneficiary. Comes back when a service has to outlive the app or serve a queue. |
| **Idempotency by `requestId`** | Resending the same `requestId` with an identical request returned the same `taskId`; different params → `IDEMPOTENCY_CONFLICT` | Without a resident process there is no memory to look `requestId` up in; a resend is an explicit decision and writes to another `outputDir`. Comes back with the queue/T1, or if the client needs "do not run this twice" without a human. |
| **Reconciliation after a failure** (`GET /v1/tasks[?requestId]`, `unknown` state, `NOT_FOUND`) | Finding out what a restarted service managed to produce | The process does not outlive the client; the authority becomes `outputDir` + `manifest.json` checked by sha256 (§6). Comes back with T1. |
| **Heartbeat / `updatedAt` / `suspectAfterSeconds` (5 s / 30 s)** | The service renewed its state periodically; the client suspected and reconciled | The `progress` lines already prove the job moves, and without a server there is no server clock ticking. Comes back when there is a job that can go quiet without dying (queue/T1). |
| **Cancellation** (`cancelToken`, `cancelFile`, `POST …/cancel`, `service-cancel-request/response`, `cancelled` line/state) | Stopping a job halfway with an ack in ≤1 s, clean staging | Killing the process is already safe because nothing is published without `os.replace` (§6): at most a `.part` is left in staging. A *correct* cancel with a GPU needs to prove the memory release and the device ownership — S2/S3 work with the real service, not a schema field. |
| **Parent-pid watchdog** (`parentPid`, exit 3) | The worker stopped when the client died, so as not to hold GPU/memory | An orphan job on a single-user PC ends, writes in the `outputDir` it was given and can be thrown away; it blocks nothing. Comes back with the first service that holds a GPU. |
| **Version negotiation (`VERSION_CONFLICT`)** | Comparing `contract` in the hello and choosing/negotiating a version | There are never two contracts in production at once: the value is the const `aicore.service/1`, echoed in `start`/`done`/`error`; a difference = stop (§9). Comes back with T1/multi-version services. |
| **Deadlines** (`deadlineSeconds`, `TASK_TIMEOUT`, `CLIENT_TIMEOUT`) | The job's deadline in the service and the waiting deadline in the client | Without a server there is no one to time it, and without a queue there is no job "waiting". The hard format/size caps (§6) stay. Comes back with the queue/T1 or when a real job needs its own deadline. |
| **`BUSY` / `maxConcurrentTasks`** | A queue with concurrency 1 in the `midi` profile | Parallelism = parallel processes with different `outputDir`s; without a queue there is no "busy". Comes back with the resident service (and with the GPU ownership of ACE training). |
| **`queued`/`cancelled`/`unknown` states and `cancelled`/`probe` events; `service-task-status` and `service-progress` schemas** | A state machine queried by polling | A subprocess only knows `start → progress → done/error`. The state schemas drop because there is no state to query; what mattered (`progress`, `result`) now lives in `service-event.schema.json`. |
| **`UNAUTHENTICATED` / `SCOPE_DENIED`** | A bearer key on HTTP | There is no HTTP or auth (`authRequired: false`, single-user loopback). Comes back if a service exposes a port. |
| **`TRANSPORT_UNKNOWN`** | "the operation may have continued; do not resend, reconcile" | In a process there is no ambiguous transport: either a terminal line arrives, or the process died (`WORKER_UNAVAILABLE`, §2.4). Comes back with T1. |
| **`probe`/`evaluate`/`export` phases of the `phase` enum** | The Flamingo/LoRA workers' phases | The enum kept the phases of this contract's profiles; the profiles that need them bring them back, additively (§9). `train` (+ `curate`, `split`) already came back with the `lora` profile (§5c). |
