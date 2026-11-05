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
from scipy import signal

from analyze import find_troublesome_band

# Gates added after review of PR #16 (2026-09-29): find_troublesome_band()
# always returns *some* argmax, so without these a window of digital
# silence yielded a "band" at the lowest search bin and white noise yielded
# a different random "band" every window -- and --rolling would have written
# either to the FreqRange characteristic. A problem tone has to stand out
# from the rest of the spectrum (prominence) and the window has to contain
# real signal (level) before it counts.
DEFAULT_MIN_PROMINENCE_DB = 10.0
DEFAULT_MIN_LEVEL_DBFS = -60.0


def window_level_dbfs(chunk: np.ndarray) -> float:
    """RMS level of the window in dBFS (0 dBFS = full-scale sine RMS 1/sqrt2
    is -3 dBFS; a full-scale square wave is 0 dBFS). -inf for digital
    silence."""
    rms = float(np.sqrt(np.mean(np.square(chunk.astype(np.float64))))) if len(chunk) else 0.0
    return 20.0 * np.log10(rms) if rms > 0 else float("-inf")


def peak_prominence_db(chunk: np.ndarray, sample_rate: int, peak_hz: float,
                       search_min_hz: float = 20.0, search_max_hz: Optional[float] = None) -> float:
    """How far the PSD at peak_hz stands above the *median* PSD of the
    searched band, in dB. White noise gives ~0-6 dB (argmax of a flat
    spectrum is just the luckiest bin); a real tone gives tens of dB.
    Uses the same Welch settings as find_troublesome_band so the peak bin
    is the same bin."""
    if search_max_hz is None:
        search_max_hz = sample_rate / 2.0
    freqs, psd = signal.welch(chunk, fs=sample_rate, nperseg=min(4096, len(chunk)))
    mask = (freqs >= search_min_hz) & (freqs <= search_max_hz)
    freqs = freqs[mask]
    psd = psd[mask]
    if len(psd) == 0:
        return 0.0
    idx = int(np.argmin(np.abs(freqs - peak_hz)))
    floor = float(np.median(psd))
    if floor <= 0.0:
        return float("inf") if psd[idx] > 0 else 0.0
    return 10.0 * float(np.log10(psd[idx] / floor))


class WindowResult(NamedTuple):
    start_s: float
    peak_hz: float
    lower_hz: float
    upper_hz: float


class SustainedBand(NamedTuple):
    """A problem tone that held roughly steady for at least min_duration_s,
    as opposed to a brief transient.

    mean_lower_hz/mean_upper_hz average each constituent window's own
    -3dB band edges -- this is what a caller actually needs to apply the
    band over BLE (ble_translator.encode_freq_range() takes a [lower,
    upper] range, not a single peak); mean_peak_hz alone isn't enough for
    that. Kept separately rather than derived from mean_peak_hz because the
    band's width can genuinely vary window to window even while the peak
    itself stays put.
    """

    start_s: float
    end_s: float
    duration_s: float
    mean_peak_hz: float
    mean_lower_hz: float
    mean_upper_hz: float
    window_count: int


def rolling_analyze(
    samples: np.ndarray,
    sample_rate: int,
    window_s: float = 2.0,
    hop_s: float = 1.0,
    min_prominence_db: float = DEFAULT_MIN_PROMINENCE_DB,
    min_level_dbfs: float = DEFAULT_MIN_LEVEL_DBFS,
    **find_kwargs,
) -> Iterator[WindowResult]:
    """Slide a window_s-second window across `samples` in hop_s-second
    steps, running find_troublesome_band on each. A window with no usable
    spectral content (find_troublesome_band's ValueError, e.g. a window
    that's pure silence outside the search range) is skipped, not raised --
    a rolling scan over a long recording shouldn't abort on one quiet
    window.

    Two gates decide whether a window's argmax is a *tone* at all:
    - level: windows quieter than min_level_dbfs RMS are skipped (digital
      silence, mic unplugged, pauses);
    - prominence: the peak must stand min_prominence_db above the median
      PSD of the searched band (white/pink noise, HVAC rumble spread over
      the spectrum, speech babble do not qualify).
    Pass min_prominence_db=0 and min_level_dbfs=-inf to get the ungated
    PR #16 behaviour back.
    """
    if window_s <= 0 or hop_s <= 0:
        raise ValueError("window_s and hop_s must be positive")

    window_n = int(window_s * sample_rate)
    hop_n = int(hop_s * sample_rate)
    if window_n < 1 or hop_n < 1:
        raise ValueError("window_s/hop_s too small for this sample_rate")

    for start in range(0, len(samples) - window_n + 1, hop_n):
        chunk = samples[start : start + window_n]
        if window_level_dbfs(chunk) < min_level_dbfs:
            continue
        try:
            peak_hz, lower_hz, upper_hz = find_troublesome_band(chunk, sample_rate, **find_kwargs)
        except ValueError:
            continue
        if min_prominence_db > 0:
            prom = peak_prominence_db(
                chunk, sample_rate, peak_hz,
                search_min_hz=find_kwargs.get("search_min_hz", 20.0),
                search_max_hz=find_kwargs.get("search_max_hz"),
            )
            if prom < min_prominence_db:
                continue
        yield WindowResult(start / sample_rate, peak_hz, lower_hz, upper_hz)


def find_sustained_bands(
    windows: List[WindowResult],
    window_s: float,
    freq_tolerance_hz: float = 200.0,
    min_duration_s: float = 2.0,
    min_windows: int = 2,
    max_gap_s: Optional[float] = None,
) -> List[SustainedBand]:
    """Group consecutive windows whose peak stays within freq_tolerance_hz
    of the group's running mean, and keep only groups that span at least
    min_duration_s -- the actual point of this function: telling a
    genuinely sustained tone apart from a momentary one that a single
    window might flag but that isn't worth alerting on.

    `windows` must be in time order (as rolling_analyze yields them).
    `window_s` must be the same value passed to the rolling_analyze() call
    that produced them -- a group's real audio coverage extends past its
    last window's *start* time by a full window length (each WindowResult
    marks where its window began, not where it ended), so duration is
    (last start - first start) + window_s, not just the gap between starts.
    Passing the wrong window_s under- or over-reports duration without
    raising -- there's no way to detect that from WindowResult alone, so
    this is a real caller contract, not just documentation.

    A previous version approximated this without window_s at all (using
    the gap between starts alone, giving 0.0 for a single-window group and
    undercounting every multi-window group by one full window length).
    Caught by actually running rolling_analyze.py end-to-end on a real
    recording, not by the unit tests alone: a genuinely 2-second sustained
    tone (two consecutive 1-second windows) was silently dropped because
    its reported duration (1.0s, the gap between starts) fell short of the
    2.0s default threshold, when its real coverage was the full 2.0s.
    """
    if not windows:
        return []
    if max_gap_s is None:
        max_gap_s = window_s  # consecutive hops overlap; a gap longer than one
        # window means at least one window in between was skipped (gated out),
        # so the tone was not actually continuous across it.

    groups: List[List[WindowResult]] = [[windows[0]]]
    for w in windows[1:]:
        current = groups[-1]
        running_mean = sum(x.peak_hz for x in current) / len(current)
        gap_s = w.start_s - current[-1].start_s
        if abs(w.peak_hz - running_mean) <= freq_tolerance_hz and gap_s <= max_gap_s:
            current.append(w)
        else:
            groups.append([w])

    sustained = []
    for group in groups:
        start_s = group[0].start_s
        duration_s = (group[-1].start_s - group[0].start_s) + window_s
        # min_windows: with the default min_duration_s == window_s, every single
        # window used to count as "sustained" by itself (duration_s == window_s
        # >= min_duration_s), which defeats the function's stated purpose. A
        # sustained tone has to survive at least min_windows consecutive looks.
        if duration_s >= min_duration_s and len(group) >= min_windows:
            mean_peak = sum(x.peak_hz for x in group) / len(group)
            mean_lower = sum(x.lower_hz for x in group) / len(group)
            mean_upper = sum(x.upper_hz for x in group) / len(group)
            sustained.append(
                SustainedBand(
                    start_s=start_s,
                    end_s=group[-1].start_s + window_s,  # end of the last window, so end_s - start_s == duration_s
                    duration_s=duration_s,
                    mean_peak_hz=mean_peak,
                    mean_lower_hz=mean_lower,
                    mean_upper_hz=mean_upper,
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

    window_s = 2.0  # matches rolling_analyze()'s own default -- kept explicit
    # here since find_sustained_bands needs the exact value, not a default.
    samples, sample_rate = load_wav_mono(sys.argv[1])
    windows = list(rolling_analyze(samples, sample_rate, window_s=window_s))
    print(f"{len(windows)} windows analyzed")
    for w in windows:
        print(f"  t={w.start_s:5.1f}s  peak={w.peak_hz:7.1f} Hz  band=[{w.lower_hz:.0f}, {w.upper_hz:.0f}] Hz")

    sustained = find_sustained_bands(windows, window_s=window_s)
    print(f"\n{len(sustained)} sustained band(s) (>= 2s, held within 200 Hz):")
    for s in sustained:
        print(
            f"  {s.start_s:.1f}s-{s.end_s:.1f}s ({s.duration_s:.1f}s, "
            f"{s.window_count} windows): ~{s.mean_peak_hz:.0f} Hz "
            f"[{s.mean_lower_hz:.0f}, {s.mean_upper_hz:.0f}] Hz"
        )
