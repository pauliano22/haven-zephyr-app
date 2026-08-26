import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from ble_translator import (
    FREQ_MAX_HZ,
    FREQ_MIN_HZ,
    clamp_band,
    decode_freq_range,
    encode_freq_range,
)


class ClampBandTests(unittest.TestCase):
    def test_within_range_unchanged(self):
        self.assertEqual(clamp_band(1000, 2000), (1000, 2000))

    def test_below_min_clamped_up(self):
        self.assertEqual(clamp_band(50, 2000), (FREQ_MIN_HZ, 2000))

    def test_above_max_clamped_down(self):
        self.assertEqual(clamp_band(1000, 20000), (1000, FREQ_MAX_HZ))

    def test_both_out_of_range(self):
        self.assertEqual(clamp_band(0, 999999), (FREQ_MIN_HZ, FREQ_MAX_HZ))

    def test_exact_boundaries_unchanged(self):
        self.assertEqual(clamp_band(FREQ_MIN_HZ, FREQ_MAX_HZ), (FREQ_MIN_HZ, FREQ_MAX_HZ))

    def test_inverted_bounds_swapped(self):
        # Defensive: if a caller passes (upper, lower) by mistake, don't
        # silently ship a nonsensical range to the firmware.
        self.assertEqual(clamp_band(3000, 1000), (1000, 3000))

    def test_rounds_floats(self):
        self.assertEqual(clamp_band(1000.4, 2000.6), (1000, 2001))


class EncodeDecodeTests(unittest.TestCase):
    def test_round_trip(self):
        wire = encode_freq_range(1800, 3200)
        self.assertEqual(decode_freq_range(wire), (1800, 3200))

    def test_wire_is_four_bytes(self):
        wire = encode_freq_range(1000, 2000)
        self.assertEqual(len(wire), 4)

    def test_little_endian_byte_order(self):
        # lower_hz=1 -> bytes [0x01, 0x00]; upper_hz=2 -> bytes [0x02, 0x00].
        # Little-endian means the low byte comes first in each field.
        wire = encode_freq_range(FREQ_MIN_HZ, FREQ_MIN_HZ)
        expected = struct.pack("<HH", FREQ_MIN_HZ, FREQ_MIN_HZ)
        self.assertEqual(wire, expected)

    def test_matches_manual_little_endian_packing(self):
        lower_hz, upper_hz = 500, 4000
        wire = encode_freq_range(lower_hz, upper_hz)
        manual = bytes([
            lower_hz & 0xFF, (lower_hz >> 8) & 0xFF,
            upper_hz & 0xFF, (upper_hz >> 8) & 0xFF,
        ])
        self.assertEqual(wire, manual)

    def test_encode_clamps_out_of_range_input(self):
        wire = encode_freq_range(10, 50000)
        self.assertEqual(decode_freq_range(wire), (FREQ_MIN_HZ, FREQ_MAX_HZ))


if __name__ == "__main__":
    unittest.main()
