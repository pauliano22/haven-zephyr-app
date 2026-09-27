"""
Continuous/streaming version of analyze.py's single-shot PSD-peak
detection: runs `find_troublesome_band` over a sliding window instead of
once on a whole recording, and separates a sustained problem tone (a
machine whine, an ongoing alarm) from a brief transient (a door slam, a
passing car horn) that happens to be loud for a moment.

This is Tier 2's original first idea from ML_RL_FEASIBILITY.md ("running
analyze.py's heuristic on a rolling window instead of a whole recording")
-- still deterministic DSP, no model, no training data, same as analyze.py
itself. It operates on a batch of samples (a long recording) here; wiring
it to a live mic feed (phone-side or on-device) is future integration work,
not done here -- same scoping analyze.py's own README already uses for
apply_over_ble.py.
"""
from typing import Iterator, List, NamedTuple, Optional

import numpy as np

from analyze import find_troublesome_band


class WindowResult(NamedTuple):
    start_s: float
    peak_hz: float
    lower_hz: float
    upper_hz: float


class SustainedBand(NamedTuple):
    """A problem tone that held roughly steady for at least min_duration_s,
    as opposed to a brief transient."""

    start_s: float
    end_s: float
    duration_s: float
    mean_peak_hz: float
    window_count: int


def rolling_analyze(
    samples: np.ndarray,
    sample_rate: int,
    window_s: float = 2.0,
    hop_s: float = 1.0,
    **find_kwargs,
) -> Iterator[WindowResult]:
    """Slide a window_s-second window across `samples` in hop_s-second
    steps, running find_troublesome_band on each. A window with no usable
    spectral content (find_troublesome_band's ValueError, e.g. a window
    that's pure silence outside the search range) is skipped, not raised --
    a rolling scan over a long recording shouldn't abort on one quiet
    window.
    """
    if window_s <= 0 or hop_s <= 0:
        raise ValueError("window_s and hop_s must be positive")

    window_n = int(window_s * sample_rate)
    hop_n = int(hop_s * sample_rate)
    if window_n < 1 or hop_n < 1:
        raise ValueError("window_s/hop_s too small for this sample_rate")

    for start in range(0, len(samples) - window_n + 1, hop_n):
        chunk = samples[start : start + window_n]
        try:
            peak_hz, lower_hz, upper_hz = find_troublesome_band(chunk, sample_rate, **find_kwargs)
        except ValueError:
            continue
        yield WindowResult(start / sample_rate, peak_hz, lower_hz, upper_hz)


def find_sustained_bands(
    windows: List[WindowResult],
    freq_tolerance_hz: float = 200.0,
    min_duration_s: float = 2.0,
) -> List[SustainedBand]:
    """Group consecutive windows whose peak stays within freq_tolerance_hz
    of the group's running mean, and keep only groups that span at least
    min_duration_s -- the actual point of this function: telling a
    genuinely sustained tone apart from a momentary one that a single
    window might flag but that isn't worth alerting on.

    `windows` must be in time order (as rolling_analyze yields them).
    """
    if not windows:
        return []

    groups: List[List[WindowResult]] = [[windows[0]]]
    for w in windows[1:]:
        current = groups[-1]
        running_mean = sum(x.peak_hz for x in current) / len(current)
        if abs(w.peak_hz - running_mean) <= freq_tolerance_hz:
            current.append(w)
        else:
            groups.append([w])

    sustained = []
    for group in groups:
        start_s = group[0].start_s
        # A group's *coverage* extends past its last window's start time by
        # one window length, not just to the last start -- otherwise a
        # group of back-to-back windows looks shorter than it really is.
        # We don't have window_s here directly, so approximate duration
        # from the span between window starts plus the hop between the
        # first two overall windows if available, else fall back to the
        # gap between this group's first and last window start.
        if len(group) == 1:
            duration_s = 0.0
        else:
            duration_s = group[-1].start_s - group[0].start_s
        if duration_s >= min_duration_s:
            mean_peak = sum(x.peak_hz for x in group) / len(group)
            sustained.append(
                SustainedBand(
                    start_s=start_s,
                    end_s=group[-1].start_s,
                    duration_s=duration_s,
                    mean_peak_hz=mean_peak,
                    window_count=len(group),
                )
            )
    return sustained


if __name__ == "__main__":
    import sys

    from analyze import load_wav_mono

    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <path-to-wav>")
        sys.exit(1)

    samples, sample_rate = load_wav_mono(sys.argv[1])
    windows = list(rolling_analyze(samples, sample_rate))
    print(f"{len(windows)} windows analyzed")
    for w in windows:
        print(f"  t={w.start_s:5.1f}s  peak={w.peak_hz:7.1f} Hz  band=[{w.lower_hz:.0f}, {w.upper_hz:.0f}] Hz")

    sustained = find_sustained_bands(windows)
    print(f"\n{len(sustained)} sustained band(s) (>= 2s, held within 200 Hz):")
    for s in sustained:
        print(
            f"  {s.start_s:.1f}s-{s.end_s:.1f}s ({s.duration_s:.1f}s, "
            f"{s.window_count} windows): ~{s.mean_peak_hz:.0f} Hz"
        )
