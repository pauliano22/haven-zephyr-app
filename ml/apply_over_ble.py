"""Bench script: analyze a .wav for its troublesome frequency band, then
write the result straight to a connected Haven board's FreqRange BLE
characteristic. Closes the loop from "recorded a problem sound" to "board is
filtering for it" without needing the phone app or tools/ble_bench_test.html
in between.

Mirrors the exact UUIDs and write semantics tools/ble_bench_test.html already
uses (see that file's SERVICE_UUID / FREQ_CHAR_UUID and its
freqChar.writeValueWithResponse(...) call) so this is a second, independent
client speaking the same protocol, not a new one.

Usage:
    python3 apply_over_ble.py recording.wav                  # scan + connect + write
    python3 apply_over_ble.py recording.wav --device Haven   # match by name (default)
    python3 apply_over_ble.py recording.wav --dry-run        # analyze + print only,
                                                               # no BLE required at all
    python3 apply_over_ble.py recording.wav --rolling        # use rolling_analyze.py
                                                               # instead of one whole-clip pass --
                                                               # see compute_sustained_payload()

Requires `bleak` (see requirements.txt) and a working Bluetooth adapter for
anything other than --dry-run. Neither was available in the environment this
was written in, so the live-write path is implemented and unit-testable
end-to-end up to the point of the actual radio write, but has not been
exercised against a real board -- verify that leg for real before trusting it
unattended.
"""
import argparse
import asyncio
import sys

from analyze import analyze_file, load_wav_mono
from ble_translator import encode_freq_range
from rolling_analyze import find_sustained_bands, rolling_analyze

SERVICE_UUID = "7a1e0001-4b5c-4e8a-9c1a-2f6b8d3c9a10"
FREQ_CHAR_UUID = "7a1e0003-4b5c-4e8a-9c1a-2f6b8d3c9a10"


def compute_payload(wav_path):
    """Analyze wav_path and return (peak_hz, lower_hz, upper_hz, wire_bytes)."""
    peak_hz, lower_hz, upper_hz = analyze_file(wav_path)
    wire = encode_freq_range(lower_hz, upper_hz)
    return peak_hz, lower_hz, upper_hz, wire


def compute_sustained_payload(wav_path, window_s=2.0, hop_s=1.0, min_duration_s=2.0):
    """Like compute_payload(), but uses rolling_analyze.py's sliding-window
    scan instead of one whole-clip pass, and applies the *longest* sustained
    band found rather than whatever a single Welch estimate over the entire
    recording happens to pick out.

    Why this is worth having as a separate mode, not just a strict upgrade:
    it deliberately refuses a recording that never held one tone steady for
    at least min_duration_s -- a single loud transient (a door slam, a car
    horn) that compute_payload() would confidently report a band for
    instead correctly produces nothing here. Returns None in that case;
    callers must handle it (main() below prints a clear message and exits
    without writing anything over BLE, rather than writing a moment's noise
    as if it were a real ongoing problem).
    """
    samples, sample_rate = load_wav_mono(wav_path)
    windows = list(rolling_analyze(samples, sample_rate, window_s=window_s, hop_s=hop_s))
    sustained = find_sustained_bands(windows, window_s=window_s, min_duration_s=min_duration_s)
    if not sustained:
        return None

    longest = max(sustained, key=lambda band: band.duration_s)
    wire = encode_freq_range(longest.mean_lower_hz, longest.mean_upper_hz)
    return longest, wire


async def write_over_ble(wire_bytes, device_name, timeout_s):
    """Scan for a device advertising `device_name`, connect, and write
    wire_bytes to the FreqRange characteristic. Raises on any failure --
    callers should let that propagate, this is a bench tool, not a service.
    """
    from bleak import BleakClient, BleakScanner

    print(f"Scanning for a device named \"{device_name}\" ({timeout_s}s)...")
    device = await BleakScanner.find_device_by_name(device_name, timeout=timeout_s)
    if device is None:
        raise RuntimeError(
            f'No device advertising as "{device_name}" found within {timeout_s}s'
        )

    print(f"Connecting to {device.address}...")
    async with BleakClient(device) as client:
        print(f"Writing {wire_bytes.hex()} to FreqRange characteristic...")
        await client.write_gatt_char(FREQ_CHAR_UUID, wire_bytes, response=True)
        print("Write acknowledged.")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("wav_path", help="Path to the .wav recording to analyze")
    parser.add_argument(
        "--device", default="Haven", help='BLE advertised name to connect to (default: "Haven")'
    )
    parser.add_argument(
        "--timeout", type=float, default=10.0, help="BLE scan timeout in seconds (default: 10)"
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Analyze and print the payload only -- no BLE, no bleak import required",
    )
    parser.add_argument(
        "--rolling",
        action="store_true",
        help=(
            "Use rolling_analyze.py's sliding-window scan instead of one whole-clip "
            "pass, and only apply a band that held sustained for at least "
            "--min-duration seconds -- see compute_sustained_payload()'s docstring"
        ),
    )
    parser.add_argument(
        "--min-duration",
        type=float,
        default=2.0,
        help="With --rolling: minimum seconds a band must hold to count as sustained (default: 2.0)",
    )
    args = parser.parse_args()

    if args.rolling:
        result = compute_sustained_payload(args.wav_path, min_duration_s=args.min_duration)
        if result is None:
            print(
                f"No band held sustained for at least {args.min_duration:.1f}s -- "
                "not applying anything. (A single loud moment isn't a real ongoing "
                "problem; re-record if this seems wrong.)"
            )
            return
        band, wire = result
        print(
            f"Sustained band: ~{band.mean_peak_hz:.1f} Hz for {band.duration_s:.1f}s "
            f"({band.window_count} windows)"
        )
        print(f"Band edges: [{band.mean_lower_hz:.1f}, {band.mean_upper_hz:.1f}] Hz")
        print(f"FreqRange payload: {wire.hex()}")
    else:
        peak_hz, lower_hz, upper_hz, wire = compute_payload(args.wav_path)
        print(f"Dominant peak: {peak_hz:.1f} Hz")
        print(f"Troublesome band (-3dB): [{lower_hz:.1f}, {upper_hz:.1f}] Hz")
        print(f"FreqRange payload: {wire.hex()}")

    if args.dry_run:
        print("(--dry-run: not connecting over BLE)")
        return

    try:
        asyncio.run(write_over_ble(wire, args.device, args.timeout))
    except ImportError:
        print(
            "\nbleak is not installed -- run `pip install -r requirements.txt` "
            "in this directory, or use --dry-run to skip the BLE write.",
            file=sys.stderr,
        )
        sys.exit(1)


if __name__ == "__main__":
    main()
