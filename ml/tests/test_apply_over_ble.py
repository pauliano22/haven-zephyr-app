import os
import subprocess
import sys
import tempfile
import unittest

import numpy as np
from scipy.io import wavfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from apply_over_ble import compute_payload
from ble_translator import decode_freq_range


def write_tone_wav(path, freq_hz, sample_rate=16000, duration_s=1.0):
    t = np.linspace(0, duration_s, int(sample_rate * duration_s), endpoint=False)
    samples = np.sin(2 * np.pi * freq_hz * t).astype(np.float32)
    wavfile.write(path, sample_rate, samples)


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


if __name__ == "__main__":
    unittest.main()
