import os
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from sound_signature import (  # noqa: E402
    DetectionStep,
    active_intervals,
    band_levels_db,
    detect,
    learn_signature,
    third_octave_edges,
)

SR = 16000
BG = 0.01


def pink(n, rms, rng):
    f = np.fft.rfft(rng.standard_normal(n))
    k = np.arange(len(f))
    k[0] = 1
    x = np.fft.irfft(f / np.sqrt(k), n)
    return x / np.std(x) * rms


def hum(n, f0, harmonics, rng):
    t = np.arange(n) / SR
    x = sum(np.sin(2 * np.pi * f0 * h * t + rng.uniform(0, 6.28)) / h for h in harmonics)
    return x / np.std(x)


def cycle(n, on_s, off_s, start_s):
    t = np.arange(n) / SR
    m = (((t - start_s) % (on_s + off_s)) < on_s) & (t >= start_s)
    k = int(0.5 * SR)
    return np.convolve(m.astype(float), np.ones(k) / k, "same")


def scene(duration_s, source, rng, level_db=12.0, on_s=25, off_s=35, start_s=20):
    n = int(duration_s * SR)
    mask = cycle(n, on_s, off_s, start_s)
    return pink(n, BG, rng) + source(n) * mask * BG * 10 ** (level_db / 20), mask


class BandLayoutTests(unittest.TestCase):
    def test_edges_are_increasing_and_end_at_nyquist(self):
        for per in (3, 6, 12):
            e = third_octave_edges(16000, per_octave=per)
            self.assertTrue(np.all(np.diff(e) > 0))
            self.assertEqual(e[-1], 8000.0)
        self.assertGreater(len(third_octave_edges(16000, per_octave=6)), len(third_octave_edges(16000, per_octave=3)))

    def test_pure_tone_lands_in_its_own_band(self):
        t = np.arange(SR * 3) / SR
        times, edges, levels = band_levels_db(0.1 * np.sin(2 * np.pi * 1000 * t), SR)
        band = int(np.argmax(levels.mean(axis=0)))
        self.assertLessEqual(edges[band], 1000)
        self.assertGreater(edges[band + 1], 1000)

    def test_rejects_stereo_and_too_short_input(self):
        with self.assertRaises(ValueError):
            band_levels_db(np.zeros((100, 2)), SR)
        with self.assertRaises(ValueError):
            band_levels_db(np.zeros(100), SR)


class LearnSignatureTests(unittest.TestCase):
    def test_learns_a_cycling_hum_from_one_clip(self):
        rng = np.random.default_rng(1)
        clip, _ = scene(30, lambda n: hum(n, 100, (1, 2, 3, 4), rng), rng, on_s=12, off_s=60, start_s=10)
        sig = learn_signature(clip, SR)
        self.assertAlmostEqual(float(np.linalg.norm(sig.profile)), 1.0, places=6)
        self.assertGreater(sig.peak_lift_db, 15.0)
        self.assertAlmostEqual(sig.duty, 12 / 30, delta=0.12)
        # The strongest bands sit at the hum's harmonics (100-400 Hz), not up in the hiss.
        top = np.argsort(sig.profile)[::-1][:3]
        centers = np.sqrt(sig.band_edges_hz[top] * sig.band_edges_hz[top + 1])
        self.assertTrue(np.all(centers < 600))

    def test_refuses_a_sound_that_never_changes(self):
        rng = np.random.default_rng(2)
        n = 30 * SR
        steady = pink(n, BG, rng) + hum(n, 100, (1, 2, 3), rng) * BG * 4
        with self.assertRaises(ValueError) as ctx:
            learn_signature(steady, SR)
        self.assertIn("always on", str(ctx.exception))


class DetectTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rng = np.random.default_rng(3)
        clip, _ = scene(30, lambda n: hum(n, 100, (1, 2, 3, 4), rng), rng, on_s=12, off_s=60, start_s=10)
        cls.sig = learn_signature(clip, SR)

    def _score(self, source_builder, seed, duration=300):
        rng = np.random.default_rng(seed)
        x, mask = scene(duration, lambda n: source_builder(n, rng), rng)
        steps = list(detect(x, SR, self.sig))
        t = np.array([s.time_s for s in steps])
        act = np.array([s.active for s in steps])
        truth = mask[np.minimum((t * SR).astype(int), len(mask) - 1)] > 0.5
        near = np.zeros(len(t), bool)
        for et in t[1:][np.abs(np.diff(truth.astype(int))) > 0]:
            near |= np.abs(t - et) <= 6.0
        ok = (t >= 15) & ~near
        return (act & truth & ok).sum() / (truth & ok).sum(), (act & ~truth & ok).sum() / (~truth & ok).sum(), act[t >= 15].mean()

    def test_finds_the_learned_sound_when_it_returns(self):
        found, false, _ = self._score(lambda n, r: hum(n, 100, (1, 2, 3, 4), r), seed=10)
        self.assertGreater(found, 0.95)
        self.assertLess(false, 0.02)

    def test_ignores_a_different_hum(self):
        # 60 Hz hum with different harmonics, cycling on the same schedule.
        _, _, active_share = self._score(lambda n, r: hum(n, 60, (1, 2, 3, 4), r), seed=11)
        self.assertLess(active_share, 0.03)

    def test_ignores_a_hum_with_odd_harmonics_only(self):
        _, _, active_share = self._score(lambda n, r: hum(n, 100, (1, 3, 5, 7), r), seed=12)
        self.assertLess(active_share, 0.03)

    def test_ignores_broadband_hiss(self):
        def hiss(n, r):
            f = np.fft.rfft(r.standard_normal(n))
            freqs = np.fft.rfftfreq(n, 1 / SR)
            f[(freqs < 2000) | (freqs > 7900)] = 0
            x = np.fft.irfft(f, n)
            return x / np.std(x)

        _, _, active_share = self._score(hiss, seed=13)
        self.assertLess(active_share, 0.03)

    def test_silence_and_plain_background_never_fire(self):
        rng = np.random.default_rng(14)
        steps = list(detect(pink(120 * SR, BG, rng), SR, self.sig))
        self.assertFalse(any(s.active for s in steps))

    def test_no_decision_during_warmup(self):
        rng = np.random.default_rng(15)
        x, _ = scene(60, lambda n: hum(n, 100, (1, 2, 3, 4), rng), rng, on_s=50, off_s=5, start_s=0)
        early = [s for s in detect(x, SR, self.sig) if s.time_s < 10.0]
        self.assertTrue(early)
        self.assertTrue(all(s.score == 0.0 and not s.active for s in early))


class ActiveIntervalsTests(unittest.TestCase):
    def test_turns_decisions_into_intervals_and_closes_an_open_one(self):
        flags = [False, True, True, False, False, True, True]
        steps = [DetectionStep(float(i), 0.9 if f else 0.0, 0.0, f) for i, f in enumerate(flags)]
        self.assertEqual(active_intervals(steps), [(1.0, 3.0), (5.0, 6.0)])

    def test_empty_input(self):
        self.assertEqual(active_intervals([]), [])


if __name__ == "__main__":
    unittest.main()
