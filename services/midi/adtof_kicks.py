"""Kick (and other drum) onsets of WAV files with ADTOF (PyTorch port, bundled pretrained weights, CPU only).

Runs in D:\\AICore\\models\\adtof-env (torch CPU + librosa + pretty_midi + adtof_pytorch from D:\\AICore\\models\\adtof-src).
For each "<in.wav>=<out.json>" writes {"onsets": {"<midi pitch>": [seconds...]}, "seconds": wall}.
ADTOF's 5 classes come out as MIDI pitches (35 = bass drum). One JSON line on stdout at the end.

  python adtof_kicks.py <in.wav>=<out.json> [...]
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import time

os.environ.setdefault("CUDA_VISIBLE_DEVICES", "")  # CPU only: the GPU belongs to ACE training


def main():
    real_stdout = sys.stdout
    sys.stdout = sys.stderr
    import pretty_midi
    import torch
    from adtof_pytorch import transcribe_to_midi
    torch.set_num_threads(int(os.environ.get("AICORE_MIDI_THREADS", "4")))
    done = []
    for pair in sys.argv[1:]:
        wav, out = pair.split("=", 1)
        t0 = time.monotonic()
        with tempfile.TemporaryDirectory() as td:
            mid = os.path.join(td, "drums.mid")
            transcribe_to_midi(wav, mid, device="cpu")
            pm = pretty_midi.PrettyMIDI(mid)
        onsets = {}
        for inst in pm.instruments:
            for n in inst.notes:
                onsets.setdefault(str(n.pitch), []).append(round(float(n.start), 5))
        for k in onsets:
            onsets[k].sort()
        wall = round(time.monotonic() - t0, 2)
        with open(out, "w", encoding="utf-8") as f:
            json.dump({"onsets": onsets, "seconds": wall}, f)
        done.append({"wav": wav, "counts": {k: len(v) for k, v in onsets.items()}, "seconds": wall})
    real_stdout.write(json.dumps({"event": "done", "files": done}) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
