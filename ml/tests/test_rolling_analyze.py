import os
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from rolling_analyze import find_sustained_bands, rolling_analyze, SustainedBand, WindowResult


def make_tone(freq_hz, sample_rate, duration_s, amplitude=1.0):
    t = np.linspace(0, duration_s, int(sample_rate * duration_s), endpoint=False)
    return amplitude * np.sin(2 * np.pi * freq_hz * t)


class RollingAnalyzeTests(unittest.TestCase):
    def test_rejects_non_positive_window_or_hop(self):
        samples = make_tone(1000.0, 16000, 2.0)
        with self.assertRaises(ValueError):
            list(rolling_analyze(samples, 16000, window_s=0))
        with self.assertRaises(ValueError):
            list(rolling_analyze(samples, 16000, hop_s=-1))

    def test_single_stable_tone_produces_consistent_windows(self):
        sample_rate = 16000
        samples = make_tone(1000.0, sample_rate, duration_s=6.0)

        windows = list(rolling_analyze(samples, sample_rate, window_s=2.0, hop_s=1.0))

        self.assertGreater(len(windows), 3)
        for w in windows:
            self.assertIsInstance(w, WindowResult)
            self.assertAlmostEqual(w.peak_hz, 1000.0, delta=30.0)

    def test_tracks_a_frequency_that_changes_partway_through(self):
        """The actual point of rolling analysis over a single batch call:
        a tone that changes over time should show up as a moving peak,
        not get averaged into something misleading."""
        sample_rate = 16000
        first_half = make_tone(1000.0, sample_rate, duration_s=5.0)
        second_half = make_tone(4000.0, sample_rate, duration_s=5.0)
        samples = np.concatenate([first_half, second_half])

        windows = list(rolling_analyze(samples, sample_rate, window_s=1.0, hop_s=1.0))

        early = [w for w in windows if w.start_s < 4.0]
        late = [w for w in windows if w.start_s >= 6.0]
        self.assertTrue(early, "expected windows before the frequency change")
        self.assertTrue(late, "expected windows after the frequency change")
        for w in early:
            self.assertAlmostEqual(w.peak_hz, 1000.0, delta=30.0)
        for w in late:
            self.assertAlmostEqual(w.peak_hz, 4000.0, delta=30.0)

    def test_skips_windows_with_no_usable_spectral_content_instead_of_raising(self):
        sample_rate = 16000
        samples = make_tone(1000.0, sample_rate, duration_s=3.0)

        # search_min_hz above Nyquist makes every window's band_mask empty --
        # find_troublesome_band raises ValueError for every one of them, and
        # a rolling scan should skip all of them rather than propagate that.
        windows = list(
            rolling_analyze(samples, sample_rate, window_s=1.0, hop_s=1.0, search_min_hz=9000.0)
        )
        self.assertEqual(windows, [])


class FindSustainedBandsTests(unittest.TestCase):
    def test_empty_input_returns_empty(self):
        self.assertEqual(find_sustained_bands([], window_s=1.0), [])

    def test_a_single_isolated_window_is_never_sustained(self):
        windows = [WindowResult(0.0, 1000.0, 900.0, 1100.0)]
        self.assertEqual(find_sustained_bands(windows, window_s=1.0), [])

    def test_a_long_stable_run_is_reported_as_one_sustained_band(self):
        windows = [WindowResult(float(i), 1000.0 + i, 900.0, 1100.0) for i in range(6)]  # 0..5s, jitters slightly

        sustained = find_sustained_bands(windows, window_s=1.0, freq_tolerance_hz=200.0, min_duration_s=2.0)

        self.assertEqual(len(sustained), 1)
        band = sustained[0]
        self.assertIsInstance(band, SustainedBand)
        self.assertEqual(band.start_s, 0.0)
        self.assertEqual(band.end_s, 6.0)  # end of the last window (start 5.0 + window_s), so end - start == duration
        self.assertAlmostEqual(band.duration_s, 6.0)  # (5-0) + window_s
        self.assertAlmostEqual(band.end_s - band.start_s, band.duration_s)
        self.assertEqual(band.window_count, 6)
        # Every window here uses the same [900, 1100] band edges -- the mean
        # across the group should just be that same constant width.
        self.assertAlmostEqual(band.mean_lower_hz, 900.0)
        self.assertAlmostEqual(band.mean_upper_hz, 1100.0)

    def test_mean_band_edges_average_varying_widths_across_the_group(self):
        # Band edges widen from window to window even though the peak (and
        # therefore the grouping decision) stays put -- mean_lower/upper_hz
        # should reflect that, not just echo the peak's own neighborhood.
        windows = [
            WindowResult(0.0, 1000.0, 950.0, 1050.0),
            WindowResult(1.0, 1000.0, 900.0, 1100.0),
            WindowResult(2.0, 1000.0, 850.0, 1150.0),
        ]

        sustained = find_sustained_bands(windows, window_s=1.0, min_duration_s=1.0)

        self.assertEqual(len(sustained), 1)
        band = sustained[0]
        self.assertAlmostEqual(band.mean_lower_hz, (950.0 + 900.0 + 850.0) / 3)
        self.assertAlmostEqual(band.mean_upper_hz, (1050.0 + 1100.0 + 1150.0) / 3)

    def test_a_brief_spike_short_of_min_duration_is_not_reported(self):
        # A stable run, then a very different single-window spike, then back
        # to stable -- the spike should be its own isolated group and, being
        # a single window, never counts as sustained.
        windows = (
            [WindowResult(float(i), 1000.0, 900.0, 1100.0) for i in range(3)]  # 0,1,2s
            + [WindowResult(3.0, 6000.0, 5900.0, 6100.0)]  # a one-window spike
            + [WindowResult(float(i), 1000.0, 900.0, 1100.0) for i in range(4, 7)]  # 4,5,6s
        )

        sustained = find_sustained_bands(windows, window_s=1.0, freq_tolerance_hz=200.0, min_duration_s=2.0)

        # The spike splits the run into two separate stable groups (3s
        # each, with window_s folded in) -- what matters is the spike
        # itself never appears in whatever comes back.
        for band in sustained:
            self.assertNotAlmostEqual(band.mean_peak_hz, 6000.0, delta=500.0)

    def test_two_genuinely_different_sustained_tones_are_reported_separately(self):
        low = [WindowResult(float(i), 1000.0, 900.0, 1100.0) for i in range(4)]  # 0..3s
        high = [WindowResult(float(i), 5000.0, 4900.0, 5100.0) for i in range(4, 9)]  # 4..8s
        windows = low + high

        sustained = find_sustained_bands(windows, window_s=1.0, freq_tolerance_hz=200.0, min_duration_s=2.0)

        self.assertEqual(len(sustained), 2)
        self.assertAlmostEqual(sustained[0].mean_peak_hz, 1000.0)
        self.assertAlmostEqual(sustained[1].mean_peak_hz, 5000.0)

    def test_tolerance_controls_whether_drifting_windows_stay_grouped(self):
        # Peak drifts 1000 -> 1030 -> 1060 -> 1090 -> 1120 over 5 windows
        # (a slow 30 Hz/window drift). A loose tolerance treats the whole
        # thing as one steady tone...
        drifting = [WindowResult(float(i), 1000.0 + 30 * i, 900.0, 1100.0) for i in range(5)]

        loose = find_sustained_bands(drifting, window_s=1.0, freq_tolerance_hz=200.0, min_duration_s=1.0)
        self.assertEqual(len(loose), 1)
        self.assertEqual(loose[0].window_count, 5)

        # ...while a tighter tolerance can't stretch across the whole drift
        # in one group -- the running mean falls too far behind the leading
        # edge partway through, splitting it into two shorter groups instead
        # of one long one. Still real, sustained bands -- just smaller ones,
        # which is the actual point: tolerance changes the grouping, not
        # just an on/off switch for whether anything survives at all.
        tighter = find_sustained_bands(drifting, window_s=1.0, freq_tolerance_hz=50.0, min_duration_s=1.0)
        self.assertGreater(len(tighter), 1)
        self.assertEqual(sum(g.window_count for g in tighter), 5)


class GateTests(unittest.TestCase):
    """Regressions for the review of PR #16: find_troublesome_band() always
    returns an argmax, so without gates silence and noise became 'bands'."""

    def test_digital_silence_yields_no_windows(self):
        sample_rate = 16000
        samples = np.zeros(sample_rate * 10)
        self.assertEqual(list(rolling_analyze(samples, sample_rate, window_s=2.0, hop_s=1.0)), [])

    def test_white_noise_yields_no_windows_but_is_visible_ungated(self):
        sample_rate = 16000
        rng = np.random.default_rng(1)
        samples = rng.standard_normal(sample_rate * 10) * 0.3
        gated = list(rolling_analyze(samples, sample_rate, window_s=2.0, hop_s=1.0))
        self.assertEqual(gated, [])
        ungated = list(rolling_analyze(samples, sample_rate, window_s=2.0, hop_s=1.0,
                                       min_prominence_db=0.0, min_level_dbfs=float("-inf")))
        self.assertGreater(len(ungated), 0)  # the PR #16 behaviour, on request only

    def test_a_tone_in_noise_still_passes_the_gates(self):
        sample_rate = 16000
        rng = np.random.default_rng(2)
        samples = make_tone(3000.0, sample_rate, 6.0, amplitude=0.3) + rng.standard_normal(sample_rate * 6) * 0.02
        windows = list(rolling_analyze(samples, sample_rate, window_s=2.0, hop_s=1.0))
        self.assertEqual(len(windows), 5)
        for w in windows:
            self.assertAlmostEqual(w.peak_hz, 3000.0, delta=20.0)

    def test_quiet_tone_below_level_gate_is_skipped(self):
        sample_rate = 16000
        samples = make_tone(3000.0, sample_rate, 6.0, amplitude=1e-4)  # about -83 dBFS
        self.assertEqual(list(rolling_analyze(samples, sample_rate, window_s=2.0, hop_s=1.0)), [])
        self.assertEqual(
            len(list(rolling_analyze(samples, sample_rate, window_s=2.0, hop_s=1.0, min_level_dbfs=-100.0))), 5)

    def test_single_window_is_not_sustained_even_when_min_duration_equals_window(self):
        windows = [WindowResult(0.0, 1000.0, 900.0, 1100.0)]
        self.assertEqual(find_sustained_bands(windows, window_s=2.0, min_duration_s=2.0), [])

    def test_a_time_gap_splits_an_otherwise_matching_group(self):
        # Same frequency at 0..2s and again at 10..12s, with the windows in
        # between gated out: two separate bands, not one 13-second one.
        windows = [WindowResult(0.0, 1000.0, 900.0, 1100.0), WindowResult(1.0, 1000.0, 900.0, 1100.0),
                   WindowResult(10.0, 1000.0, 900.0, 1100.0), WindowResult(11.0, 1000.0, 900.0, 1100.0)]
        sustained = find_sustained_bands(windows, window_s=2.0, min_duration_s=2.0)
        self.assertEqual([(b.start_s, b.end_s) for b in sustained], [(0.0, 3.0), (10.0, 13.0)])
        for b in sustained:
            self.assertAlmostEqual(b.end_s - b.start_s, b.duration_s)


if __name__ == "__main__":
    unittest.main()
