"""Translate a detected (lower_hz, upper_hz) frequency band into the exact
wire format Haven's firmware expects on the FreqRange BLE characteristic.

Characteristic: 7a1e0003-4b5c-4e8a-9c1a-2f6b8d3c9a10
Service:        7a1e0001-4b5c-4e8a-9c1a-2f6b8d3c9a10  (Haven Audio service)
Wire format:    4 bytes, little-endian: uint16 lower_hz, uint16 upper_hz
                (see haven-zephyr-app/src/gatt_audio_service.c:read_freq_range /
                apply_freq_range, and gatt_audio_service.h for the struct).

Valid range is firmware-enforced as [FREQ_MIN_HZ, FREQ_MAX_HZ] = [200, 8000].
Values outside that range are clamped here so this tool can't hand the
firmware something it would otherwise have to reject.
"""
import struct

FREQ_MIN_HZ = 200
FREQ_MAX_HZ = 8000


def clamp_band(lower_hz, upper_hz):
    """Clamp a (lower_hz, upper_hz) band into [FREQ_MIN_HZ, FREQ_MAX_HZ],
    preserving lower <= upper. Returns (lower_hz, upper_hz) as ints.
    """
    lower_hz = max(FREQ_MIN_HZ, min(FREQ_MAX_HZ, round(lower_hz)))
    upper_hz = max(FREQ_MIN_HZ, min(FREQ_MAX_HZ, round(upper_hz)))
    if lower_hz > upper_hz:
        lower_hz, upper_hz = upper_hz, lower_hz
    return lower_hz, upper_hz


def encode_freq_range(lower_hz, upper_hz):
    """Clamp and pack a frequency band into the 4-byte little-endian wire
    format the FreqRange characteristic expects.
    """
    lower_hz, upper_hz = clamp_band(lower_hz, upper_hz)
    return struct.pack("<HH", lower_hz, upper_hz)


def decode_freq_range(wire_bytes):
    """Inverse of encode_freq_range() -- mainly useful for tests/inspection."""
    lower_hz, upper_hz = struct.unpack("<HH", wire_bytes)
    return lower_hz, upper_hz


if __name__ == "__main__":
    example = encode_freq_range(1800, 3200)
    print(f"encode_freq_range(1800, 3200) -> {example!r} (hex: {example.hex()})")
    print(f"decoded back -> {decode_freq_range(example)}")
