"""f0 track of one WAV with CREPE (torchcrepe, CPU only). Runs in its own env (D:\\AICore\\models\\crepe-env).

  python crepe_f0.py <in.wav> <out.npz> [--model full|tiny] [--hop-ms 5]

Writes times_s, f0_hz, periodicity (0..1), rms (per hop). Logs on stderr only; prints one JSON line on
stdout at the end ({"event": "done", ...}). No network: torchcrepe ships its weights inside the package.
"""
from __future__ import annotations

import argparse
import json
import os
import sys

os.environ.setdefault("CUDA_VISIBLE_DEVICES", "")  # CPU only: the GPU belongs to ACE training


def read_wav(path):
    import numpy as np
    import struct
    data = open(path, "rb").read()
    pos, fmt, raw = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, ch, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            if tag == 0xFFFE and len(body) >= 26:
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, ch, rate, bits)
        elif cid == b"data":
            raw = body
        pos += 8 + size + (size & 1)
    tag, ch, rate, bits = fmt
    if (tag, bits) == (3, 32):
        x = np.frombuffer(raw[: len(raw) // (4 * ch) * 4 * ch], "<f4")
    elif (tag, bits) == (1, 16):
        x = np.frombuffer(raw[: len(raw) // (2 * ch) * 2 * ch], "<i2").astype(np.float32) / 32768
    elif (tag, bits) == (1, 24):
        b = np.frombuffer(raw[: len(raw) // (3 * ch) * 3 * ch], np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        x = (np.where(v & 0x800000, v - 0x1000000, v) / 8388608.0).astype(np.float32)
    else:
        raise SystemExit(f"unsupported WAV tag {tag} / {bits} bit")
    return x.reshape(-1, ch).mean(1).astype(np.float32), rate


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("out")
    ap.add_argument("--model", default="full", choices=["full", "tiny"])
    ap.add_argument("--hop-ms", type=float, default=5.0)
    ap.add_argument("--fmin", type=float, default=40.0)  # >= CREPE lowest bin; fmin 30 gives garbage. Viterbi decoding returned ~fmax on every frame with torch 2.15 / torchcrepe here, so weighted_argmax
    ap.add_argument("--fmax", type=float, default=2000.0)
    ap.add_argument("--threads", type=int, default=int(os.environ.get("AICORE_MIDI_THREADS", "4")))
    a = ap.parse_args()
    real_stdout = sys.stdout
    sys.stdout = sys.stderr
    import numpy as np
    import torch
    import torchcrepe
    torch.set_num_threads(a.threads)
    x, rate = read_wav(a.wav)
    sr = 16000
    n = int(round(len(x) * sr / rate))
    x16 = np.interp(np.linspace(0, len(x) - 1, n), np.arange(len(x)), x).astype(np.float32)
    hop = int(sr * a.hop_ms / 1000)
    audio = torch.from_numpy(x16)[None]
    with torch.no_grad():
        f0, per = torchcrepe.predict(audio, sr, hop, a.fmin, a.fmax, a.model, batch_size=512, device="cpu",
                                     return_periodicity=True, decoder=torchcrepe.decode.weighted_argmax, pad=True)
    f0, per = f0[0].numpy(), per[0].numpy()
    frames = len(f0)
    times = np.arange(frames) * hop / sr
    rms = np.array([float(np.sqrt(np.mean(x16[i * hop: i * hop + hop] ** 2) + 1e-12)) if i * hop < len(x16) else 0.0
                    for i in range(frames)])
    np.savez(a.out, times=times, f0=f0, periodicity=per, rms=rms)
    real_stdout.write(json.dumps({"event": "done", "frames": frames, "model": a.model, "hopMs": a.hop_ms,
                                  "torch": torch.__version__}) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
