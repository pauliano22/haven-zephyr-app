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

from analyze import analyze_file
from ble_translator import encode_freq_range

SERVICE_UUID = "7a1e0001-4b5c-4e8a-9c1a-2f6b8d3c9a10"
FREQ_CHAR_UUID = "7a1e0003-4b5c-4e8a-9c1a-2f6b8d3c9a10"


def compute_payload(wav_path):
    """Analyze wav_path and return (peak_hz, lower_hz, upper_hz, wire_bytes)."""
    peak_hz, lower_hz, upper_hz = analyze_file(wav_path)
    wire = encode_freq_range(lower_hz, upper_hz)
    return peak_hz, lower_hz, upper_hz, wire


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
    args = parser.parse_args()

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
