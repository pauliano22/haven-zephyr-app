/* Host test for mock_audio_pipeline.c's biquad DSP -- the REAL production
 * file is compiled in unmodified (via fake zephyr/ headers, see
 * tests/host/fakes/); recompute_filter()/process_buffer()/
 * generate_test_tone()/on_volume_changed() are `static` in that file, so
 * this only works because we #include the .c file directly into this
 * translation unit, not because they were made non-static for testing.
 *
 * These are functional DSP correctness checks (does the filter actually
 * pass its center frequency and reject far-off frequencies?), not
 * hand-derived expected coefficient constants -- deriving exact expected
 * b0/b1/b2/a1/a2 float values by hand and asserting against them would
 * just be re-deriving the RBJ formula a second time and risking testing
 * my own arithmetic instead of the production code's behavior.
 */
#include "test_harness.h"

#include "../../src/gatt_audio_service.h"

/* Link-satisfying stub: mock_audio_pipeline_init() (not called by these
 * tests, but still compiled as part of the real .c file) calls this real
 * gatt_audio_service.h function. Never exercised.
 */
void gatt_audio_service_set_callbacks(audio_volume_changed_cb_t on_volume,
				       audio_freq_range_changed_cb_t on_freq_range)
{
	(void)on_volume;
	(void)on_freq_range;
}

#include "../../src/mock_audio_pipeline.c"

static void generate_sine(float *buf, size_t len, float freq_hz, float *phase)
{
	float w = 2.0f * (float)M_PI * (freq_hz / MOCK_SAMPLE_RATE_HZ);

	for (size_t i = 0; i < len; i++) {
		buf[i] = sinf(*phase);
		*phase += w;
	}
}

/* Runs `cycles_to_settle` full buffers through the filter first (letting
 * any startup transient die out) and returns the peak of the NEXT buffer.
 */
static float settled_peak_at(float freq_hz)
{
	float phase = 0.0f;
	float buf[256];

	for (int i = 0; i < 40; i++) {
		generate_sine(buf, 256, freq_hz, &phase);
		process_buffer(buf, 256);
	}
	generate_sine(buf, 256, freq_hz, &phase);
	return process_buffer(buf, 256);
}

static void test_bandpass_passes_center_rejects_far_off(void)
{
	struct audio_freq_range range = { .lower_hz = 900, .upper_hz = 1100 };

	recompute_filter(&range);
	current_gain = 1.0f;

	float center_peak = settled_peak_at(1000.0f);
	/* Constant-skirt-gain RBJ bandpass: unity gain at the center freq by
	 * construction. Allow some tolerance for float accumulation over
	 * many buffers, not for the design itself being approximate.
	 */
	CHECK(center_peak > 0.9f && center_peak < 1.1f);

	/* Two octaves below center (250 Hz) should be substantially
	 * attenuated at Q = center/bandwidth = 1000/200 = 5.
	 */
	float far_peak = settled_peak_at(250.0f);

	CHECK(far_peak < 0.3f);
	CHECK(far_peak < center_peak);
}

static void test_recompute_resets_filter_memory(void)
{
	struct audio_freq_range range = { .lower_hz = 900, .upper_hz = 1100 };

	recompute_filter(&range);
	current_gain = 1.0f;

	/* Build up nonzero filter memory. */
	float buf[64];
	float phase = 0.0f;

	generate_sine(buf, 64, 1000.0f, &phase);
	process_buffer(buf, 64);
	CHECK(filter_z1 != 0.0f || filter_z2 != 0.0f);

	/* Recomputing (even to the same range) must zero z1/z2 -- stale
	 * memory acting on fresh coefficients would otherwise produce a
	 * spurious transient. Verify directly: zero input right after a
	 * recompute must produce exactly zero output.
	 */
	recompute_filter(&range);
	CHECK(filter_z1 == 0.0f);
	CHECK(filter_z2 == 0.0f);

	float zero_buf[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	float peak = process_buffer(zero_buf, 4);

	CHECK(peak == 0.0f);
	for (int i = 0; i < 4; i++) {
		CHECK(zero_buf[i] == 0.0f);
	}
}

static void test_gain_scales_output_linearly(void)
{
	struct audio_freq_range range = { .lower_hz = 900, .upper_hz = 1100 };

	recompute_filter(&range);
	on_volume_changed(100);
	float peak_full = settled_peak_at(1000.0f);

	recompute_filter(&range); /* reset filter memory between measurements */
	on_volume_changed(50);
	float peak_half = settled_peak_at(1000.0f);

	recompute_filter(&range);
	on_volume_changed(0);
	float peak_zero = settled_peak_at(1000.0f);

	CHECK(current_gain == 0.0f);
	CHECK(peak_zero == 0.0f);
	/* Gain is applied as a flat post-multiply -- ratio should be exact
	 * (within float rounding), not just "roughly half".
	 */
	CHECK_FLOAT_NEAR(peak_half / peak_full, 0.5f, 0.02f);
}

static void test_generate_test_tone_bounded_and_wraps(void)
{
	tone_phase = 0.0f;
	float buf[MOCK_BUFFER_LEN];

	for (int i = 0; i < 500; i++) {
		generate_test_tone(buf, MOCK_BUFFER_LEN);
		for (int j = 0; j < MOCK_BUFFER_LEN; j++) {
			CHECK(buf[j] >= -1.0001f && buf[j] <= 1.0001f);
		}
		/* Phase must stay wrapped into [0, 2*pi] -- an unbounded
		 * ever-growing phase would eventually lose sinf() precision.
		 */
		CHECK(tone_phase >= 0.0f && tone_phase <= 2.0f * (float)M_PI);
	}
}

int main(void)
{
	RUN(test_bandpass_passes_center_rejects_far_off);
	RUN(test_recompute_resets_filter_memory);
	RUN(test_gain_scales_output_linearly);
	RUN(test_generate_test_tone_bounded_and_wraps);
	return haven_test_summary("test_biquad_pipeline");
}
