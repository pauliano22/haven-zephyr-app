import os
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from analyze import find_troublesome_band


def make_tone(freq_hz, sample_rate=16000, duration_s=1.0, amplitude=1.0):
    t = np.linspace(0, duration_s, int(sample_rate * duration_s), endpoint=False)
    return amplitude * np.sin(2 * np.pi * freq_hz * t)


class FindTroublesomeBandTests(unittest.TestCase):
    def test_detects_known_pure_tone(self):
        tone_hz = 1000.0
        samples = make_tone(tone_hz, sample_rate=16000, duration_s=2.0)

        peak_hz, lower_hz, upper_hz = find_troublesome_band(samples, sample_rate=16000)

        # Welch's method has finite frequency resolution; allow some slack.
        self.assertAlmostEqual(peak_hz, tone_hz, delta=20.0)
        self.assertLess(lower_hz, peak_hz)
        self.assertGreater(upper_hz, peak_hz)

    def test_detects_dominant_of_two_tones(self):
        sample_rate = 16000
        loud = make_tone(2000.0, sample_rate=sample_rate, duration_s=2.0, amplitude=1.0)
        quiet = make_tone(500.0, sample_rate=sample_rate, duration_s=2.0, amplitude=0.05)
        samples = loud + quiet

        peak_hz, _, _ = find_troublesome_band(samples, sample_rate=sample_rate)

        self.assertAlmostEqual(peak_hz, 2000.0, delta=30.0)

    def test_search_range_restricts_peak_selection(self):
        sample_rate = 16000
        low_tone = make_tone(300.0, sample_rate=sample_rate, duration_s=2.0, amplitude=1.0)
        high_tone = make_tone(5000.0, sample_rate=sample_rate, duration_s=2.0, amplitude=0.3)
        samples = low_tone + high_tone

        # Without restricting the search range the loud low tone should win.
        peak_hz, _, _ = find_troublesome_band(samples, sample_rate=sample_rate)
        self.assertAlmostEqual(peak_hz, 300.0, delta=30.0)

        # Restricting the search range should pick the quieter high tone instead.
        peak_hz_restricted, _, _ = find_troublesome_band(
            samples, sample_rate=sample_rate, search_min_hz=1000.0
        )
        self.assertAlmostEqual(peak_hz_restricted, 5000.0, delta=50.0)

    def test_empty_search_range_raises(self):
        samples = make_tone(1000.0, sample_rate=16000, duration_s=1.0)
        with self.assertRaises(ValueError):
            find_troublesome_band(
                samples, sample_rate=16000, search_min_hz=7000.0, search_max_hz=6000.0
            )


if __name__ == "__main__":
    unittest.main()
