/* Host test for adau1860_control.c's calc_band_coeffs() (RBJ notch /
 * variable-depth peaking-cut biquad).
 *
 * UNLIKE the other test files here, this one does NOT #include the real
 * adau1860_control.c: that file has a file-scope devicetree macro
 * (`I2C_DT_SPEC_GET(DT_NODELABEL(adau1860))`) that expands against
 * generated devicetree headers from a real Zephyr build -- faithfully
 * faking that would mean reimplementing a slice of Zephyr's devicetree
 * macro machinery, which felt like more risk of a *subtly wrong* fake than
 * value. Instead, calc_band_coeffs()'s formula is copied verbatim below.
 * If adau1860_control.c's formula ever changes, this copy must be updated
 * to match by hand -- it will NOT catch a drift automatically. This is a
 * real, disclosed limitation, not a hidden one.
 *
 * To compensate, these tests check FUNCTIONAL/structural properties of the
 * RBJ formula itself (does the resulting biquad actually notch/pass the
 * right frequencies; do known-good algebraic identities of the formula
 * hold) rather than hardcoded expected coefficient constants, so a
 * transcription mistake in the copy below is likely to show up as a
 * failing test rather than silently validating itself.
 */
#include "test_harness.h"

#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define ADAU1860_SAMPLE_RATE_HZ 48000.0f
#define PROTOCOL_ATTEN_MAX_DB 40.0f

struct filter_band {
	float f0_hz;
	float q;
	float atten_db;
};

struct adau1860_biquad {
	float b0, b1, b2, a1, a2;
};

/* ---- verbatim copy of adau1860_control.c's calc_band_coeffs() ---- */
static void calc_band_coeffs(const struct filter_band *band, struct adau1860_biquad *c)
{
	float w0 = 2.0f * (float)M_PI * (band->f0_hz / ADAU1860_SAMPLE_RATE_HZ);
	float alpha = sinf(w0) / (2.0f * band->q);
	float cosw0 = cosf(w0);

	if (band->atten_db >= PROTOCOL_ATTEN_MAX_DB) {
		float a0 = 1.0f + alpha;

		c->b0 = 1.0f / a0;
		c->b1 = -2.0f * cosw0 / a0;
		c->b2 = 1.0f / a0;
		c->a1 = -2.0f * cosw0 / a0;
		c->a2 = (1.0f - alpha) / a0;
	} else {
		float A = powf(10.0f, -band->atten_db / 40.0f);
		float a0 = 1.0f + alpha / A;

		c->b0 = (1.0f + alpha * A) / a0;
		c->b1 = -2.0f * cosw0 / a0;
		c->b2 = (1.0f - alpha * A) / a0;
		c->a1 = -2.0f * cosw0 / a0;
		c->a2 = (1.0f - alpha / A) / a0;
	}
}
/* ---- end verbatim copy ---- */

/* Direct Form II Transposed apply, mirroring mock_audio_pipeline.c's
 * process_buffer recurrence, for functional response testing.
 */
static float apply_biquad_peak(const struct adau1860_biquad *c, float freq_hz,
				float sample_rate_hz, int settle_buffers, int buf_len)
{
	float z1 = 0.0f, z2 = 0.0f, phase = 0.0f;
	float w = 2.0f * (float)M_PI * (freq_hz / sample_rate_hz);
	float peak = 0.0f;

	for (int b = 0; b < settle_buffers; b++) {
		peak = 0.0f;
		for (int i = 0; i < buf_len; i++) {
			float x = sinf(phase);

			phase += w;
			float y = c->b0 * x + z1;

			z1 = c->b1 * x - c->a1 * y + z2;
			z2 = c->b2 * x - c->a2 * y;
			float mag = fabsf(y);

			if (mag > peak) {
				peak = mag;
			}
		}
	}
	return peak;
}

static void test_notch_structural_identity(void)
{
	/* Classic RBJ notch: b1 and a1 share the exact same expression
	 * (-2*cos(w0)/a0) -- verify this identity holds, which it will only
	 * if the notch branch was copied/executed correctly.
	 */
	struct filter_band band = { .f0_hz = 4000.0f, .q = 5.0f, .atten_db = PROTOCOL_ATTEN_MAX_DB };
	struct adau1860_biquad c;

	calc_band_coeffs(&band, &c);
	CHECK_FLOAT_NEAR(c.b1, c.a1, 0.0001f);
	CHECK_FLOAT_NEAR(c.b0, c.b2, 0.0001f);
}

static void test_zero_cut_peaking_is_identity_filter(void)
{
	/* atten_db=0 still takes the peaking branch (condition is >=40 for
	 * notch), with A = 10^(0/-40) = 1 -- algebraically this degenerates
	 * the peaking formula to b0=1, b1=a1, b2=a2, i.e. an exact all-pass/
	 * identity filter (y[n] == x[n] for all n from a zero initial state).
	 * This is a strong, precise check that the peaking branch's algebra
	 * was copied correctly, not just "roughly resembles a filter".
	 */
	struct filter_band band = { .f0_hz = 3000.0f, .q = 4.0f, .atten_db = 0.0f };
	struct adau1860_biquad c;

	calc_band_coeffs(&band, &c);
	CHECK_FLOAT_NEAR(c.b0, 1.0f, 0.001f);
	CHECK_FLOAT_NEAR(c.b1, c.a1, 0.0001f);
	CHECK_FLOAT_NEAR(c.b2, c.a2, 0.0001f);

	float peak_at_f0 = apply_biquad_peak(&c, 3000.0f, ADAU1860_SAMPLE_RATE_HZ, 20, 64);

	CHECK(peak_at_f0 > 0.95f && peak_at_f0 < 1.05f);
}

static void test_notch_rejects_f0_passes_far_off(void)
{
	struct filter_band band = { .f0_hz = 4000.0f, .q = 8.0f, .atten_db = PROTOCOL_ATTEN_MAX_DB };
	struct adau1860_biquad c;

	calc_band_coeffs(&band, &c);

	float peak_at_notch = apply_biquad_peak(&c, 4000.0f, ADAU1860_SAMPLE_RATE_HZ, 30, 128);
	float peak_far_off = apply_biquad_peak(&c, 1000.0f, ADAU1860_SAMPLE_RATE_HZ, 30, 128);

	/* A working notch should reject its target frequency hard... */
	CHECK(peak_at_notch < 0.1f);
	/* ...and pass everything else close to unity. */
	CHECK(peak_far_off > 0.85f);
}

static void test_partial_peaking_cut_attenuates_less_than_full_notch(void)
{
	struct filter_band notch_band = { .f0_hz = 4000.0f, .q = 8.0f, .atten_db = 40.0f };
	struct filter_band cut12_band = { .f0_hz = 4000.0f, .q = 8.0f, .atten_db = 12.0f };
	struct adau1860_biquad c_notch, c_cut12;

	calc_band_coeffs(&notch_band, &c_notch);
	calc_band_coeffs(&cut12_band, &c_cut12);

	float peak_notch = apply_biquad_peak(&c_notch, 4000.0f, ADAU1860_SAMPLE_RATE_HZ, 30, 128);
	float peak_cut12 = apply_biquad_peak(&c_cut12, 4000.0f, ADAU1860_SAMPLE_RATE_HZ, 30, 128);

	/* A 12 dB cut should attenuate less than a full (~infinite-depth
	 * ideal) notch -- i.e. let more signal through at f0.
	 */
	CHECK(peak_cut12 > peak_notch);
	/* 12 dB down is a factor of 10^(-12/20) =~ 0.251 in amplitude. */
	CHECK_FLOAT_NEAR(peak_cut12, 0.251f, 0.05f);
}

int main(void)
{
	RUN(test_notch_structural_identity);
	RUN(test_zero_cut_peaking_is_identity_filter);
	RUN(test_notch_rejects_f0_passes_far_off);
	RUN(test_partial_peaking_cut_attenuates_less_than_full_notch);
	return haven_test_summary("test_adau1860_coeffs");
}
