# services/midi — audio→MIDI service (S2)

A separable service that speaks the `aicore.service/1` contract (S1b, `contracts/CONTRACT.md`): a JSON request on
stdin, `start` → `progress`… → `done` | `error` lines on stdout, exit 0/2. It writes SMF1 `.mid` at **PPQ 960**,
with a grid down to **1/32**: the note start goes to the grid, the length comes from the audio. CPU only, child
processes at BELOW_NORMAL priority. Nothing is promoted without the owner listening: everything comes out
`validation: "unreviewed"`.

## Pipeline

It ports (does not import) the measured baseline `D:\AICore\midi-maker\midi_maker.py` (READ-ONLY):

1. Reads the WAV, cuts the `startSeconds`/`lengthSeconds` window, converts to 44.1 kHz stereo.
2. Demucs `htdemucs` on CPU through AICORE's `core_stems.py` (READ-ONLY, `D:\AICore\app`) in the `stems-env`, with
   local weights (`HF_HUB_OFFLINE=1`, no downloads). `demucs_seeded.py` fixes `params.seed`: without it the same
   request gave different stems (measured: 69 vs 75 kicks in two equal runs).
3. Kick: onsets of the 40–120 Hz band of the drum stem → `kickNote`. BPM: the given `bpm`, from the file name
   (`bpmSource: filename`) or from the kicks; grid phase from the kicks.
4. Bass: Basic Pitch ONNX on the bass stem (30–400 Hz), re-cut at the 1/16 attacks (`splitOnOnsets`),
   monophonic (`bassMono`, the lowest note wins).
5. Lead: Basic Pitch on the "other" stem (150–5000 Hz), FX/blip filter (`leadFilter`), at most 2 voices.
6. `transcription.mid` (tracks `kick` ch10, `bass` ch2, `lead` ch3; tick 0 = grid phase), optional stems,
   `report.json`, `manifest.json`, all through staging + `os.replace`, sha256 per file.

Differences from the baseline: PPQ 960 (was 480), configurable grid down to 1/32 (was 1/16 only), measured length
(was whole grid steps), Demucs seed, stdout with contract lines only (Basic Pitch prints "Predicting MIDI for…" on
stdout; the service moves fd 1 to stderr).

## Running (on this machine)

The midi-maker `.venv` has a broken trampoline (HANDOFF §3): use the base interpreter + `PYTHONPATH`.

```
set PYTHONPATH=D:\AICore\midi-maker\.venv\Lib\site-packages
D:\AICore\uv-python\cpython-3.11.18-windows-x86_64-none\python.exe services\midi\aicore_midi_service.py hello
D:\AICore\uv-python\cpython-3.11.18-windows-x86_64-none\python.exe services\midi\aicore_midi_service.py job < request.json
```

Variables (optional): `AICORE_STEMS_PY` (the stems-env python), `AICORE_CORE_STEMS` (core_stems.py),
`AICORE_MODELS_HOME` (`D:\AICore\models`), `AICORE_MIDI_THREADS` (4).

## Gates

```
python services/midi/gate_service.py           # contract against the real service -> exit 0
python services/midi/gate_service.py --noop    # no-op control -> exit 1 (proof that it fails)
python services/midi/eval/score_templates.py --out <folder> --quantize 1/16   # quality vs baseline
```

- **Contract** (`gate_service.py`, a 6 s window of the `hitech-t1-v1` render): hello; happy path → PPQ 960 `.mid`
  with starts on the 120-tick grid and measured lengths; **typed error after progress** (`BACKEND_FAILED` with
  Demucs missing, `outputDir` stays empty); 10 bad requests → typed error; same request → same sha; canaries: a
  tampered `.mid` and a polluted stdout must be rejected. Evidence: `docs/evidence/s2-gate-service.log`.
- **Quality** (`eval/score_templates.py`): the 4 windows where the baseline was measured (renders of Core templates,
  true notes from the `.core-template.json`), the same rule as `avaliar.py` (start within ±½ of a 1/16 step). The
  baseline `.mid` files in `D:\AI Dev\midi-maker\avaliacao` are scored again by the same code, not copied. Bass also
  by pitch class. Evidence: `docs/evidence/s2-score.log` and `s2-score-*.json`.

## Measured result (2026-11-16, seed 42, 1/16 grid) — F1 service / baseline

| window | kick | bass | lead |
|---|---|---|---|
| remember 200 s +30 | 0.780 / 0.767 | 0.605 / 0.614 | 0.278 / 0.312 |
| wakeup 18 s +25 | 0.882 / 0.951 | 0.360 / 0.239 | 0.432 / 0.439 |
| t1 61.7 s +25 | 0.972 / 0.972 | 0.771 / 0.775 | 0.061 / 0.090 |
| t2 23.9 s +25 | 0.519 / 0.511 | 0.656 / 0.656 | 0.264 / 0.294 |

Reading: a port at parity, not an improvement. The wakeup kick difference is within Demucs' variation (seeds 1–5:
F1 0.859–0.931; the baseline's saved stems give 77 kicks, two new runs without a seed gave 69 and 75). The lead is
0.01–0.03 lower in 3 of 4 windows. The bass pitch is still bad in remember/t1 (pitch class 0.01–0.11 in both), the
known sampler problem. ~12 s per 25 s window on CPU.

## Quality debt (open) — the owner listened and rejected it

2026-11-16: the owner opened the wakeup `.mid` in the Core: *"very bad"*, *"chaotic, nothing makes sense"*, *"it
should be like listening to Guitar Pro"*. His decision: S2 goes into `main` as **plumbing** (the contract is proven)
and the quality stays **pending**. **Acceptance: sound like a Guitar Pro tab**, i.e. clean parts per instrument,
quantised rhythm and musical lengths, no rubbish. F1 alone is not enough.

Diagnosis measured note by note (seed 42, windows above, `D:\AICore\historico\coord-scratch-s1b-s2-s5\s2`):

| problem | measured |
|---|---|
| grid early | kick/bass starts **16–36 ms early** (median per window; bass with ~0 spread, i.e. the grid phase is wrong, not the notes). At 210 BPM, 36 ms is almost 1/32. Likely cause: the kick onset time is the start of the 2048 FFT window, not its centre |
| lead off by an octave | wakeup: 26 of 27 matched notes are **-12**; t1: +12/+7; remember: -12/-13 |
| ghost notes | wakeup lead: 96 made for 30 true (69 too many); bass: 30–61 too many per window, with harmonics (+7, +19) |
| bass a semitone low | remember and t1: most matched notes are **-1** (does the template sampler sound a semitone below the MIDI note?). t2: right |
| lost kicks | t2: 73 made for 196 true |

Proposed order, no new licences: (1) fix the grid phase (onset at the window centre + local tuning per kick);
(2) monophonic lead per stem with octave correction against the stem's register and a 1/16 minimum length;
(3) bass kept to the low register, no harmonics; (4) higher note thresholds. Better models (MuScriptor for bass/lead,
ADTOF for drums) have **non-commercial** weights and need permission to download.

**Not proven here [H]:** import into the Core (the `.mid` reads back at 960 with a tempo map, but it still has to be
opened in the Core), the owner listening, KBBB loops (not found on this machine), comparison with muscriptor
(blocked by the Vulkan SDK + HF weights), note F1 with offset (mir_eval not installed).
