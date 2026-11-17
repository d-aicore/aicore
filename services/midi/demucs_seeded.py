"""Run core_stems.py (READ-ONLY, AICORE repo) with the request's seed.

htdemucs `apply_model(shifts=1)` draws a random time shift per call, so the same input gives different
stems run to run (measured on the wakeup window: 69 vs 75 kick onsets from two identical calls). The
contract has `params.seed`; this wrapper seeds Python/numpy/torch and then runs core_stems.py unchanged
as __main__, so the same request gives the same stems.

  python demucs_seeded.py <seed> <core_stems.py> separate --input ... --out ...
"""
import random
import runpy
import sys

seed = int(sys.argv[1])
script = sys.argv[2]
random.seed(seed)
try:
    import numpy
    numpy.random.seed(seed)
except ImportError:
    pass
try:
    import torch
    torch.manual_seed(seed)
except ImportError:
    pass
sys.argv = [script] + sys.argv[3:]
runpy.run_path(script, run_name="__main__")
