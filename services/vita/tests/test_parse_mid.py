"""The Vita service's MIDI reader (audit 2026-11-16). Stdlib only: the vita module is not needed.

  python -m unittest discover -s services/vita/tests -v
"""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import aicore_vita_service as v  # noqa: E402
import media  # noqa: E402  (contracts/sim, put on the path by the service)


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(out))


def track(events):
    """events: [(tick, bytes)] -> MTrk with end-of-track."""
    body, last = b"", 0
    for tick, ev in sorted(events, key=lambda e: e[0]) + [(max([0] + [e[0] for e in events]), b"\xFF\x2F\x00")]:
        body += vlq(tick - last) + ev
        last = tick
    return b"MTrk" + struct.pack(">I", len(body)) + body


def smf(tracks, fmt=1, ntrks=None, ppq=960):
    return b"MThd" + struct.pack(">IHHH", 6, fmt, len(tracks) if ntrks is None else ntrks, ppq) + b"".join(tracks)


def tempo(bpm):
    return b"\xFF\x51\x03" + int(round(60e6 / bpm)).to_bytes(3, "big")


def on(p, vel=100):
    return bytes([0x90, p, vel])


def off(p):
    return bytes([0x80, p, 0])


class ParseMid(unittest.TestCase):
    def test_tempo_change_moves_later_notes(self):
        # 120 BPM for the first quarter (0.5 s), then 60 BPM: a note at tick 1920 starts at 0.5 + 1.0 = 1.5 s
        data = smf([track([(0, tempo(120)), (960, tempo(60))]),
                    track([(0, on(60)), (480, off(60)), (1920, on(62)), (2880, off(62))])])
        bpm, notes = v.parse_mid(data)
        self.assertEqual(bpm, 120.0)
        self.assertAlmostEqual(notes[0][0], 0.0)
        self.assertAlmostEqual(notes[0][1], 0.25)
        self.assertAlmostEqual(notes[1][0], 1.5)
        self.assertAlmostEqual(notes[1][1], 1.0)  # one quarter at 60 BPM

    def test_overlapping_same_pitch_are_two_notes(self):
        data = smf([track([(0, tempo(120))]),
                    track([(0, on(60)), (480, on(60)), (960, off(60)), (1440, off(60))])])
        _, notes = v.parse_mid(data)
        self.assertEqual(len(notes), 2)
        self.assertEqual([round(n[0], 3) for n in notes], [0.0, 0.25])
        self.assertEqual([round(n[1], 3) for n in notes], [0.5, 0.5])

    def test_declared_track_count_must_match(self):
        data = smf([track([(0, tempo(120))]), track([(0, on(60)), (480, off(60))])], ntrks=3)
        with self.assertRaises(v.ContractError) as e:
            v.parse_mid(data)
        self.assertEqual(e.exception.code, "UNSUPPORTED_FORMAT")

    def test_smf_format_2_refused(self):
        data = smf([track([(0, tempo(120)), (0, on(60)), (480, off(60))])], fmt=2)
        with self.assertRaises(v.ContractError):
            v.parse_mid(data)

    def test_truncated_track_refused(self):
        data = smf([track([(0, tempo(120))]), track([(0, on(60)), (480, off(60))])])
        with self.assertRaises(v.ContractError):
            v.parse_mid(data[:-2])

    def test_ppq_480_refused(self):
        data = smf([track([(0, tempo(120)), (0, on(60)), (480, off(60))])], ppq=480)
        with self.assertRaises(v.ContractError):
            v.parse_mid(data)

    def test_midi_over_8_mib_refused_before_reading(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "big.mid"
            with open(p, "wb") as f:
                f.truncate(media.MAX_MIDI_BYTES + 1)
            with self.assertRaises(media.MediaError) as e:
                v.read_mid(p)
            self.assertEqual(e.exception.code, "LIMIT_EXCEEDED")


if __name__ == "__main__":
    unittest.main()
