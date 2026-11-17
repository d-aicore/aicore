# AICore-C — universal brief (every agent)

The **native C++** AICORE app: audio studio, memory and training client. It replaces the Python/PySide6 app, in
phases, copying it screen by screen. **No browser, no webview, no web UI.**
Owner: Diogo. **The interface and everything about the app (docs, notes, commits) in English** (owner 2026-11-16);
talking with the owner in PT-PT. Use is **private, single machine, never distributed**.

This is the only instruction file for agents — do not write briefs per feature. Each task gets a short delta on top
of this brief.

## Hardware (optimise for this machine)

Ryzen 7 7800X3D (8C/16T), ~31 GiB RAM, RTX 5070 Ti 16 GB VRAM (Blackwell sm_120), Windows 11 + WSL2.

## Fixed stack [V 2026-11-16]

- **C++20, JUCE 9.0.5** — the same pin as the Core DAW: commit `d60b7a78030661574a615b586d9325ba7a7fa7dd`,
  URL `https://github.com/juce-framework/JUCE/archive/refs/tags/9.0.5.tar.gz`,
  SHA256 `d92207f51ba728d3d195b6d5e93ecd049717a915fc250ff6844c32ea7272315a`.
  AICore's own copy in `D:/AICore/development/deps/JUCE-9.0.5` (official tarball, SHA256 above checked
  2026-11-16) — nothing comes from the Core; use `FETCHCONTENT_SOURCE_DIR_JUCE` (no download). Framework confirmed
  by the owner ("sim juce").
- **CMake ≥ 3.22 + Ninja + MSVC** (VS 18 BuildTools:
  `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools`). Installed: cmake 4.4.4, ninja 1.13.2, git 2.52.
  Build: vcvars64 → `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`, at low priority (`start /LOW`).
- ONNX Runtime C/C++ for native inference (Basic Pitch). SQLite C API (the same databases).
- The version is baked at configure from the git sha (the Core's `core_version.h` pattern): version string ==
  commit sha, **without `.modified`**. A dirty tree at build = invalid gate.

## ML boundaries (not negotiable)

ACE-Step v1.5 turbo (generation + ACE LoRA), Music Flamingo 7B (audio understanding), Demucs (stems), Qwen LoRA
(text training) are **separable Python/WSL backends**, called through **local contracts**: versioned loopback HTTP
(model: `ace_backend.py` in the AICORE repo) or a JSON job (model: `experiments/audio-listener`). The native app
**never** imports torch/CUDA, never embeds models, never runs inference in the UI process. **Weight downloads only
with the owner's explicit permission.** No paid APIs. The audio stays on the PC.

**Audio→MIDI (Demucs + Basic Pitch) is a separable SERVICE next to ACE** — it speaks the same local contract, it is
not an in-process UI module. **The app↔services contract is defined EARLY** (requests, .mid/.wav files, errors,
cancel, progress, versioning) — that is where these apps usually break (S1). Training a LoRA in C++ is not realistic
today (libtorch C++ beta, PEFT/bitsandbytes Python) → a Python/WSL worker.

The pages' heavy jobs run in AICore's own Python (`D:\AICore\python`) through `app/src/ui/Worker`
(`-m aicore_studio.audio_tasks` or the scripts in `services/`), with their data in `%APPDATA%\AICore-C`. AICore
reads and runs nothing of the Core; renders use AICore's own `aicore-render` (VST3).

## Logic sources to port (READ-ONLY — never write there)

- `D:\AICore\app` — the installed AICORE Python app (the old PySide6 UI, contracts, schemas, DSP): **the reference
  every page is copied from**. `D:\AIcore-new` — the AICORE Python repo @ `500bec8`.
- `D:\AICore\midi-maker\` — the measured audio→MIDI pipeline (Demucs + Basic Pitch ONNX, kick→note 36, mono bass,
  1/16 quantisation with the kick's phase; recall 60–70% on 3/4 Core renders, precision 0.68–0.87).
- Core DAW (C++/JUCE): reuse build/version patterns and, when it makes sense, engine code (copied, never linked).

## Roles and git flow

- **Workers**: implement on their own branch (`slice-N/name`), **one commit per task**, never push/PR/merge. They
  report numbers, not states.
- **Coordinator**: dispatch, local gate with numbers, merge to `main` and **push to origin after the gate**, only
  with the owner's OK written in the thread.
- **Owner**: decisions that change the goal, irreversible steps, blocks only he can clear.
  Remote: https://github.com/dmo-app/AICore-C (private).

## Gates (what counts as proof)

1. A build from a **deleted build folder**; version string == HEAD sha, without `.modified`.
2. Reproducing the problem/feature: **exact command + output before/after**, or a sha256 that moves/does not move.
3. A sound change is **always reported** with both hashes and the dB delta — never hidden or "tuned".
4. UI: "everything opens" — the window opens and the flow works (an offscreen screenshot/grab as evidence); for a
   page copied from the old app, the old and new screenshots side by side.
5. **The suite is not true by itself.** A green without numbers authorises nothing. Before trusting a gate, prove it
   can fail (a no-op control / missing reference must give red, not skip).
6. Coverage derives from the current source (counts/names from the code, never copied literals).

## Working rules

- Each claim is **[V]** (run/inspected, with sha/version/command) or **[H]** (still to measure). Estimates never as
  measurements. Do not promise gains before measuring.
- Do not install global dependencies without need — FetchContent / local copies first.
- Do not ask preferences already answered; recommend and justify choices that can be inferred.
- One build per folder (two ninjas in the same folder corrupt the lock). Logs with their own names per worker.
- If Bash gives fork errors/0xC0000142, use the documented recovery (cmd/powershell for the build).
- Restrained UI: no needless chrome; follow the old AICORE's look screen by screen.
- Keep the owner's originals and changes; `git status` before any destructive operation; nothing new at `D:\` —
  everything inside `D:\AICore`.

## Temporary restrictions (owner 2026-11-16)

- **treino-3 running — DO NOT TOUCH until it ends (~16:00 Europe/Lisbon 2026-11-16).** Do not interfere, kill or
  restart that training.
- **GPU reserved by treino-3** → GPU qualifications (Flamingo/MuScriptor/audio.cpp CUDA/Demucs GPU) blocked until it
  ends. CPU/docs/builds/contract OK meanwhile. Confirm treino-3 has ended + the GPU is free before any GPU run (and
  the owner's permission for weights).

## State

See `PLAN.md` for slices, gates and open items, and `docs/HANDOFF-2026-11-16.md`. Every page of the old app is
ported (PR #1, branch `slice-6/definicoes`); Flamingo is the screen only (paused by the owner).
