/* Host test for settings_store.c's haven_settings_set() key-parsing/
 * dispatch logic -- the real production file, compiled unmodified via the
 * fake zephyr/settings/settings.h in tests/host/fakes/.
 *
 * This is NOT a real flash/NVS round-trip test: there's no native_sim (or
 * any other Zephyr board target) build available in this sandbox to host
 * a real settings backend against simulated flash, so a genuine save-then-
 * reboot-then-restore test isn't attempted here rather than faked into
 * looking like one. What IS tested for real: given a settings key name and
 * a read callback (exactly the two inputs Zephyr's settings subsystem
 * actually hands to a registered handler), does haven_settings_set()
 * correctly dispatch to the right consumer, reject the wrong length, and
 * propagate a read failure -- using deliberately ISOLATED test-double
 * gatt_audio_set_volume()/gatt_audio_set_freq_range() (recording calls,
 * not the real gatt_audio_service.c) so this test doesn't also depend on
 * that module's own correctness.
 */
#include "test_harness.h"

#include <stdbool.h>
#include <string.h>

#include "../../src/gatt_audio_service.h"

static uint8_t last_volume_set;
static int volume_set_calls;
static struct audio_freq_range last_freq_set;
static int freq_set_calls;

void gatt_audio_set_volume(uint8_t volume_pct)
{
	last_volume_set = volume_pct;
	volume_set_calls++;
}

void gatt_audio_set_freq_range(const struct audio_freq_range *range)
{
	last_freq_set = *range;
	freq_set_calls++;
}

#include "../../src/settings_store.c"

/* Fake settings_read_cb: copies up to `len` bytes from a fixed buffer into
 * the caller's destination, or simulates a hardware/storage read failure
 * if `fail_reads` is set.
 */
static const uint8_t *fake_read_buf;
static size_t fake_read_buf_len;
static bool fail_reads;

static ssize_t fake_read_cb(void *cb_arg, void *data, size_t len)
{
	(void)cb_arg;
	if (fail_reads) {
		return -5; /* arbitrary negative errno-shaped value */
	}
	size_t n = len < fake_read_buf_len ? len : fake_read_buf_len;

	memcpy(data, fake_read_buf, n);
	return (ssize_t)n;
}

static void reset(void)
{
	volume_set_calls = 0;
	freq_set_calls = 0;
	fail_reads = false;
}

static void test_volume_dispatch_success(void)
{
	reset();
	uint8_t stored = 73;

	fake_read_buf = &stored;
	fake_read_buf_len = sizeof(stored);

	int rc = haven_settings_set("volume", sizeof(stored), fake_read_cb, NULL);

	CHECK(rc == 0);
	CHECK(volume_set_calls == 1);
	CHECK(last_volume_set == 73);
	CHECK(freq_set_calls == 0);
}

static void test_volume_dispatch_wrong_length(void)
{
	reset();
	uint8_t stored[2] = {1, 2};

	fake_read_buf = stored;
	fake_read_buf_len = sizeof(stored);

	int rc = haven_settings_set("volume", 2 /* real key is 1 byte */, fake_read_cb, NULL);

	CHECK(rc == -EINVAL);
	CHECK(volume_set_calls == 0);
}

static void test_volume_dispatch_read_failure_propagates(void)
{
	reset();
	fail_reads = true;

	int rc = haven_settings_set("volume", 1, fake_read_cb, NULL);

	CHECK(rc == -EIO);
	CHECK(volume_set_calls == 0);
}

static void test_freq_dispatch_success(void)
{
	reset();
	struct audio_freq_range stored = { .lower_hz = 500, .upper_hz = 7000 };

	fake_read_buf = (const uint8_t *)&stored;
	fake_read_buf_len = sizeof(stored);

	int rc = haven_settings_set("freq", sizeof(stored), fake_read_cb, NULL);

	CHECK(rc == 0);
	CHECK(freq_set_calls == 1);
	CHECK(last_freq_set.lower_hz == 500);
	CHECK(last_freq_set.upper_hz == 7000);
	CHECK(volume_set_calls == 0);
}

static void test_freq_dispatch_wrong_length(void)
{
	reset();
	uint8_t garbage[3] = {0, 0, 0};

	fake_read_buf = garbage;
	fake_read_buf_len = sizeof(garbage);

	int rc = haven_settings_set("freq", sizeof(garbage), fake_read_cb, NULL);

	CHECK(rc == -EINVAL);
	CHECK(freq_set_calls == 0);
}

static void test_freq_dispatch_read_failure_propagates(void)
{
	reset();
	fail_reads = true;

	int rc = haven_settings_set("freq", sizeof(struct audio_freq_range), fake_read_cb, NULL);

	CHECK(rc == -EIO);
	CHECK(freq_set_calls == 0);
}

static void test_unknown_key_returns_enoent(void)
{
	reset();
	uint8_t x = 0;

	fake_read_buf = &x;
	fake_read_buf_len = 1;

	CHECK(haven_settings_set("bogus", 1, fake_read_cb, NULL) == -ENOENT);
	CHECK(volume_set_calls == 0 && freq_set_calls == 0);
}

static void test_subtree_vs_leaf_name_matching(void)
{
	/* "volume/extra" has "volume" as its first path segment but is NOT
	 * the leaf itself (settings_name_steq's `next` would be non-NULL,
	 * pointing at "extra") -- haven_settings_set()'s `&& !next` guard
	 * must NOT treat this as a volume write. This is exercising the real
	 * settings_name_steq() semantics from the fake header (a real
	 * reimplementation of that Zephyr helper's contract, not a no-op),
	 * combined with the real production dispatch guard.
	 */
	reset();
	uint8_t x = 42;

	fake_read_buf = &x;
	fake_read_buf_len = 1;

	CHECK(haven_settings_set("volume/extra", 1, fake_read_cb, NULL) == -ENOENT);
	CHECK(volume_set_calls == 0);

	CHECK(haven_settings_set("freq/extra", 1, fake_read_cb, NULL) == -ENOENT);
	CHECK(freq_set_calls == 0);

	/* Sanity: the exact leaf name still matches after confirming the
	 * subtree case correctly does NOT.
	 */
	CHECK(haven_settings_set("volume", 1, fake_read_cb, NULL) == 0);
	CHECK(volume_set_calls == 1);
}

int main(void)
{
	RUN(test_volume_dispatch_success);
	RUN(test_volume_dispatch_wrong_length);
	RUN(test_volume_dispatch_read_failure_propagates);
	RUN(test_freq_dispatch_success);
	RUN(test_freq_dispatch_wrong_length);
	RUN(test_freq_dispatch_read_failure_propagates);
	RUN(test_unknown_key_returns_enoent);
	RUN(test_subtree_vs_leaf_name_matching);
	return haven_test_summary("test_settings_dispatch");
}
