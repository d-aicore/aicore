# PLAN — AICore-C (AICORE's native C++ app)

Date: 2026-11-16. Owner: Diogo. Directive: "I want nothing in the browser, I want a robust solution like C++" +
"AICORE also works with ACE, not only Flamingo and Demucs".
**Owner additions (2026-11-16):** (1) define the JUCE↔Python contract EARLY (requests, .mid/.wav, errors) — that is
where these apps break; (2) audio→MIDI (Demucs + Basic Pitch) comes in as a **service** next to ACE; (3) **do not
touch treino-3 until it ends (~16:00)**; (4) the native app is a screen-by-screen copy of the old PySide6 AICORE —
"finished" = everything the old one did; (5) AICore is a program of its own: nothing read from the Core; (6) the
interface and everything about the app in English.
Universal brief: `AGENTS.md`. Repo: https://github.com/dmo-app/AICore-C (private).

## Fixed decisions

| Decision | Value | Evidence |
|---|---|---|
| Language/app | native C++20, no browser | Owner directive 2026-11-16 |
| UI/framework | **JUCE 9.0.5 — owner confirmed "sim juce"** (same pin as the Core) | [V] Core CMakeLists SHA256 d92207f5…, commit d60b7a78; owner decision 2026-11-16 |
| Build | CMake + Ninja + MSVC VS18 BuildTools | [V] cmake 4.4.4, ninja 1.13.2, vswhere → 18\BuildTools |
| JUCE source | Own copy `D:/AICore/development/deps/JUCE-9.0.5` (official tarball, SHA256 checked 2026-11-16) — **9.0.5 confirmed**, `FETCHCONTENT_SOURCE_DIR_JUCE` (no download) | [V] `project(JUCE VERSION 9.0.5)` + juce_core.h; AGPLv3+commercial (private OK) |
| Architecture | JUCE/C++ interface + audio; heavy ML (ACE/Flamingo/Demucs/LoRA) in Python as **separate services**; training in C++ not realistic today | Owner 2026-11-16 + R1 [V] (libtorch C++ beta, PEFT/bitsandbytes Python) |
| Interface | English only (owner 2026-11-16) | Owner message in the thread |
| Native transcription | **muscriptor.cpp** (C++/ggml, MIT, Vulkan) candidate; Basic Pitch ONNX (ORT C++) measured baseline | [V] NeuralNote v2 + muscriptor repo |
| Heavy ML | Separable services through a local contract (ACE loopback HTTP, Flamingo JSON job, Demucs JSON lines, **MIDI service**) | [V] ace_backend.py, core_stems.py, audio-listener |
| DB | SQLite C API, the same files/contracts | [V] AICORE repo SQLite/FTS5 |
| Flamingo | **NOT a transcriber** — audio→text understanding (Qwen2.5, vocab 151672). It guides parameters, never makes notes | [V] local config + README/card/paper + audio.cpp doc, reproduced |

## Research gate (2026-11-16) — R1 and R2 CLOSED and checked

- **R2 (Flamingo→MIDI):** report + evidence in `D:\AIcore-new\docs\research\2026-11-16-flamingo-midi\`. Reproduced:
  Flamingo config vocab 151672 / 8.27B / 15.4 GiB bf16, 4.4 MB blobs (weights INCOMPLETE); wakeup bass recall 0.159 /
  precision 0.481, t1 0.691 / 0.883 (exact JSON); `D:\AICORE-data` gone, `D:\AICore` equivalents OK; WSL venv torch
  2.11.0+cu128 / transformers 5.22.1 / bnb 0.50.2; muscriptor.cpp (MIT, ggml, Vulkan, C++23, GGUF 210 MB/620 MB/
  2.7 GB). **Natives checked:** audio.cpp (0xShug0 v0.9.5, Apache-2.0, ggml, families htdemucs/muscriptor/
  audio_flamingo-2601/ace_step, CLI+server+C API), demucs.cpp (sevagh, MIT), demucs-onnx (HRSadeghi), original
  Flamingo llama.cpp GGUF+mmproj (henry1477).
- **R1 (language):** report + benchmarks in `D:\AIcore-new\docs\research\2026-11-16-linguagem\`. Reproduced: LOC
  studio 19 258 / PySide6 UI 11 105; B1 ~73–76× realtime (peak 0.4259); corrections — Memory stdlib
  ThreadingHTTPServer (not FastAPI), ONNX/ffmpeg missing from the repo, TRL only mentioned in a README (real trainer =
  MockTrainer P5); local JUCE 9.0.5 + AGPLv3; ACE routes (`/v1/model_inventory`, `/release_task`, `/query_result`,
  `/v1/audio`, `/v1/training/*`, `/v1/dataset/*`) and `prepare_ace.py` (bundle vae + Qwen3-Embedding-0.6B +
  acestep-v15-turbo) confirmed locally. **R1 recommends Qt Widgets for a ~1:1 UI; the owner chose JUCE — decision
  recorded below, report kept.**

## Decision note — performance and framework (2026-11-16)

- **The performance suspicion is NOT confirmed.** Python/numpy/scipy DSP: export ~67× realtime, live 14.7× headroom,
  mix 6×3 min 7.3×; I/O/resample/LUFS 590–9885×. Heavy ML already native/GPU in subprocesses. **The native rewrite is
  a product decision (robustness/long term/alignment with the Core), not a performance need.**
- **The only "slow" thing measured:** LUFS re-measure after knobs = 15.8 s (6 stems × 3 min). Optimise first in
  Python (float32/fusion/cache) with A/B + owner approval; it serves as the parity oracle for the DSP port. To
  measure: UI start, model/WSL loading.
- **Framework — OWNER'S FINAL DECISION: "sim juce" → JUCE, Qt dropped.** C++/JUCE 9.0.5 (present, proven, Core reuse,
  no install). A more hand-made JUCE UI (Component/resized()/LookAndFeel, no QLayout), made up for by the included
  audio/MIDI/DSP + Core reuse. AGPL does not bite in private use.
- **What is NOT rewritten:** Memory (stdlib HTTP loopback), the LoRA worker (Python/WSL), the Demucs/ACE/Flamingo
  providers, contracts + 27 schemas, data. A Rust rewrite is not recommended; it stays as a surgical PyO3 extension if
  a measured gate fails.

## Temporary restrictions (owner 2026-11-16)

- **treino-3 running — DO NOT TOUCH until it ends, ~16:00 Europe/Lisbon 2026-11-16** ("around 16:00"). Do not
  interfere, kill or restart that training.
- **GPU reserved by treino-3** → GPU qualification runs (Flamingo NF4, MuScriptor, audio.cpp CUDA, Demucs GPU)
  **blocked until treino-3 ends**. CPU/docs/builds/contract work OK meanwhile. Confirm treino-3 has ended + the GPU is
  free before any GPU qualification (and the owner's permission for weights).

## Slices

### S0 — Toolchain proof + skeleton (CLOSED and integrated @ 61ccf7ed39)
CMake+JUCE that builds from a deleted build folder, opens a window with title+version, `--version` with the git sha
(without `.modified`), a minimal self-test. Folder layout, .gitignore, README.
**Gate:** clean build + version==sha + the window opens (grab) + self-test ok. Branch `slice-0/skeleton`.

### S1 — JUCE↔Python contract (DEFINE EARLY — CLOSED @ 7bf7d1f)
"That is where these apps usually break." Spec of the native app ↔ Python services contract, **before** building the
services. Reuse the AICORE repo's `packages/contracts/schemas/v1` (27 framework-agnostic schemas).
- **Transport:** versioned loopback HTTP (`ace_backend.py` model) and/or a JSON-lines subprocess (`core_stems.py`
  model); chosen per service; **no browser**. → **S1b: the MIDI service keeps only the JSON-lines subprocess; T1 HTTP
  leaves the new contract (the legacy ACE keeps its own T1, unchanged).**
- **Requests/responses:** versioned JSON schemas; request/task id, parameters, absolute paths.
- **.mid/.wav files:** path ownership, formats (WAV PCM16/24/float + SR; MIDI PPQ 960 — the same as the Core, no
  conversion step; owner ruling 2026-11-16, see S1b), **atomic writes** (staging+rename), who deletes, size/length
  limits.
- **Errors:** typed codes (`TransportUnknown` model — no silent automatic retry), a backend failure reported never as
  a fake success, timeouts, OOM.
- **Cancel/progress:** cancel file/token (core_stems + audio-listener model), phases, parent-pid watch.
- **Versioning:** contract version + compatibility; a manifest with sha256 (ACE `trained-unvalidated` model).
**Deliverable:** `contracts/` spec + JSON schemas + synthetic request/response examples.
**Gate:** contract exercised against a versioned simulator (requests/responses recorded; error and cancel proven —
the gate can fail, a missing reference gives red not skip). Branch `slice-1/contract`.

### S1b — Contract revision by owner ruling (2026-11-16, CLOSED)

The owner's ruling, applied on top of S1 (three points, all his, not my suggestions):

1. **PPQ 960, not 480.** The Core writes templates at 960 ([V] `Templates/*.core-template.json` `"ppq": 960` +
   `doc::kPpq` in the engine); the contract at 480 lost precision in rolls and short notes in the round trip. Choice:
   the contract moves to 960 (lossless, no conversion) instead of fixing 480 + documenting a converter.
2. **Grid down to 1/32.** psytrance/hitech rolls are at 1/32; with a 1/16 maximum they were lost. Default 1/16 for
   generic MIDI, 1/32 documented as the choice for rolls. At 960, 1/32 = 120 ticks.
3. **Less weight for one person on one PC.** Idempotency, reconciliation, heartbeat, cancel, parent-pid, version
   negotiation and the second transport go to a **"Deferred, when it is needed"** section — visible, not deleted. The
   exercised contract becomes the MIDI happy path: **request → progress → `.mid` (+sha256 manifest, atomic writes) →
   typed error**, on one transport (JSON-lines subprocess). The legacy ACE profile stays **unchanged** with its own T1.
- **Not deferred:** atomic writes + sha256 manifest + fail-closed schema validator + a non-vacuous gate (`--noop` must
  exit red). File integrity and proof that the gate fails are not a luxury of scale.
**Deliverable:** CONTRACT.md + schemas + examples + `contracts/sim/` and gate redone; manifest regenerated.
**Gate:** `make_manifest.py verify` exit 0; `gate.py` exit 0; `gate.py --noop` exit 1. Branch `slice-1b/contract-slim`.

### S2 — Audio→MIDI as a SERVICE (Demucs + Basic Pitch), next to ACE (plumbing CLOSED, quality open)
A **separable service** that speaks the S1 contract — not an in-process UI module. Native app = client. The service
runs as a backend (initial Python oracle, or native demucs.cpp/ORT C++ after parity), exposed through a local contract
**next to ACE** (the same service pattern).
- **Baseline (measured):** the logic of `midi_maker.py` (read-only) — Demucs → low-band 40–120 Hz kick onsets → note
  36; Basic Pitch ONNX on the bass/other stems; mono bass; BPM from the name/kick; 1/16 quantisation with the kick's
  phase; `split_on_onsets` re-cut; lead filter; .mid PPQ 480. Measured: bass recall 0.564–0.691 (remember/t1/t2;
  wakeup 0.159 bad case), precision 0.48–0.88; ~35 s/30 s CPU. **The S2 service writes at PPQ 960 and accepts a grid
  down to 1/32 (S1b); the 480/1/16 above is what `midi_maker.py` measures, not the contract.**
- **Quality debt:** the owner listened and rejected it ("it should be like listening to Guitar Pro") — see
  `services/midi/README.md`.
- **Native candidate: muscriptor.cpp** (C++/ggml, MIT, Vulkan; multi-instrument incl. drums/e-bass/synth lead; 16 kHz
  mono → note events). Qualify on the SAME fixtures (4 Core renders + .core-template.json + 3 KBBB loops) with **note
  F1 mir_eval** (onset ±50 ms + offset). Caveat: pitchExact bass in KBBB samplers invalid → measure pitch class.
  Psytrance OOD → measure. Needs the Vulkan SDK + gated HF GGUF weights CC BY-NC (owner). MSVC not tested upstream
  (Clang only) → qualify the compiler.
**Gate:** note F1 per part vs truth; paired baseline vs muscriptor comparison; an audio→.mid demo that opens in the
Core. Nothing promoted without measurement + the owner listening. **Flamingo does NOT transcribe** — understanding
only.

### S3 — ML backends through the contract + a unified native option
Two paths, decided with proof of parity (framework-agnostic contracts):
- **Local contract (baseline, already exists):** ACE loopback HTTP client (`ace_backend.py`) + Flamingo JSON job
  (`audio-listener`) + Demucs JSON-lines stems (`core_stems.py`) + MIDI service (S2). Current Python/WSL backends as
  the oracle. No torch/CUDA in the UI process.
- **Unified native engine (strong candidate, TO QUALIFY): audio.cpp** [V primary source] — C++/ggml, no Python,
  Apache-2.0 (0xShug0 v0.9.5, 149 families): `htdemucs` (Sep), `muscriptor` (MIDI), `audio_flamingo` (MF **2601**,
  understanding), `ace_step` (1.5 Turbo, Music/Edit). **CLI + loopback server + C API** without a browser (the
  embedded SvelteKit WebUI is NOT used). CPU/CUDA/Vulkan; Windows CUDA 12.4/13.3 prebuilt — **sm_120 needs 12.8+** →
  13.3/Vulkan or `--cuda-arch native` [H]. Build with VS BuildTools/MSVC/CMake/Ninja. **Risks:** fast community
  project, parity by its authors not by us; weights keep their upstream licences. Do not adopt blindly — a parity gate
  (SDR/note F1/A/B/ACE audio) vs Python before replacing.
- **Point natives:** demucs.cpp (sevagh, MIT), demucs-onnx (HRSadeghi, ORT C++); Basic Pitch `nmp.onnx` (225 KB) ORT
  C/C++ + a `note_creation` port; original Flamingo llama.cpp GGUF+mmproj vs 2601 audio.cpp.
**Gate:** contract exercised against a real backend/versioned simulator, requests/responses recorded; a failure
reported never as a fake success. Native engine: parity measured vs the Python oracle before removing a Python step.
**ACE coexists** — not replaced without proof + approval.

### S4+ — Porting the studio (DONE 2026-11-16, PR #1)
Every page of the old app, screen by screen, with old-vs-new screenshots in `docs/evidence/`: EDIT (TRACKS with
CHAIN · MASTER, SAMPLE, STEMS, VOICE, MIDI), AI (ACE · FLAMINGO — Flamingo the screen only, paused by the owner —,
AUDIO LORA, TEMPLATE RENDERS through `aicore-render`, DATABASE), SETTINGS (FOLDERS, MODELS, KEYBINDS, and the new
PLUGINS). The heavy jobs run in AICore's own Python (`ui/Worker`). Still to check with the GPU: a real Demucs
separation and an ACE generate.

### S5 — plugin render outside the Core (DONE: `aicore-render`)

The owner's external brief (external tools, 16/11) proposed rendering the `.mid`/`.json` through his plugins **without
the Core open**, through **DawDreamer** (Python + JUCE, GPLv3). That would make a **third plugin host** on the same
machine, so the render became native: `aicore-render` (C++/JUCE VST3 host, `render` profile of the contract), with
AICore's own plugin list (Vital, Airwindows, Dragonfly; sfizz waiting for the download OK) and roles chosen in
SETTINGS > PLUGINS. DawDreamer only as an oracle, if the owner says yes. See `services/render/README.md`.

- **ACE `lego` mode** (making a track while listening to the mix): `ace_backend.py:104` closes the modes and `:119`
  already chooses the model by `supported_task_types`, so it is a small widening of the **legacy** ACE profile — but it
  needs the GPU and checking whether treino-3's LoRA works with lego. **Blocked until treino-3 ends + permission.**

### Future — instrument maker (sample → preset). **DO NOT START NOW**

The owner's idea, clarified 2026-11-16: give a sound he likes and the app returns a matching preset. **It already
exists, outside the app** — checked on this PC: `D:\AI Dev\core-final\tools\bcore_fit\fitwave.py` (per-sample patch
by differential evolution over ~30 knobs, log-mel distance objective), `tools\kick_fit\` (the same for the kick);
numbers in `D:\AI Dev\avaliacao-instrumentos\LEIA.txt` (9 Nov): bass copied sample by sample at **0.5–0.9 dB in 13 of
20 samples**; kick **0.6–1.5 dB**; hats **3.4–8.4 dB** (the synth does not get there; the sample is better).

**Where it goes, when its turn comes:** the fit engine **does not go into the UI process** — a separable service on the
S1b happy path (request → progress → preset `.json` + dB report with a sha256 manifest → typed error); the UI (drag a
sample, choose an instrument, A/B, import the preset) is a studio preparation page. With the VST-only pivot, the
target is a Vital preset rendered by `aicore-render`, not the Core's engine. **Owner's condition:** only after the
plugins are at 100 %; while the focus is fixing bugs, nothing new.

## Flamingo qualification (its own round, after permission + after treino-3) — PAUSED by the owner

Plan with exact commands in R2 §5. State [V]: WSL venv ready; weights INCOMPLETE (re-fetch ~15.4 GiB,
`HF_HUB_DISABLE_XET=1`, revision e560c33…); the repo's probe without the bf16 fix (4 lines in the local variant
`D:\AICore\models\flamingo\audio-listener\probe.py`). NF4 30 s ≤384 tokens; VRAM 6–10 GB [H]; OOM ladder without
silent retries. **Paused by the owner (2026-11-16) and blocked until treino-3 ends + weight permission.**

## Owner's open items

1. **MuScriptor:** HF account + CC BY-NC 4.0 licence + token; permission to download the GGUF (210/620 MB/2.7 GB) and
   to evaluate on the fixtures. **GPU gated until treino-3 ends.**
2. **Flamingo:** paused.
3. ~~**Basic Pitch .onnx**~~ **SOLVED without a download (2026-11-16):** `basic_pitch 0.4.0` installed in
   `D:\AICore\midi-maker\.venv` and the model is already there —
   `.venv\Lib\site-packages\basic_pitch\saved_models\icassp_2022\nmp.onnx`, 230 444 bytes, sha256 `2c3c1d14…`,
   mtime 2026-11-10. Apache-2.0, no permission needed.
4. **Quality gates** (note F1 per part) before promoting/replacing.
5. **Licences:** Flamingo OneWay Noncommercial + MuScriptor CC BY-NC — private OK, never distributed.
6. **Vulkan SDK** for the GPU build of muscriptor.cpp/audio.cpp (CPU fallback without it).
7. **sfizz download** (drums through SFZ kits) — waiting for the owner's OK.
8. **Merging PR #1** into `main` — the owner's decision.

## State

- 2026-11-16: repo created by the owner; foundation + plans pushed. **R1 and R2 closed/checked.**
- **S0 CLOSED and integrated @ 61ccf7ed39.**
- **S1 CLOSED and integrated @ 7bf7d1f.** S1b closed and pushed (78e2b84).
- **S2 pushed as plumbing (eee728a)**; quality rejected by the owner's ear, debt documented.
- **S5 render through plugins pushed (4cfbeb2).**
- **S4+ every page ported** (branch `slice-6/definicoes`, draft PR #1, with `chore/juce-proprio` and
  `slice-5b/sampler`); `D:\AICore\Main` = the installed build, Desktop shortcut "AICORE (novo)".
- Active restriction: treino-3 not to be touched until ~16:00; GPU reserved → GPU checks blocked until then.
