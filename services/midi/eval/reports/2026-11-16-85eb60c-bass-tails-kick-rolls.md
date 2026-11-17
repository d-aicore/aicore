# Bass release tails, kick rolls, wakeup lead — 2026-11-16, base `85eb60c` (main)

Append-only. Branch `s2q/rolls-extra-notes`. CPU only, no downloads, no training.

## 1. Bass release tails (kept)
Wakeup controlled had 83 extra bass notes; 80 were exactly one 1/16 after a true note, same pitch: the release
tail of the note re-cut as a new note. Rule (declared, not tuned per song): a 1/16 step that is only a decaying
tail (no louder than the end of the previous step, and its end below 70 % of its start) is not a new note.
Controlled set, bass at 1/32, F1 full (J): remember 0.868 → **0.955** (1.18 → 0.98), wakeup 0.843 → **0.939**
(1.31 → 1.08), t1 0.838 → 0.855, t2 unchanged. Real renders: unchanged (remember onset F1 0.704 → 0.693, P up).

## 2. Kick rolls (kept)
Inside one beat, a 1/32 kick roll makes the 40–120 Hz envelope of drums + bass pulse at the 1/32 rate with no
1/16 component; single kicks, bass notes and 1/16 lines all carry a 1/16 component. `kick_rolls()` adds a kick on
the eight 1/32 lines of a beat when |C8| ≥ 0.05 and |C8| ≥ 4·|C4| (declared a priori; measured separation:
roll beats 17–81, all other beats ≤ 1.7 on the controlled set, ≤ 1.8 on the real renders).
Controlled t2 kick: R 0.33 → **0.94**, P 1.00. Real t2 kick F1 0.486 → 0.532 (1 roll beat found, P 1.00);
other renders: 0 roll beats, unchanged. On the real t2 render most of the roll has a long tail and does not pulse,
so it stays missed. `metrics.kickRollBeats` reports how many beats were filled.

## 3. Wakeup lead extra notes (tried, reverted)
All 7 extra lead notes on wakeup controlled were 15–30 semitones below the line (bass bleeding into `other`).
- Register ±19 around the line median: wakeup J 1.23 → 1.03, but t1 lead R 0.76 → 0.63 (t1's arp is low). Reverted.
- Drop lead notes that are +12/+19/+24 above a sounding bass note: same wakeup gain, t1 R 0.76 → 0.57. Reverted.
Wakeup lead stays a grey zone (J 1.23).

## Gates
Unit tests 18/18; mutations 17/17 caught (new: no tail rule, roll detector removed, roll detector on every beat).
Contract: manifest 0, gate 0, noop 1; MIDI gate 7/7 + 2/2, noop 1.
Listening set: `D:\AICore\exports\midi\midi-maker-p8-2026-11-16\`.
