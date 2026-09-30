import os
import subprocess
import sys
import tempfile
import unittest

import numpy as np
from scipy.io import wavfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from apply_over_ble import compute_payload, compute_sustained_payload
from ble_translator import decode_freq_range


def write_tone_wav(path, freq_hz, sample_rate=16000, duration_s=1.0):
    t = np.linspace(0, duration_s, int(sample_rate * duration_s), endpoint=False)
    samples = np.sin(2 * np.pi * freq_hz * t).astype(np.float32)
    wavfile.write(path, sample_rate, samples)


def write_two_tone_wav(path, freq1_hz, freq2_hz, sample_rate=16000, duration_each_s=1.0):
    """A short burst of freq1 followed by a longer, sustained run of freq2 --
    freq1 should never survive compute_sustained_payload()'s minimum-
    duration gate, freq2 should."""
    n = int(sample_rate * duration_each_s)
    t = np.linspace(0, duration_each_s, n, endpoint=False)
    part1 = np.sin(2 * np.pi * freq1_hz * t).astype(np.float32)
    part2 = np.tile(np.sin(2 * np.pi * freq2_hz * t).astype(np.float32), 4)  # 4x as long
    wavfile.write(path, sample_rate, np.concatenate([part1, part2]))


class ComputePayloadTests(unittest.TestCase):
    def test_end_to_end_matches_wire_format(self):
        with tempfile.TemporaryDirectory() as tmp:
            wav_path = os.path.join(tmp, "tone.wav")
            write_tone_wav(wav_path, freq_hz=1200.0)

            peak_hz, lower_hz, upper_hz, wire = compute_payload(wav_path)

            self.assertAlmostEqual(peak_hz, 1200.0, delta=20.0)
            self.assertLessEqual(lower_hz, peak_hz)
            self.assertGreaterEqual(upper_hz, peak_hz)
            self.assertEqual(len(wire), 4)

            decoded_lower, decoded_upper = decode_freq_range(wire)
            self.assertEqual(decoded_lower, round(lower_hz))
            self.assertEqual(decoded_upper, round(upper_hz))


class ComputeSustainedPayloadTests(unittest.TestCase):
    def test_finds_the_longer_of_two_tones_and_ignores_the_brief_one(self):
        with tempfile.TemporaryDirectory() as tmp:
            wav_path = os.path.join(tmp, "two_tone.wav")
            write_two_tone_wav(wav_path, freq1_hz=1200.0, freq2_hz=4000.0, duration_each_s=1.0)

            result = compute_sustained_payload(wav_path, window_s=1.0, hop_s=1.0, min_duration_s=2.0)

            self.assertIsNotNone(result)
            band, wire = result
            self.assertAlmostEqual(band.mean_peak_hz, 4000.0, delta=30.0)
            self.assertEqual(len(wire), 4)
            decoded_lower, decoded_upper = decode_freq_range(wire)
            self.assertLessEqual(decoded_lower, 4000)
            self.assertGreaterEqual(decoded_upper, 4000)

    def test_returns_none_when_nothing_holds_long_enough(self):
        with tempfile.TemporaryDirectory() as tmp:
            wav_path = os.path.join(tmp, "brief.wav")
            write_tone_wav(wav_path, freq_hz=1500.0, duration_s=1.0)  # shorter than min_duration_s

            result = compute_sustained_payload(wav_path, window_s=1.0, hop_s=1.0, min_duration_s=3.0)

            self.assertIsNone(result)


class CliDryRunTests(unittest.TestCase):
    def test_dry_run_requires_no_ble(self):
        """--dry-run must work even with no bluetooth adapter or bleak
        installed -- it should never import bleak on this path.
        """
        with tempfile.TemporaryDirectory() as tmp:
            wav_path = os.path.join(tmp, "tone.wav")
            write_tone_wav(wav_path, freq_hz=2000.0)

            script = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                   "apply_over_ble.py")
            result = subprocess.run(
                [sys.executable, script, wav_path, "--dry-run"],
                capture_output=True, text=True, timeout=30,
            )

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertIn("FreqRange payload:", result.stdout)
            self.assertIn("--dry-run", result.stdout)

    def test_rolling_dry_run_reports_a_sustained_band(self):
        with tempfile.TemporaryDirectory() as tmp:
            wav_path = os.path.join(tmp, "tone.wav")
            write_tone_wav(wav_path, freq_hz=3000.0, duration_s=3.0)

            script = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                   "apply_over_ble.py")
            result = subprocess.run(
                [sys.executable, script, wav_path, "--rolling", "--dry-run",
                 "--min-duration", "2.0"],
                capture_output=True, text=True, timeout=30,
            )

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertIn("Sustained band:", result.stdout)
            self.assertIn("FreqRange payload:", result.stdout)

    def test_rolling_dry_run_reports_nothing_sustained_without_crashing(self):
        with tempfile.TemporaryDirectory() as tmp:
            wav_path = os.path.join(tmp, "brief.wav")
            write_tone_wav(wav_path, freq_hz=3000.0, duration_s=1.0)

            script = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                   "apply_over_ble.py")
            result = subprocess.run(
                [sys.executable, script, wav_path, "--rolling", "--dry-run",
                 "--min-duration", "5.0"],
                capture_output=True, text=True, timeout=30,
            )

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertIn("not applying anything", result.stdout)
            self.assertNotIn("FreqRange payload:", result.stdout)


if __name__ == "__main__":
    unittest.main()
