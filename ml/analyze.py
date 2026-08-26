"""Find the most "troublesome" frequency band in a .wav recording, for
feeding into Haven's FreqRange BLE characteristic (see ble_translator.py).

Heuristic (documented explicitly since "troublesome" is a design choice,
not a fact): hearing-protection-relevant noise problems are very often a
single dominant narrowband component -- a machine whine, an alarm tone,
audio feedback, a motor harmonic -- riding on top of a broader noise
floor. Rather than a broad "loudest octave" measure (which just tracks
overall spectral tilt and would flag almost any voice/music clip), this
tool:

  1. Computes a power spectral density (PSD) estimate over the whole
     clip via Welch's method (scipy.signal.welch) -- averaging over many
     overlapping windows makes the estimate robust to transient noise
     and does not depend on exact timing within the clip.
  2. Finds the single dominant spectral peak in that PSD.
  3. Reports the -3 dB (half-power) bandwidth around that peak as the
     "troublesome band" -- the standard way to characterize how wide a
     tonal/narrowband component is, e.g. in audio and RF engineering.

This deliberately does NOT attempt full source separation or a learned
model -- there's no labeled "troublesome vs. fine" dataset for this
project yet, so a narrowband-peak heuristic is the honest, inspectable
starting point. See PROPOSALS.md for a note on where a learned approach
could go later.
"""
import numpy as np
from scipy import signal
from scipy.io import wavfile


def load_wav_mono(path):
    """Load a .wav file and return (samples, sample_rate) as float32 in
    [-1, 1], downmixed to mono if the file is multi-channel.
    """
    sample_rate, data = wavfile.read(path)

    if data.ndim > 1:
        data = data.mean(axis=1)

    if np.issubdtype(data.dtype, np.integer):
        max_val = np.iinfo(data.dtype).max
        data = data.astype(np.float32) / max_val
    else:
        data = data.astype(np.float32)

    return data, sample_rate


def find_troublesome_band(samples, sample_rate, search_min_hz=20.0,
                           search_max_hz=None):
    """Return (peak_hz, lower_hz, upper_hz) for the dominant spectral peak
    and its -3 dB (half-power) bandwidth.

    search_min_hz / search_max_hz restrict which part of the spectrum is
    considered when picking the peak (default: 20 Hz to Nyquist) -- this
    is independent of the [200, 8000] Hz clamp applied later by
    ble_translator.encode_freq_range(), which is a firmware/hardware
    constraint, not an analysis one.
    """
    if search_max_hz is None:
        search_max_hz = sample_rate / 2.0

    freqs, psd = signal.welch(samples, fs=sample_rate, nperseg=min(4096, len(samples)))

    band_mask = (freqs >= search_min_hz) & (freqs <= search_max_hz)
    if not np.any(band_mask):
        raise ValueError(
            f"No spectral content between {search_min_hz} and {search_max_hz} Hz "
            f"(sample_rate={sample_rate})"
        )

    freqs = freqs[band_mask]
    psd = psd[band_mask]

    peak_idx = int(np.argmax(psd))
    peak_hz = float(freqs[peak_idx])
    half_power = psd[peak_idx] / 2.0

    lower_idx = peak_idx
    while lower_idx > 0 and psd[lower_idx] >= half_power:
        lower_idx -= 1
    upper_idx = peak_idx
    while upper_idx < len(psd) - 1 and psd[upper_idx] >= half_power:
        upper_idx += 1

    return peak_hz, float(freqs[lower_idx]), float(freqs[upper_idx])


def analyze_file(path):
    """Convenience wrapper: load a .wav and return its troublesome band."""
    samples, sample_rate = load_wav_mono(path)
    return find_troublesome_band(samples, sample_rate)


if __name__ == "__main__":
    import sys

    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <path-to-wav>")
        sys.exit(1)

    peak_hz, lower_hz, upper_hz = analyze_file(sys.argv[1])
    print(f"Dominant peak: {peak_hz:.1f} Hz")
    print(f"Troublesome band (-3dB): [{lower_hz:.1f}, {upper_hz:.1f}] Hz")

    from ble_translator import encode_freq_range

    wire = encode_freq_range(lower_hz, upper_hz)
    print(f"FreqRange characteristic payload: {wire.hex()}")
