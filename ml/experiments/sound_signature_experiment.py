"""
Does a one-example sound signature recognise a cycling machine and ignore other sounds?

    python3 experiments/sound_signature_experiment.py

Synthetic scenes only: a "pump" (100 Hz hum with harmonics, cycling 25 s on / 35 s off) is learned
from one 30 s clip, then searched for in 7 minute scenes with several other sounds mixed in. Pump
levels are given relative to the room's background noise. Windows within 6 s of a pump start or stop
are ignored, because the detector needs about 2-4 s to notice a change (that lag is a property of the
method, reported separately).

What this does NOT show: real recordings, real rooms, or real people. Synthetic hums are cleaner than
a real geothermal system. Treat the numbers as a check that the idea is not obviously broken, and as a
way to compare variants, not as performance claims.
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from sound_signature import active_intervals, detect, learn_signature  # noqa: E402

SR = 16000
BG_RMS = 0.01
TRANSITION_TOL_S = 6.0


def make_rng(seed):
    return np.random.default_rng(seed)


def pink(n, rms, rng):
    f = np.fft.rfft(rng.standard_normal(n))
    k = np.arange(len(f)); k[0] = 1
    x = np.fft.irfft(f / np.sqrt(k), n)
    return x / np.std(x) * rms


def hum(n, f0, harmonics, weights, rng):
    t = np.arange(n) / SR
    x = sum(w * np.sin(2 * np.pi * f0 * h * t + rng.uniform(0, 2 * np.pi)) for h, w in zip(harmonics, weights))
    return x / np.std(x)


def cycle_mask(n, on_s, off_s, start_s):
    t = np.arange(n) / SR
    phase = (t - start_s) % (on_s + off_s)
    m = ((phase < on_s) & (t >= start_s)).astype(float)
    k = int(0.5 * SR)
    return np.convolve(m, np.ones(k) / k, "same")


def bandnoise(n, lo, hi, rng):
    f = np.fft.rfft(rng.standard_normal(n))
    freqs = np.fft.rfftfreq(n, 1 / SR)
    f[(freqs < lo) | (freqs > hi)] = 0
    x = np.fft.irfft(f, n)
    return x / np.std(x)


def bursts(n, lo, hi, rng, every=(5, 20), length=(1, 3)):
    carrier = bandnoise(n, lo, hi, rng)
    m = np.zeros(n)
    t = rng.uniform(*every)
    while t < n / SR:
        d = rng.uniform(*length)
        m[int(t * SR):int((t + d) * SR)] = 1
        t += d + rng.uniform(*every)
    return carrier * np.convolve(m, np.ones(int(0.1 * SR)) / (0.1 * SR), "same")


PUMP = dict(f0=100, harmonics=(1, 2, 3, 4, 5, 6), weights=(1, .7, .5, .4, .3, .2))
SOUNDS = {
    # name: (builder(n, rng) -> unit-RMS signal, relative level in dB vs background RMS)
    "pump": lambda n, r: hum(n, rng=r, **PUMP) * cycle_mask(n, 25, 35, 20),
    "fan (60 Hz hum, cycles)": lambda n, r: hum(n, 60, (1, 2, 3, 4), (1, .8, .5, .3), r) * cycle_mask(n, 40, 80, 50),
    "blower (100 Hz odd harmonics, cycles)": lambda n, r: hum(n, 100, (1, 3, 5, 7), (1, .7, .5, .3), r) * cycle_mask(n, 30, 50, 35),
    "kettle hiss (2-8 kHz, cycles)": lambda n, r: bandnoise(n, 2000, 7900, r) * cycle_mask(n, 20, 100, 70),
    "speech-like bursts": lambda n, r: bursts(n, 200, 3000, r),
    "beeper 2 kHz": lambda n, r: np.sin(2 * np.pi * 2000 * np.arange(n) / SR) * cycle_mask(n, 1, 29, 10),
}


def build_scene(duration_s, include, snr_db, seed):
    rng = make_rng(seed)
    n = int(duration_s * SR)
    x = pink(n, BG_RMS, rng)
    for name in include:
        sig = SOUNDS[name](n, rng)
        x = x + sig * BG_RMS * 10 ** (snr_db / 20)
    pump_mask = cycle_mask(n, 25, 35, 20) if "pump" in include else np.zeros(n)
    return x, pump_mask


def score(steps, pump_mask, ignore_before_s=15.0):
    t = np.array([s.time_s for s in steps])
    act = np.array([s.active for s in steps])
    truth = pump_mask[np.minimum((t * SR).astype(int), len(pump_mask) - 1)] > 0.5
    # Ignore windows near a pump start/stop.
    edge = np.abs(np.diff(truth.astype(int)))
    edge_t = t[1:][edge > 0]
    near = np.zeros(len(t), bool)
    for et in edge_t:
        near |= np.abs(t - et) <= TRANSITION_TOL_S
    ok = (t >= ignore_before_s) & ~near
    pos, neg = truth & ok, ~truth & ok
    tpr = (act & pos).sum() / pos.sum() if pos.sum() else float("nan")
    fpr = (act & neg).sum() / neg.sum() if neg.sum() else float("nan")
    return tpr, fpr, act, t


def lag_stats(steps, pump_mask):
    """Seconds between the true start/stop and the detector's on/off decision."""
    iv = active_intervals(steps)
    starts = np.flatnonzero(np.diff((pump_mask > 0.5).astype(int)) == 1) / SR
    stops = np.flatnonzero(np.diff((pump_mask > 0.5).astype(int)) == -1) / SR
    on_lag, off_lag = [], []
    for s in starts:
        m = [a for a, b in iv if s - 2 <= a <= s + 15]
        if m:
            on_lag.append(m[0] - s)
    for e in stops:
        m = [b for a, b in iv if e - 2 <= b <= e + 15]
        if m:
            off_lag.append(m[0] - e)
    f = lambda v: (round(float(np.median(v)), 1) if v else None)
    return f(on_lag), f(off_lag), len(starts)


def main():
    # One 30 s example: the pump runs for 12 s of it, in an otherwise quiet room. Learned at 12 dB.
    rng = make_rng(7)
    n = 30 * SR
    teach = pink(n, BG_RMS, rng) + hum(n, rng=rng, **PUMP) * cycle_mask(n, 12, 60, 10) * BG_RMS * 10 ** (12 / 20)
    sig = learn_signature(teach, SR)
    print("Learned from one 30 s clip: peak lift %.1f dB, on-time %.0f%% of the clip\n" % (sig.peak_lift_db, 100 * sig.duty))

    print("A. Pump alone in a quiet room (detect it)")
    print("   %-14s %-10s %-10s %-22s" % ("pump level", "found", "false", "start/stop lag (s)"))
    for snr in (0, 6, 12, 20):
        x, m = build_scene(420, ["pump"], snr, seed=100 + snr)
        steps = list(detect(x, SR, sig))
        tpr, fpr, _, _ = score(steps, m)
        on_lag, off_lag, n_cyc = lag_stats(steps, m)
        print("   %+3d dB vs room  %-10s %-10s %s / %s" % (snr, "%.0f%%" % (100 * tpr), "%.1f%%" % (100 * fpr), on_lag, off_lag))

    print("\nB. Another sound, no pump at all (how often does it fire wrongly?)")
    print("   %-40s %-14s" % ("the other sound (at +12 dB)", "wrong 'active' time"))
    for name in SOUNDS:
        if name == "pump":
            continue
        x, m = build_scene(420, [name], 12, seed=200 + len(name))
        steps = list(detect(x, SR, sig))
        _, fpr, act, t = score(steps, m)
        print("   %-40s %.1f%%" % (name, 100 * np.mean(act[t >= 15])))

    print("\nC. Pump plus every other sound at once")
    print("   %-14s %-10s %-10s" % ("pump level", "found", "false"))
    for snr in (6, 12, 20):
        x, m = build_scene(420, list(SOUNDS), snr, seed=300 + snr)
        steps = list(detect(x, SR, sig))
        tpr, fpr, _, _ = score(steps, m)
        print("   %+3d dB vs room  %-10s %-10s" % (snr, "%.0f%%" % (100 * tpr), "%.1f%%" % (100 * fpr)))


if __name__ == "__main__":
    main()
