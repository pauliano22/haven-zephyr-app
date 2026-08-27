/* Host test for gatt_audio_service.c's WRITE VALIDATION logic
 * (write_volume/write_freq_range) -- the real production file, compiled
 * unmodified via the fake zephyr/bluetooth+settings headers in
 * tests/host/fakes/. Does NOT exercise real BLE transport, ATT bearer, or
 * notification delivery -- those fakes are link-satisfying stubs, not a
 * real GATT server. What IS real: the accept/reject/clamp decisions in
 * write_volume()/write_freq_range(), the little-endian wire encode/decode
 * for FreqRange, and the registered-callback firing path.
 */
#include "test_harness.h"

#include "../../src/gatt_audio_service.c"

static uint8_t last_volume_cb_value;
static int volume_cb_calls;
static struct audio_freq_range last_freq_cb_value;
static int freq_cb_calls;

static void on_volume(uint8_t v)
{
	last_volume_cb_value = v;
	volume_cb_calls++;
}

static void on_freq(const struct audio_freq_range *r)
{
	last_freq_cb_value = *r;
	freq_cb_calls++;
}

static void reset_state(void)
{
	current_volume_pct = 100;
	current_freq_range.lower_hz = AUDIO_FREQ_MIN_HZ;
	current_freq_range.upper_hz = AUDIO_FREQ_MAX_HZ;
	volume_notify_enabled = false;
	freq_range_notify_enabled = false;
	volume_cb_calls = 0;
	freq_cb_calls = 0;
	gatt_audio_service_set_callbacks(on_volume, on_freq);
}

static void test_write_volume_accepts_in_range(void)
{
	reset_state();
	uint8_t val = 42;
	ssize_t rc = write_volume(NULL, NULL, &val, sizeof(val), 0, 0);

	CHECK(rc == (ssize_t)sizeof(val));
	CHECK(current_volume_pct == 42);
	CHECK(volume_cb_calls == 1);
	CHECK(last_volume_cb_value == 42);
}

static void test_write_volume_boundary_values(void)
{
	reset_state();
	uint8_t zero = 0;
	uint8_t max = AUDIO_VOLUME_MAX_PCT;

	CHECK(write_volume(NULL, NULL, &zero, 1, 0, 0) == 1);
	CHECK(current_volume_pct == 0);
	CHECK(write_volume(NULL, NULL, &max, 1, 0, 0) == 1);
	CHECK(current_volume_pct == AUDIO_VOLUME_MAX_PCT);
}

static void test_write_volume_rejects_out_of_range_without_changing_state(void)
{
	reset_state();
	current_volume_pct = 77; /* sentinel: a rejection must leave this untouched */

	uint8_t too_high = AUDIO_VOLUME_MAX_PCT + 1; /* 101 */
	ssize_t rc = write_volume(NULL, NULL, &too_high, 1, 0, 0);

	CHECK(rc == BT_GATT_ERR(BT_ATT_ERR_OUT_OF_RANGE));
	/* Rejected, not clamped -- unlike protocol.c's MULTI_FILTER path,
	 * this module's header explicitly documents "rejected outright...
	 * rather than clamped". A rejection must not silently apply 100.
	 */
	CHECK(current_volume_pct == 77);
	CHECK(volume_cb_calls == 0);

	uint8_t max_uint8 = 255;

	rc = write_volume(NULL, NULL, &max_uint8, 1, 0, 0);
	CHECK(rc == BT_GATT_ERR(BT_ATT_ERR_OUT_OF_RANGE));
	CHECK(current_volume_pct == 77);
}

static void test_write_volume_rejects_bad_length_and_offset(void)
{
	reset_state();
	current_volume_pct = 55;

	uint8_t two_bytes[2] = {1, 2};
	CHECK(write_volume(NULL, NULL, two_bytes, 2, 0, 0) ==
	      BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN));
	CHECK(current_volume_pct == 55);

	uint8_t val = 10;
	CHECK(write_volume(NULL, NULL, &val, 1, 5 /* nonzero offset */, 0) ==
	      BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET));
	CHECK(current_volume_pct == 55);
}

static void test_write_freq_range_accepts_in_range_and_decodes_wire_correctly(void)
{
	reset_state();
	uint8_t wire[4];

	sys_put_le16(1000, &wire[0]);
	sys_put_le16(5000, &wire[2]);

	ssize_t rc = write_freq_range(NULL, NULL, wire, 4, 0, 0);

	CHECK(rc == 4);
	CHECK(current_freq_range.lower_hz == 1000);
	CHECK(current_freq_range.upper_hz == 5000);
	CHECK(freq_cb_calls == 1);
	CHECK(last_freq_cb_value.lower_hz == 1000);
	CHECK(last_freq_cb_value.upper_hz == 5000);
}

static void test_write_freq_range_boundary_values(void)
{
	reset_state();
	uint8_t wire[4];

	sys_put_le16(AUDIO_FREQ_MIN_HZ, &wire[0]);
	sys_put_le16(AUDIO_FREQ_MAX_HZ, &wire[2]);
	CHECK(write_freq_range(NULL, NULL, wire, 4, 0, 0) == 4);
	CHECK(current_freq_range.lower_hz == AUDIO_FREQ_MIN_HZ);
	CHECK(current_freq_range.upper_hz == AUDIO_FREQ_MAX_HZ);
}

static void test_write_freq_range_rejects_out_of_bounds(void)
{
	reset_state();
	current_freq_range.lower_hz = 111;
	current_freq_range.upper_hz = 222;
	uint8_t wire[4];

	/* lower below AUDIO_FREQ_MIN_HZ */
	sys_put_le16(AUDIO_FREQ_MIN_HZ - 1, &wire[0]);
	sys_put_le16(5000, &wire[2]);
	CHECK(write_freq_range(NULL, NULL, wire, 4, 0, 0) ==
	      BT_GATT_ERR(BT_ATT_ERR_OUT_OF_RANGE));
	CHECK(current_freq_range.lower_hz == 111);

	/* upper above AUDIO_FREQ_MAX_HZ */
	sys_put_le16(1000, &wire[0]);
	sys_put_le16(AUDIO_FREQ_MAX_HZ + 1, &wire[2]);
	CHECK(write_freq_range(NULL, NULL, wire, 4, 0, 0) ==
	      BT_GATT_ERR(BT_ATT_ERR_OUT_OF_RANGE));
	CHECK(current_freq_range.upper_hz == 222);
}

static void test_write_freq_range_rejects_lower_not_strictly_below_upper(void)
{
	reset_state();
	current_freq_range.lower_hz = 111;
	current_freq_range.upper_hz = 222;
	uint8_t wire[4];

	/* lower == upper -- must be rejected ("lower < upper required"). */
	sys_put_le16(4000, &wire[0]);
	sys_put_le16(4000, &wire[2]);
	CHECK(write_freq_range(NULL, NULL, wire, 4, 0, 0) ==
	      BT_GATT_ERR(BT_ATT_ERR_OUT_OF_RANGE));
	CHECK(current_freq_range.lower_hz == 111);

	/* lower > upper. */
	sys_put_le16(5000, &wire[0]);
	sys_put_le16(1000, &wire[2]);
	CHECK(write_freq_range(NULL, NULL, wire, 4, 0, 0) ==
	      BT_GATT_ERR(BT_ATT_ERR_OUT_OF_RANGE));
	CHECK(current_freq_range.upper_hz == 222);
}

static void test_write_freq_range_rejects_bad_length(void)
{
	reset_state();
	uint8_t three_bytes[3] = {1, 2, 3};

	CHECK(write_freq_range(NULL, NULL, three_bytes, 3, 0, 0) ==
	      BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN));
}

static void test_read_volume_and_read_freq_range_roundtrip(void)
{
	reset_state();
	current_volume_pct = 63;

	uint8_t buf[8];
	ssize_t n = read_volume(NULL, NULL, buf, sizeof(buf), 0);

	CHECK(n == 1);
	CHECK(buf[0] == 63);

	current_freq_range.lower_hz = 300;
	current_freq_range.upper_hz = 6000;
	n = read_freq_range(NULL, NULL, buf, sizeof(buf), 0);
	CHECK(n == 4);
	CHECK(sys_get_le16(&buf[0]) == 300);
	CHECK(sys_get_le16(&buf[2]) == 6000);
}

static void test_trusted_path_skips_att_validation(void)
{
	/* gatt_audio_set_volume()/gatt_audio_set_freq_range() are documented
	 * as the trusted, non-BLE-write path (settings restore) that "skips
	 * ATT-layer validation since the caller is expected to already be
	 * handing back a value this module itself previously accepted and
	 * saved." Verify that contract precisely: this path applies values
	 * write_volume() would reject, with NO error path at all (it
	 * returns void). This is a real, load-bearing distinction between
	 * the two entry points -- worth pinning down explicitly rather than
	 * assuming.
	 */
	reset_state();
	gatt_audio_set_volume(255); /* write_volume() would reject this */
	CHECK(current_volume_pct == 255);
	CHECK(volume_cb_calls == 1);
	CHECK(last_volume_cb_value == 255);

	struct audio_freq_range bogus = { .lower_hz = 1, .upper_hz = 0 }; /* inverted */
	gatt_audio_set_freq_range(&bogus);
	CHECK(current_freq_range.lower_hz == 1);
	CHECK(current_freq_range.upper_hz == 0);
}

int main(void)
{
	RUN(test_write_volume_accepts_in_range);
	RUN(test_write_volume_boundary_values);
	RUN(test_write_volume_rejects_out_of_range_without_changing_state);
	RUN(test_write_volume_rejects_bad_length_and_offset);
	RUN(test_write_freq_range_accepts_in_range_and_decodes_wire_correctly);
	RUN(test_write_freq_range_boundary_values);
	RUN(test_write_freq_range_rejects_out_of_bounds);
	RUN(test_write_freq_range_rejects_lower_not_strictly_below_upper);
	RUN(test_write_freq_range_rejects_bad_length);
	RUN(test_read_volume_and_read_freq_range_roundtrip);
	RUN(test_trusted_path_skips_att_validation);
	return haven_test_summary("test_gatt_validation");
}
