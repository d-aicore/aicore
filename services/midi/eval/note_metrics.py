"""S2 quality metrics (P0): pitch-strict, offset-aware note scores on a grid tolerance, no mir_eval.

Notes are (onset_s, pitch, duration_s). A made note matches a true note when
  - its onset is within `tol` seconds (tol = half a grid cell: 1/16 or 1/32 at the template's BPM),
  - (pitch-strict) its pitch is the same MIDI note (an octave error is a miss),
  - (offset-aware) its end is within max(50 ms, 20 % of the true duration) of the true end
    (the mir_eval / MIREX note-with-offset rule, written here so nothing new is installed).
Matching is one-to-one, greedy on the smallest onset error over all candidate pairs (closer to the optimal
assignment than the headline's true-order greedy; on these tolerances the two agree to a few notes).

Junk ratio = made / true (1.0 = as many notes as the truth; the plan's proposed target is <= 1.2).
"""
from __future__ import annotations

from collections import Counter


def match(true, pred, tol, pitch_strict=False, offset=False, offset_tol=None):
    """Returns [(i_true, j_pred, onset_error_s)] one-to-one. offset_tol=None: mir_eval end rule
    (max(50 ms, 20 % of the true duration)); a number: |end error| <= offset_tol s (the owner's protocol, §11)."""
    pairs = []
    for i, (t, p, d) in enumerate(true):
        for j, (u, q, e) in enumerate(pred):
            dt = u - t
            if abs(dt) > tol:
                continue
            if pitch_strict and q != p:
                continue
            if offset and abs((u + e) - (t + d)) > (max(0.05, 0.2 * d) if offset_tol is None else offset_tol):
                continue
            pairs.append((abs(dt), i, j, dt))
    pairs.sort()
    ut, up, out = set(), set(), []
    for _, i, j, dt in pairs:
        if i in ut or j in up:
            continue
        ut.add(i)
        up.add(j)
        out.append((i, j, dt))
    return out


def prf(n_hit, n_true, n_pred):
    r = n_hit / max(n_true, 1)
    p = n_hit / max(n_pred, 1)
    return {"tp": n_hit, "recall": round(r, 3), "precision": round(p, 3),
            "f1": round(2 * r * p / (r + p), 3) if r + p else 0.0}


def median(xs):
    xs = sorted(xs)
    if not xs:
        return None
    m = len(xs) // 2
    return xs[m] if len(xs) % 2 else 0.5 * (xs[m - 1] + xs[m])


def scores(true, pred, tol):
    """All P0 numbers for one part at one tolerance."""
    onset = match(true, pred, tol)
    strict = match(true, pred, tol, pitch_strict=True)
    strict_off = match(true, pred, tol, pitch_strict=True, offset=True)
    errs = [dt for _, _, dt in onset]
    intervals = Counter(pred[j][1] - true[i][1] for i, j, _ in onset)
    return {"toleranceMs": round(tol * 1000, 2), "true": len(true), "made": len(pred),
            "junkRatio": round(len(pred) / max(len(true), 1), 3),
            "onsetOnly": prf(len(onset), len(true), len(pred)),
            "pitchStrict": prf(len(strict), len(true), len(pred)),
            "pitchStrictOffset": prf(len(strict_off), len(true), len(pred)),
            "onsetErrorMedianMs": round(median(errs) * 1000, 2) if errs else None,
            "onsetErrorMeanMs": round(sum(errs) / len(errs) * 1000, 2) if errs else None,
            "pitchIntervals": {str(k): v for k, v in intervals.most_common(6)}}


def protocol(true, pred, tol, tps):
    """The owner's A/B protocol (MIDI-MAKER.md §11): pitch exact, onset and offset within +-tol s,
    greedy 1:1. Counts, P/R, F1 onset/full, junk, ghost/missed shares, |end error| median/p90 in ticks,
    pitch residuals of onset-matched pairs (pitch ignored)."""
    import numpy as np
    on = match(true, pred, tol, pitch_strict=True)
    full = match(true, pred, tol, pitch_strict=True, offset=True, offset_tol=tol)
    anyp = match(true, pred, tol)
    D, T = len(pred), len(true)
    dend = [abs((pred[j][0] + pred[j][2]) - (true[i][0] + true[i][2])) * tps for i, j, _ in on]
    res = Counter(pred[j][1] - true[i][1] for i, j, _ in anyp)
    tp = len(on)
    return {"D": D, "T": T, "TP": tp, "FP": D - tp, "FN": T - tp,
            "P": round(tp / D, 3) if D else 0.0, "R": round(tp / T, 3) if T else 0.0,
            "F1onset": round(2 * tp / (D + T), 3) if D + T else 0.0,
            "F1full": round(2 * len(full) / (D + T), 3) if D + T else 0.0,
            "J": round(D / T, 3) if T else None, "ghost": round((D - tp) / D, 3) if D else 0.0,
            "missed": round((T - tp) / T, 3) if T else 0.0,
            "endErrTicksMed": round(float(np.median(dend)), 1) if dend else None,
            "endErrTicksP90": round(float(np.percentile(dend, 90)), 1) if dend else None,
            "pitchResiduals": {str(k): v for k, v in res.most_common(8)}}
