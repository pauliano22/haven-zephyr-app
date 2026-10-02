#!/usr/bin/env bash
# Host-side unit tests: plain gcc, no Zephyr/native_sim/twister. See
# tests/host/README.md for why, and what this does/doesn't cover.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"

BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

TESTS="test_protocol test_ack test_biquad_pipeline test_adau1860_coeffs test_eq_route test_tone_path test_gatt_validation test_settings_dispatch test_tac5301_coeffs"
FAIL=0

for t in $TESTS; do
	echo "=== building $t ==="
	# test_tac5301_coeffs needs its own zephyr/drivers/i2c.h (the TAC5301's
	# I2C wire format is a single register-address byte, not the ADAU1860's
	# 4-byte scheme) -- fakes_tac5301 is searched first so that one header
	# comes from there, everything else still resolves from fakes/.
	if [ "$t" = "test_tac5301_coeffs" ]; then
		INCLUDES="-Ifakes_tac5301 -Ifakes"
	else
		INCLUDES="-Ifakes"
	fi
	if ! gcc -std=c11 -Wall -Wextra -Wno-unused-parameter $INCLUDES \
		-o "$BUILD_DIR/$t" "$t.c" -lm; then
		echo "BUILD FAILED: $t"
		FAIL=1
		continue
	fi
	echo "=== running $t ==="
	if ! "$BUILD_DIR/$t"; then
		FAIL=1
	fi
	echo
done

if [ "$FAIL" -eq 0 ]; then
	echo "ALL TEST SUITES PASSED"
else
	echo "ONE OR MORE TEST SUITES FAILED"
fi
exit $FAIL
