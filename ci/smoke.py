"""CI smoke tests of the native exes (no GPU, no plugins, no network).

    python ci/smoke.py <AICoreC.exe> <aicore-render.exe> <work folder>

1. AICoreC --version / --self-test.
2. AICoreC --sample-test on a generated WAV (SAMPLE edits, WAV/FLAC export, save sample; MP3 needs AICore's Python
   and is allowed to fail here only).
3. aicore-render hello, and a render with AICore's own sampler (a generated kick, 8 notes at PPQ 960): done, a WAV that
   is not silent, the manifest's sha256 matches.
4. aicore-render refuses a PPQ 480 MIDI (UNSUPPORTED_FORMAT).
5. Controls: each check is run once against a broken input and must go red (a silent WAV makes --sample-test's
   normalize fail; a silent kit must give BACKEND_FAILED; one changed byte must break the sha256 match).
Exit 0 only when every check passes. These are AICore's own tests, written for AICore.
"""
from __future__ import annotations

import hashlib
import json
import math
import struct
import subprocess
import sys
import time
import wave
from pathlib import Path


def tone(path: Path, seconds: float, rate: int = 44100, kick: bool = False) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(int(rate * seconds)):
            t = i / rate
            if kick:
                v = math.sin(2 * math.pi * (50 + 120 * math.exp(-t * 30)) * t) * math.exp(-t * 8)
            else:
                v = 0.5 * math.sin(2 * math.pi * 220 * t) * (0.6 + 0.4 * math.sin(2 * math.pi * 0.5 * t))
            s = struct.pack("<h", int(v * 30000))
            frames += s + s
        w.writeframes(bytes(frames))


def vlq(n: int) -> bytes:
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(out))


def midi(path: Path, ppq: int, notes: int = 8, bpm: float = 145) -> None:
    data = vlq(0) + b"\xff\x51\x03" + int(60_000_000 / bpm).to_bytes(3, "big")
    for i in range(notes):
        data += vlq(0 if i == 0 else ppq // 2) + bytes([0x90, 36, 110]) + vlq(ppq // 2) + bytes([0x80, 36, 0])
    data += vlq(0) + b"\xff\x2f\x00"
    path.write_bytes(b"MThd" + struct.pack(">IHHH", 6, 0, 1, ppq) + b"MTrk" + struct.pack(">I", len(data)) + data)


def run(cmd, stdin=None):
    p = subprocess.run(cmd, input=stdin, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=600)
    return p.returncode, p.stdout, p.stderr


def main(argv) -> int:
    app, render, work = Path(argv[0]).resolve(), Path(argv[1]).resolve(), Path(argv[2]).resolve()  # the contract wants absolute paths
    work = work / time.strftime("run-%Y%m%d-%H%M%S")  # a fresh folder per run: outputDirs are never reused
    work.mkdir(parents=True, exist_ok=True)
    fails = []

    def check(ok: bool, what: str) -> None:
        print(("ok   " if ok else "FAIL ") + what, flush=True)
        if not ok:
            fails.append(what)

    code, out, _ = run([str(app), "--version"])
    check(code == 0 and out.strip().startswith("0.1.0+") and ".modified" not in out, "AICoreC --version: " + out.strip())
    code, out, _ = run([str(app), "--self-test"])
    check(code == 0 and out.startswith("ok:"), "AICoreC --self-test: " + out.strip())

    wav = work / "tone.wav"
    tone(wav, 4.0)
    code, out, _ = run([str(app), "--sample-test", str(wav), str(work / "sample-test")])
    lines = [ln for ln in out.splitlines() if ln.strip()]
    failed = [ln for ln in lines if ln.startswith("FAIL") and "export mp3" not in ln]
    check(len(lines) >= 14 and not failed, f"AICoreC --sample-test: {len(lines)} checks, failed {failed}")

    code, out, _ = run([str(render), "hello"])
    hello = json.loads(out.splitlines()[-1]) if out.strip() else {}
    check(code == 0 and hello.get("ppq") == 960, "aicore-render hello: ppq 960")

    kick = work / "kick.wav"
    tone(kick, 0.4, kick=True)
    mid = work / "beat.mid"
    midi(mid, 960)
    outdir = work / "render-out"
    outdir.mkdir(exist_ok=True)
    body = {"schemaVersion": "1.0", "requestId": "ci-sampler", "profile": "render", "operation": "render",
            "params": {"engine": "sampler", "kit": [{"note": 36, "sample": str(kick)}], "sampleRate": 44100,
                       "blockSize": 512, "tailSeconds": 1},
            "inputs": [{"role": "midi", "path": str(mid)}], "outputDir": str(outdir)}
    code, out, err = run([str(render), "job"], json.dumps(body) + "\n")
    last = json.loads(out.splitlines()[-1]) if out.strip() else {}
    ok = code == 0 and last.get("event") == "done"
    if ok:
        wav_out = next(o for o in last["result"]["outputs"] if o["role"] == "audio")
        sha = hashlib.sha256(Path(wav_out["path"]).read_bytes()).hexdigest()
        ok = sha == wav_out["sha256"] and last["result"]["metrics"]["peakDbfs"] > -60
    check(ok, "aicore-render own sampler: done, sha256 matches, not silent " + (err[-300:] if not ok else ""))

    # ---- audit 2026-11-16 fixes, each seen from outside the exe ----
    res = last.get("result", {}) if ok else {}
    mp = Path(res.get("manifestPath", work / "none"))
    check(ok and mp.is_file() and hashlib.sha256(mp.read_bytes()).hexdigest() == res.get("manifestSha256")
          and mp.stat().st_size == res.get("manifestBytes"), "aicore-render done line carries manifest.json's sha256")
    before = hashlib.sha256((outdir / "render.wav").read_bytes()).hexdigest() if ok else ""
    reuse = dict(body, requestId="ci-reuse")
    code, out, _ = run([str(render), "job"], json.dumps(reuse) + "\n")
    last2 = json.loads(out.splitlines()[-1]) if out.strip() else {}
    check(code == 2 and last2.get("error", {}).get("code") == "OUTPUT_EXISTS" and ok
          and hashlib.sha256((outdir / "render.wav").read_bytes()).hexdigest() == before,
          "aicore-render refuses an outputDir that holds a published job (OUTPUT_EXISTS, earlier render intact)")
    big = work / "render-big"
    big.mkdir(exist_ok=True)
    huge = dict(body, requestId="ci-big", outputDir=str(big),
                params=dict(body["params"], sampleRate=96000, lengthSeconds=600))
    code, out, _ = run([str(render), "job"], json.dumps(huge) + "\n")
    last2 = json.loads(out.splitlines()[-1]) if out.strip() else {}
    check(code == 2 and last2.get("error", {}).get("code") == "LIMIT_EXCEEDED" and not any(big.iterdir()),
          "aicore-render: 600 s at 96 kHz is LIMIT_EXCEEDED before the buffer is allocated")
    code, out, _ = run([str(render), "job"], "{" + " " * (2 * 1024 * 1024 + 10) + "}\n")
    last2 = json.loads(out.splitlines()[-1]) if out.strip() else {}
    check(code == 2 and last2.get("error", {}).get("code") == "LIMIT_EXCEEDED", "aicore-render: a request line over 2 MiB")
    code, out, _ = run([str(render), "roles", "set"])
    check(code != 0 and "usage" in out, "aicore-render roles: arity checked before use")
    try:
        p = subprocess.run([str(app), "--grab"], capture_output=True, text=True, timeout=60)
        check(p.returncode == 2 and "needs a .png path" in p.stdout, "AICoreC --grab without a path ends with exit 2")
    except subprocess.TimeoutExpired:
        check(False, "AICoreC --grab without a path ends with exit 2 (it hung)")

    mid480 = work / "beat480.mid"
    midi(mid480, 480)
    out480 = work / "render-480"
    out480.mkdir(exist_ok=True)
    body["requestId"], body["inputs"], body["outputDir"] = "ci-480", [{"role": "midi", "path": str(mid480)}], str(out480)
    code, out, _ = run([str(render), "job"], json.dumps(body) + "\n")
    last = json.loads(out.splitlines()[-1]) if out.strip() else {}
    check(code == 2 and last.get("error", {}).get("code") == "UNSUPPORTED_FORMAT" and not any(out480.glob("*.wav")),
          "aicore-render refuses PPQ 480 (UNSUPPORTED_FORMAT, nothing written)")

    # ---- controls: each check above must go red when its feature is broken (AICore's rule: a test that cannot fail
    # tests nothing). A broken input stands in for the broken feature.
    silent = work / "silent.wav"
    with wave.open(str(silent), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(44100)
        w.writeframes(b"\x00" * 4 * 44100 * 2)
    code, out, _ = run([str(app), "--sample-test", str(silent), str(work / "sample-test-silent")])
    check(code != 0 and "FAIL: normalize" in out, "control: --sample-test goes red when normalize cannot reach -1 dB (silent input)")

    out_silent = work / "render-silent"
    out_silent.mkdir(exist_ok=True)
    body["requestId"], body["outputDir"] = "ci-silent", str(out_silent)
    body["inputs"] = [{"role": "midi", "path": str(mid)}]
    body["params"]["kit"] = [{"note": 36, "sample": str(silent)}]
    code, out, _ = run([str(render), "job"], json.dumps(body) + "\n")
    last = json.loads(out.splitlines()[-1]) if out.strip() else {}
    check(code == 2 and last.get("error", {}).get("code") == "BACKEND_FAILED" and not any(out_silent.glob("*.wav")),
          "control: a silent render is BACKEND_FAILED, never a done (silent kit)")

    tampered = work / "tampered.wav"
    good = next((outdir.glob("render.wav")), None)
    if good is not None:
        data = bytearray(good.read_bytes())
        data[-1] ^= 0xFF
        tampered.write_bytes(bytes(data))
        manifest = json.loads((outdir / "manifest.json").read_text(encoding="utf-8"))
        want = next(o["sha256"] for o in manifest["outputs"] if o["role"] == "audio")
        check(hashlib.sha256(tampered.read_bytes()).hexdigest() != want,
              "control: the sha256 check catches one changed byte in render.wav")
    else:
        check(False, "control: no render.wav to tamper with")

    print(f"{len(fails)} failed" if fails else "all smoke checks passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
