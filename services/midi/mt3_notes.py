"""Notes of one or more WAVs with YourMT3 (YPTF.MoE+Multi noPS, pretrained, CPU, no training).

Runs in its own env (D:\\AICore\\models\\mt3-env) against the code + weights in D:\\AICore\\models\\mt3-src
(the Hugging Face space mimbres/YourMT3). Loads the model once, then for each "<in.wav>=<out.json>" pair
writes {"notes": [[onset_s, offset_s, pitch, program, is_drum, velocity], ...], "seconds": wall}.
Logs on stderr only; one JSON line on stdout at the end.

  python mt3_notes.py <in.wav>=<out.json> [...]
"""
from __future__ import annotations

import json
import os
import sys
import time

SRC = os.environ.get("AICORE_MT3_SRC", r"D:\AICore\models\mt3-src")
CHECKPOINT = "mc13_256_g4_all_v7_mt3f_sqr_rms_moe_wf4_n8k2_silu_rope_rp_b36_nops@last.ckpt"
ARGS = [CHECKPOINT, "-p", "2024", "-tk", "mc13_full_plus_256", "-dec", "multi-t5", "-nl", "26",
        "-enc", "perceiver-tf", "-sqr", "1", "-ff", "moe", "-wf", "4", "-nmoe", "8", "-kmoe", "2",
        "-act", "silu", "-epe", "rope", "-rp", "1", "-ac", "spec", "-hop", "300", "-atc", "1", "-pr", "32"]
os.environ.setdefault("CUDA_VISIBLE_DEVICES", "")  # CPU only: the GPU belongs to ACE training


def stub_mir_eval():
    """YourMT3 imports mir_eval only for its own training metrics (utils/metrics_helper.py). Inference does not
    call it, and installing mir_eval needs the owner's OK, so the names are stubbed: any call raises."""
    import types

    def missing(*_a, **_k):
        raise RuntimeError("mir_eval is not installed (metrics are not used for inference)")
    names = {"transcription": ["precision_recall_f1_overlap"], "multipitch": ["evaluate"],
             "melody": ["to_cent_voicing", "raw_pitch_accuracy", "raw_chroma_accuracy", "overall_accuracy"],
             "util": ["midi_to_hz"]}
    root = types.ModuleType("mir_eval")
    sys.modules["mir_eval"] = root
    for sub, fns in names.items():
        m = types.ModuleType(f"mir_eval.{sub}")
        for f in fns:
            setattr(m, f, missing)
        setattr(root, sub, m)
        sys.modules[f"mir_eval.{sub}"] = m


def read_wav_mono(path):
    """(mono float32 numpy, rate) from a PCM 16/24/32 or float32 WAV; channels averaged."""
    import struct
    import numpy as np
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
    width = bits // 8
    raw = raw[: len(raw) // (width * ch) * width * ch]
    if (tag, bits) == (3, 32):
        x = np.frombuffer(raw, "<f4").astype(np.float32)
    elif (tag, bits) == (1, 16):
        x = np.frombuffer(raw, "<i2").astype(np.float32) / 32768
    elif (tag, bits) == (1, 32):
        x = np.frombuffer(raw, "<i4").astype(np.float32) / 2147483648.0
    elif (tag, bits) == (1, 24):
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        x = (np.where(v & 0x800000, v - 0x1000000, v) / 8388608.0).astype(np.float32)
    else:
        raise SystemExit(f"unsupported WAV tag {tag} / {bits} bit")
    return np.ascontiguousarray(x.reshape(-1, ch).mean(1), dtype=np.float32), rate


def stub_wandb():
    """ymt3.py imports wandb (experiment logging, training only); inference never touches it."""
    import types
    if "wandb" not in sys.modules:
        m = types.ModuleType("wandb")
        m.Table = lambda *a, **k: None
        sys.modules["wandb"] = m


def main():
    stub_mir_eval()
    stub_wandb()
    real_stdout = sys.stdout
    sys.stdout = sys.stderr
    os.chdir(SRC)
    sys.path.insert(0, os.path.join(SRC, "amt", "src"))
    sys.path.insert(0, SRC)
    import torch
    import torchaudio
    torch.set_num_threads(int(os.environ.get("AICORE_MIDI_THREADS", "4")))
    from model_helper import load_model_checkpoint
    from utils.audio import slice_padded_array
    from utils.note2event import mix_notes
    from utils.event2note import merge_zipped_note_events_and_ties_to_notes
    t0 = time.monotonic()
    model = load_model_checkpoint(args=ARGS, device="cpu")
    load_s = time.monotonic() - t0
    done = []
    for pair in sys.argv[1:]:
        wav, out = pair.split("=", 1)
        t1 = time.monotonic()
        mono, sr = read_wav_mono(wav)  # torchaudio.load now needs torchcodec (not installed)
        audio = torch.from_numpy(mono).unsqueeze(0)
        audio = torchaudio.functional.resample(audio, sr, model.audio_cfg["sample_rate"])
        segs = slice_padded_array(audio, model.audio_cfg["input_frames"], model.audio_cfg["input_frames"])
        segs = torch.from_numpy(segs.astype("float32")).unsqueeze(1)
        with torch.no_grad():
            pred, _ = model.inference_file(bsz=8, audio_segments=segs)
        starts = [model.audio_cfg["input_frames"] * i / model.audio_cfg["sample_rate"] for i in range(segs.shape[0])]
        per_ch = []
        for ch in range(model.task_manager.num_decoding_channels):
            zipped, _, _ = model.task_manager.detokenize_list_batches([a[:, ch, :] for a in pred], starts,
                                                                      return_events=True)
            notes_ch, _ = merge_zipped_note_events_and_ties_to_notes(zipped)
            per_ch.append(notes_ch)
        notes = mix_notes(per_ch)
        rows = [[float(n.onset), float(n.offset), int(n.pitch), int(n.program), bool(n.is_drum), int(n.velocity)]
                for n in notes]
        wall = round(time.monotonic() - t1, 2)
        with open(out, "w", encoding="utf-8") as f:
            json.dump({"notes": rows, "seconds": wall, "checkpoint": CHECKPOINT, "torch": torch.__version__}, f)
        done.append({"wav": wav, "notes": len(rows), "seconds": wall})
    real_stdout.write(json.dumps({"event": "done", "loadSeconds": round(load_s, 2), "files": done}) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
