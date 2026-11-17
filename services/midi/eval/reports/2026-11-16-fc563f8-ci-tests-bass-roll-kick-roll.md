# CI tests, bass 1/32 rolls and the t2 kick roll — 2026-11-16, base `fc563f8`

Append-only. English from now on (Diogo's rule for app docs). No downloads, no training, no GPU.

## CI unit tests (`6c47b38`)
- `services/midi/tests/test_midi_maker_units.py`: 11 tests, ~0.1 s, CPU only, numpy (+ mido optional).
- `services/midi/tests/mutations.py`: 8 in-process mutations; every one must make the suite fail. Result: 8/8 caught,
  clean run passes. Checked against the released code (`851d095`): the old kick timing is 42 ms early on the
  synthetic kick test, and the old circular-mean phase is 31 ms off on the rolling-kick phase test.
- The heavy contract gate (`gate_service.py`) needs Demucs + Basic Pitch and stays a local gate.

## Bass 1/32 rolls (tried, reverted)
The t2 bassline is half 1/32 notes (134 of 322 spaced 120 ticks); `split_on_onsets` only cuts on 1/16.
Controlled set, bass at 1/32 (D / P / R / F1 full), baseline `fc563f8` first:
| rule for the 1/32 off-line | remember | wakeup | t1 | t2 |
|---|---|---|---|---|
| baseline (no off-lines) | 266 / 0.81 / 0.95 / 0.81 | 288 / 0.83 / 0.97 / 0.89 | 224 / 0.95 / 0.81 / 0.00 | 109 / 0.94 / 0.32 / 0.46 |
| energy x2, fine RMS gate | 225 / 0.95 / 0.95 / 0.87 | 261 / 0.91 / 0.97 / 0.87 | 224 / 0.95 / 0.81 / 0.00 | **29** / 0.79 / **0.07** / 0.11 |
| energy x2, RMS attack | 414 / 0.52 / 0.95 / 0.23 | 457 / 0.52 / 0.97 / 0.20 | 368 / 0.58 / 0.82 / 0.44 | 192 / 0.55 / 0.33 / 0.41 |
| energy x2, zero-phase envelope | 486 / 0.44 / 0.95 / 0.05 | 528 / 0.45 / 0.97 / 0.00 | 428 / 0.51 / 0.82 / 0.56 | 220 / 0.48 / 0.33 / 0.38 |
| Basic Pitch onset near the off-line | 269 / 0.80 / 0.95 / 0.79 | 294 / 0.81 / 0.97 / 0.86 | 225 / 0.95 / 0.81 / 0.00 | 110 / 0.93 / 0.32 / 0.46 |
None raises t2 recall without doubling notes elsewhere. Reverted; the service is unchanged.

## t2 kick roll
129 of the 196 t2 kicks are a 1/32 roll; Demucs puts it in the bass stem (−11 dB there, −49 dB in drums).
Low-band attack ratios at grid lines do not separate roll kicks from bass notes (controlled: roll median 1.8,
bass-only 5.9, none 2.7; real render: 4.3 / 19.9 / 4.3). Detecting on drums+bass: no net gain (see the
lead-lengths report). Needs a dedicated drum transcriber (e.g. ADTOF: a download, needs Diogo's OK).

## New .mid set
None: the service did not change since `fc563f8`. The set to listen to is still
`D:\AICore\exports\midi\midi-maker-p6-2026-11-16\`.
