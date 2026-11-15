"""
"Teach the device a sound": learn a signature from one short clip of a
bothersome sound, then detect that sound later in a live stream.

Offline prototype for the design in ADAPTIVE_PROTECTION_DESIGN.md (haven_workspace
root). Still deterministic DSP with no trained model, like analyze.py and
rolling_analyze.py. It answers one question: can a device recognise a sound it
was shown once (a cycling basement pump, say) and ignore other sounds?

How it works
  * Split the audio into 1/3-octave bands and measure each band's level over time.
  * Learn: the sound is whatever LIFTS each band above that band's usual level
    during the clip. The signature is that lift pattern as a unit vector,
    plus (information only) how much of the time the sound is on and how often
    it repeats.
  * Detect: in each short window, measure how far each band sits above its own
    recent background (the 20th percentile of the last minute), then compare
    that lift pattern with the signature (cosine similarity). A window only
    counts if the signature's bands really rise (min_lift_db), and the decision
    uses hysteresis so one stray window does not toggle it.

Known limits (by design, see the tests and experiments/sound_signature_experiment.py)
  * A sound that is on nearly all the time has no "background" to rise above,
    so it cannot be learned this way. Those need a shape-based mode, not built.
  * It tells apart sounds with different spectral shapes. Two machines with
    similar harmonic patterns can be confused; the experiment measures how often.
  * It can never act before it has heard the first part of a sound.
"""
from typing import Iterator, List, NamedTuple, Optional, Sequence, Tuple

import numpy as np
from scipy import signal

LOW_HZ = 80.0
THIRD_OCTAVE_STEP = 2.0 ** (1.0 / 3.0)
FLOOR_DB = -120.0

NPERSEG = 4096
HOP = 2048


class Signature(NamedTuple):
    profile: np.ndarray  # unit vector, one entry per band: how much each band rises
    band_edges_hz: np.ndarray  # len(profile) + 1 edges
    duty: Optional[float]  # fraction of the clip the sound was on (information only)
    period_s: Optional[float]  # repeat interval if the clip shows a clear repeat
    peak_lift_db: float  # largest band lift seen while learning


class DetectionStep(NamedTuple):
    time_s: float  # window end time
    score: float  # similarity 0..1 (0 if the lift gate or warm-up failed)
    lift_db: float  # largest lift among the signature's strongest bands
    active: bool  # decision after hysteresis


def third_octave_edges(sample_rate: float, low_hz: float = LOW_HZ, per_octave: int = 3) -> np.ndarray:
    """Band edges, `per_octave` bands per doubling of frequency (3 = third octave)."""
    step = 2.0 ** (1.0 / per_octave)
    nyquist = sample_rate / 2.0
    edges = [low_hz]
    while edges[-1] * step < nyquist:
        edges.append(edges[-1] * step)
    edges.append(nyquist)
    return np.array(edges)


def band_levels_db(samples: np.ndarray, sample_rate: float, edges: Optional[np.ndarray] = None
                   ) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Return (frame_times_s, band_edges_hz, levels_db[frames, bands]) in dB re full scale."""
    x = np.asarray(samples, dtype=np.float64)
    if x.ndim != 1:
        raise ValueError("samples must be mono (1-D)")
    if len(x) < NPERSEG:
        raise ValueError("need at least %d samples" % NPERSEG)
    if edges is None:
        edges = third_octave_edges(sample_rate)
    freqs, times, sxx = signal.spectrogram(
        x, fs=sample_rate, nperseg=NPERSEG, noverlap=NPERSEG - HOP, scaling="spectrum", mode="psd")
    levels = np.full((sxx.shape[1], len(edges) - 1), FLOOR_DB)
    for b in range(len(edges) - 1):
        sel = (freqs >= edges[b]) & (freqs < edges[b + 1])
        if np.any(sel):
            power = sxx[sel, :].sum(axis=0)
            levels[:, b] = 10.0 * np.log10(np.maximum(power, 1e-12))
    return times, edges, levels


def _background(levels: np.ndarray, frame_s: float, background_s: float, i: int) -> np.ndarray:
    """Per-band 20th percentile of the last `background_s` seconds up to frame i."""
    n = max(1, int(round(background_s / frame_s)))
    lo = max(0, i - n + 1)
    return np.percentile(levels[lo:i + 1, :], 20, axis=0)


def _repeat_period(on_curve: np.ndarray, frame_s: float, min_s: float, max_s: float) -> Optional[float]:
    x = on_curve - on_curve.mean()
    if np.allclose(x, 0):
        return None
    ac = np.correlate(x, x, mode="full")[len(x) - 1:]
    ac = ac / ac[0]
    lo, hi = int(min_s / frame_s), min(int(max_s / frame_s), len(ac) - 1)
    if hi <= lo:
        return None
    lag = lo + int(np.argmax(ac[lo:hi + 1]))
    return float(lag * frame_s) if ac[lag] >= 0.4 else None


def learn_signature(samples: np.ndarray, sample_rate: float, min_peak_lift_db: float = 6.0,
                    per_octave: int = 6) -> Signature:
    """Learn a signature from a clip that contains the sound starting and stopping."""
    times, edges, levels = band_levels_db(samples, sample_rate, third_octave_edges(sample_rate, per_octave=per_octave))
    frame_s = float(times[1] - times[0]) if len(times) > 1 else HOP / sample_rate
    floor = np.percentile(levels, 20, axis=0)
    high = np.percentile(levels, 90, axis=0)
    lift = np.maximum(high - floor, 0.0)
    peak = float(lift.max())
    if peak < min_peak_lift_db:
        raise ValueError(
            "no band rises by at least %.1f dB in this clip; the sound needs to start and stop "
            "inside the clip (a sound that is always on cannot be learned this way)" % min_peak_lift_db)
    profile = lift / np.linalg.norm(lift)

    strong = lift >= 0.5 * peak
    on_curve = (levels[:, strong] - floor[strong]).mean(axis=1)
    on_mask = on_curve > 0.5 * on_curve.max()
    duty = float(on_mask.mean())
    period = _repeat_period(on_curve, frame_s, min_s=4.0, max_s=min(120.0, 0.5 * len(on_curve) * frame_s))
    return Signature(profile, edges, duty, period, peak)


def detect(samples: np.ndarray, sample_rate: float, sig: Signature, window_s: float = 2.0,
           hop_s: float = 1.0, background_s: float = 60.0, threshold: float = 0.85,
           min_lift_db: float = 6.0, warmup_s: float = 10.0, on_after: int = 2,
           off_after: int = 3, focus_strong: bool = False,
           focus_fraction: float = 0.25) -> Iterator[DetectionStep]:
    """Slide a window over the stream and report when the learned sound is present."""
    times, _, levels = band_levels_db(samples, sample_rate, sig.band_edges_hz)
    frame_s = float(times[1] - times[0])
    win_frames = max(1, int(round(window_s / frame_s)))
    hop_frames = max(1, int(round(hop_s / frame_s)))
    strong = np.argsort(sig.profile)[::-1][:max(1, int(np.sum(sig.profile >= 0.5 * sig.profile.max())))]

    active, run_hi, run_lo = False, 0, 0
    for end in range(win_frames - 1, len(times), hop_frames):
        t = float(times[end])
        score, lift_db = 0.0, 0.0
        if t >= warmup_s:
            window = levels[end - win_frames + 1:end + 1, :]
            lift = np.maximum(window.mean(axis=0) - _background(levels, frame_s, background_s, end), 0.0)
            lift_db = float(lift[strong].max())
            if focus_strong:
                # Compare only the bands the learned sound uses, so other sounds playing in
                # other bands at the same time do not drag the match down.
                keep = sig.profile >= focus_fraction * sig.profile.max()
                lift = np.where(keep, lift, 0.0)
                ref = np.where(keep, sig.profile, 0.0)
                ref = ref / np.linalg.norm(ref)
            else:
                ref = sig.profile
            norm = np.linalg.norm(lift)
            if lift_db >= min_lift_db and norm > 0:
                score = float(np.dot(lift / norm, ref))
        hit = score >= threshold
        run_hi, run_lo = (run_hi + 1, 0) if hit else (0, run_lo + 1)
        if not active and run_hi >= on_after:
            active = True
        elif active and run_lo >= off_after:
            active = False
        yield DetectionStep(t, score, lift_db, active)


def active_intervals(steps: Sequence[DetectionStep]) -> List[Tuple[float, float]]:
    out, start = [], None
    for s in steps:
        if s.active and start is None:
            start = s.time_s
        elif not s.active and start is not None:
            out.append((start, s.time_s))
            start = None
    if start is not None and steps:
        out.append((start, steps[-1].time_s))
    return out
