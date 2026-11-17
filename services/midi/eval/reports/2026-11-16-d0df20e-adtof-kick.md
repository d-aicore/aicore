# ADTOF for the kick — 2026-11-16, base `d0df20e`

Append-only. Diogo authorized the ADTOF download (11:50, thread 11:52). Pretrained only, CPU, no GPU.

## Model
- Code + bundled weights: `github.com/xavriley/ADTOF-pytorch` @ `85c192e78f716ea0b111cc8a5ee4a8f6a3a4f8a9`
  in `D:\AICore\models\adtof-src`; weights `adtof_frame_rnn_pytorch_weights.pth` sha256
  `1bc986e596ec47ba0b44916f87cd4a39f0b2bec23596df3fb5d0e87749217320`. PyTorch port of ADTOF (Zehren et al.).
- Env `D:\AICore\models\adtof-env`: python 3.11.18, torch 2.15.0+cpu, librosa, pretty_midi.
- Runner `services/midi/adtof_kicks.py` (~0.5 s per 25–30 s window after the first load).

## Kick, F1 onset at 1/32 (template renders, drums stem of one Demucs run)
| arm | remember | wakeup | t1 | t2 | mean |
|---|---|---|---|---|---|
| our onset detector (P1) | 0.762 | 0.951 | 0.972 | 0.509 | 0.798 |
| ADTOF on the drums stem | **0.938** | **0.988** | 0.948 | 0.486 | **0.840** |
| ours ∪ ADTOF | 0.828 | 0.988 | 0.966 | 0.515 | 0.824 |
ADTOF on the full mix is worse (t2 P 0.42). ADTOF does not catch the t2 1/32 kick roll either (R 0.32).

## Service change
`adtof_kicks()` runs ADTOF on the drums stem when `D:\AICore\models\adtof-env` exists; otherwise (or on failure)
the onset detector runs and `metrics.kickDetector` says which (`adtof` / `onset (ADTOF not installed)` /
`onset (ADTOF failed)`). `environment.kickModel` records it too. No contract change.

## Service results at 1/32 (score_templates, leadFilter off), kick F1 (all metrics agree: pitch is fixed)
remember 0.783 → **0.931**, wakeup 0.882 → **0.982**, t1 0.972 → 0.954, t2 0.506 → 0.486. Bass unchanged.

## Controlled set (Vita + synthetic kick), 1/32, R / P / F1 full
- kick: remember 0.99 / 1.00 / 0.99, wakeup 0.98 / 1.00 / 0.99, t1 0.97 / 1.00 / 0.96, t2 0.33 / 1.00 / 0.49
- bass: remember 0.95 / 0.80 / 0.87, wakeup 0.98 / 0.74 / 0.84 (J 1.31, was 1.18), t1 0.80 / 0.91 / **0.84** (was 0.00),
  t2 0.28 / 0.95 / 0.42
- lead: remember 0.99 / 0.93, wakeup 1.00 / 0.81 (J 1.23), t1 0.76 / 0.80 / 0.44, t2 0.45 / 0.74 / 0.34

## Gates
Contract: manifest 0, gate 0, noop 1, MIDI gate 7/7 + canaries 2/2 with ADTOF active (same request → same sha),
noop 1. Unit tests 12/12; mutations 9/9 caught (new: silent ADTOF fallback without a label).
Grey zone stays: wakeup lead J 1.23 and now wakeup bass J 1.31.

## Listening set
`D:\AICore\exports\midi\midi-maker-p7-2026-11-16\`.
