/* Host test for protocol.c -- pure C, zero Zephyr dependency, compiled and
 * linked as-is (not reimplemented). Covers parsing, clamping (including the
 * safety-critical tone level_db clamp), and malformed/truncated input.
 */
#include "test_harness.h"

#include "../../src/protocol.c"

static void test_multi_filter_basic(void)
{
	struct dsp_command cmd;
	int rc = protocol_parse_line(
		"{\"type\":\"MULTI_FILTER\",\"bands\":[{\"f0\":4500,\"Q\":10.0}]}", &cmd);

	CHECK(rc == 0);
	CHECK(cmd.type == DSP_CMD_MULTI_FILTER);
	CHECK(cmd.band_count == 1);
	CHECK_FLOAT_NEAR(cmd.bands[0].f0_hz, 4500.0f, 0.01f);
	CHECK_FLOAT_NEAR(cmd.bands[0].q, 10.0f, 0.01f);
	/* atten_db omitted -> defaults to full notch */
	CHECK_FLOAT_NEAR(cmd.bands[0].atten_db, PROTOCOL_ATTEN_MAX_DB, 0.01f);
}

static void test_multi_filter_multi_band_and_atten(void)
{
	struct dsp_command cmd;
	int rc = protocol_parse_line(
		"{\"type\":\"MULTI_FILTER\",\"bands\":["
		"{\"f0\":1000,\"Q\":5,\"atten_db\":12},"
		"{\"f0\":2000,\"Q\":8}]}",
		&cmd);

	CHECK(rc == 0);
	CHECK(cmd.band_count == 2);
	CHECK_FLOAT_NEAR(cmd.bands[0].atten_db, 12.0f, 0.01f);
	CHECK_FLOAT_NEAR(cmd.bands[1].atten_db, PROTOCOL_ATTEN_MAX_DB, 0.01f);
}

static void test_multi_filter_f0_and_q_clamp(void)
{
	struct dsp_command cmd;

	/* Below range on both. */
	CHECK(protocol_parse_line(
		      "{\"type\":\"MULTI_FILTER\",\"bands\":[{\"f0\":10,\"Q\":0.1}]}", &cmd) == 0);
	CHECK_FLOAT_NEAR(cmd.bands[0].f0_hz, PROTOCOL_F0_MIN_HZ, 0.01f);
	CHECK_FLOAT_NEAR(cmd.bands[0].q, PROTOCOL_Q_MIN, 0.01f);

	/* Above range on both. */
	CHECK(protocol_parse_line(
		      "{\"type\":\"MULTI_FILTER\",\"bands\":[{\"f0\":99999,\"Q\":500}]}",
		      &cmd) == 0);
	CHECK_FLOAT_NEAR(cmd.bands[0].f0_hz, PROTOCOL_F0_MAX_HZ, 0.01f);
	CHECK_FLOAT_NEAR(cmd.bands[0].q, PROTOCOL_Q_MAX, 0.01f);

	/* atten_db above ATTEN_MAX_DB clamps down, negative clamps to 0. */
	CHECK(protocol_parse_line(
		      "{\"type\":\"MULTI_FILTER\",\"bands\":"
		      "[{\"f0\":1000,\"Q\":5,\"atten_db\":999}]}",
		      &cmd) == 0);
	CHECK_FLOAT_NEAR(cmd.bands[0].atten_db, PROTOCOL_ATTEN_MAX_DB, 0.01f);

	CHECK(protocol_parse_line(
		      "{\"type\":\"MULTI_FILTER\",\"bands\":"
		      "[{\"f0\":1000,\"Q\":5,\"atten_db\":-40}]}",
		      &cmd) == 0);
	CHECK_FLOAT_NEAR(cmd.bands[0].atten_db, 0.0f, 0.01f);
}

static void test_multi_filter_band_count_ceiling(void)
{
	struct dsp_command cmd;
	/* 6 bands in the input; PROTOCOL_MAX_BANDS is 5 -- parser stops
	 * accumulating rather than erroring (documented loop behavior, not
	 * an explicit "too many bands" rejection). Pin that behavior down
	 * explicitly so a future change to it is a deliberate decision, not
	 * an accident.
	 */
	int rc = protocol_parse_line(
		"{\"type\":\"MULTI_FILTER\",\"bands\":["
		"{\"f0\":100,\"Q\":1},{\"f0\":200,\"Q\":1},{\"f0\":300,\"Q\":1},"
		"{\"f0\":400,\"Q\":1},{\"f0\":500,\"Q\":1},{\"f0\":600,\"Q\":1}]}",
		&cmd);

	CHECK(rc == 0);
	CHECK(cmd.band_count == PROTOCOL_MAX_BANDS);
	CHECK_FLOAT_NEAR(cmd.bands[4].f0_hz, 500.0f, 0.01f);
}

static void test_multi_filter_malformed(void)
{
	struct dsp_command cmd;

	CHECK(protocol_parse_line("{\"type\":\"MULTI_FILTER\"}", &cmd) == -EINVAL);
	CHECK(protocol_parse_line("{\"type\":\"MULTI_FILTER\",\"bands\":[]}", &cmd) == -EINVAL);
	CHECK(protocol_parse_line(
		      "{\"type\":\"MULTI_FILTER\",\"bands\":[{\"Q\":5}]}", &cmd) == -EINVAL);
	CHECK(protocol_parse_line(
		      "{\"type\":\"MULTI_FILTER\",\"bands\":[{\"f0\":1000}]}", &cmd) == -EINVAL);
	/* Truncated mid-object: no closing brace for the band, no closing bracket. */
	CHECK(protocol_parse_line(
		      "{\"type\":\"MULTI_FILTER\",\"bands\":[{\"f0\":1000,\"Q\":5", &cmd) ==
	      -EINVAL);
	/* Non-numeric where a number is expected. */
	CHECK(protocol_parse_line(
		      "{\"type\":\"MULTI_FILTER\",\"bands\":[{\"f0\":\"abc\",\"Q\":5}]}",
		      &cmd) == -EINVAL);
}

static void test_bypass(void)
{
	struct dsp_command cmd;

	CHECK(protocol_parse_line("{\"type\":\"BYPASS\",\"enabled\":true}", &cmd) == 0);
	CHECK(cmd.type == DSP_CMD_BYPASS);
	CHECK(cmd.bypass_enabled == true);

	CHECK(protocol_parse_line("{\"type\":\"BYPASS\",\"enabled\":false}", &cmd) == 0);
	CHECK(cmd.bypass_enabled == false);

	CHECK(protocol_parse_line("{\"type\":\"BYPASS\",\"enabled\":maybe}", &cmd) == -EINVAL);
	CHECK(protocol_parse_line("{\"type\":\"BYPASS\"}", &cmd) == -EINVAL);
	/* Truncated: "enabled" key present but no value/colon reachable. */
	CHECK(protocol_parse_line("{\"type\":\"BYPASS\",\"enabled\"", &cmd) == -EINVAL);
}

static void test_tone_start_clamp_and_safety_ceiling(void)
{
	struct dsp_command cmd;

	CHECK(protocol_parse_line(
		      "{\"type\":\"TONE_START\",\"f0\":4000,\"level_db\":30}", &cmd) == 0);
	CHECK(cmd.type == DSP_CMD_TONE_START);
	CHECK_FLOAT_NEAR(cmd.tone_f0_hz, 4000.0f, 0.01f);
	CHECK_FLOAT_NEAR(cmd.tone_level_db, 30.0f, 0.01f);

	/* Safety-critical: a request far above the ceiling must clamp to
	 * EXACTLY PROTOCOL_TONE_LEVEL_MAX_DB (85.0), never anything higher,
	 * regardless of what the app already clamped it to -- this is the
	 * independent firmware-side ceiling the header comment describes.
	 */
	CHECK(protocol_parse_line(
		      "{\"type\":\"TONE_START\",\"f0\":4000,\"level_db\":250}", &cmd) == 0);
	CHECK_FLOAT_NEAR(cmd.tone_level_db, PROTOCOL_TONE_LEVEL_MAX_DB, 0.0001f);
	CHECK(cmd.tone_level_db <= PROTOCOL_TONE_LEVEL_MAX_DB);

	/* Exact boundary values pass through unchanged (not off-by-one clamped). */
	CHECK(protocol_parse_line(
		      "{\"type\":\"TONE_START\",\"f0\":4000,\"level_db\":85.0}", &cmd) == 0);
	CHECK_FLOAT_NEAR(cmd.tone_level_db, 85.0f, 0.0001f);

	CHECK(protocol_parse_line(
		      "{\"type\":\"TONE_START\",\"f0\":4000,\"level_db\":0.0}", &cmd) == 0);
	CHECK_FLOAT_NEAR(cmd.tone_level_db, 0.0f, 0.0001f);

	/* Negative level_db clamps up to the floor, not left negative. */
	CHECK(protocol_parse_line(
		      "{\"type\":\"TONE_START\",\"f0\":4000,\"level_db\":-999}", &cmd) == 0);
	CHECK_FLOAT_NEAR(cmd.tone_level_db, PROTOCOL_TONE_LEVEL_MIN_DB, 0.0001f);

	CHECK(protocol_parse_line("{\"type\":\"TONE_START\",\"f0\":4000}", &cmd) == -EINVAL);
	CHECK(protocol_parse_line("{\"type\":\"TONE_START\",\"level_db\":30}", &cmd) == -EINVAL);
}

static void test_tone_level(void)
{
	struct dsp_command cmd;

	CHECK(protocol_parse_line("{\"type\":\"TONE_LEVEL\",\"level_db\":42}", &cmd) == 0);
	CHECK(cmd.type == DSP_CMD_TONE_LEVEL);
	CHECK_FLOAT_NEAR(cmd.tone_level_db, 42.0f, 0.01f);

	CHECK(protocol_parse_line("{\"type\":\"TONE_LEVEL\",\"level_db\":9001}", &cmd) == 0);
	CHECK_FLOAT_NEAR(cmd.tone_level_db, PROTOCOL_TONE_LEVEL_MAX_DB, 0.0001f);

	CHECK(protocol_parse_line("{\"type\":\"TONE_LEVEL\"}", &cmd) == -EINVAL);
}

static void test_tone_stop_and_unknown(void)
{
	struct dsp_command cmd;

	CHECK(protocol_parse_line("{\"type\":\"TONE_STOP\"}", &cmd) == 0);
	CHECK(cmd.type == DSP_CMD_TONE_STOP);
	/* Body content after the type is irrelevant for TONE_STOP. */
	CHECK(protocol_parse_line("{\"type\":\"TONE_STOP\",\"garbage\":123}", &cmd) == 0);

	CHECK(protocol_parse_line("{\"type\":\"NOT_A_REAL_COMMAND\"}", &cmd) == -EINVAL);
	CHECK(protocol_parse_line("{}", &cmd) == -EINVAL);
	CHECK(protocol_parse_line("", &cmd) == -EINVAL);
	CHECK(protocol_parse_line("not even json", &cmd) == -EINVAL);
}

static void test_cmd_is_zeroed_on_every_call(void)
{
	/* protocol_parse_line memsets cmd first -- a failed parse must not
	 * leave stale data from a previous successful call visible.
	 */
	struct dsp_command cmd;

	CHECK(protocol_parse_line("{\"type\":\"BYPASS\",\"enabled\":true}", &cmd) == 0);
	CHECK(cmd.bypass_enabled == true);

	CHECK(protocol_parse_line("{\"type\":\"GARBAGE\"}", &cmd) == -EINVAL);
	CHECK(cmd.type == DSP_CMD_NONE);
	CHECK(cmd.bypass_enabled == false);
}

int main(void)
{
	RUN(test_multi_filter_basic);
	RUN(test_multi_filter_multi_band_and_atten);
	RUN(test_multi_filter_f0_and_q_clamp);
	RUN(test_multi_filter_band_count_ceiling);
	RUN(test_multi_filter_malformed);
	RUN(test_bypass);
	RUN(test_tone_start_clamp_and_safety_ceiling);
	RUN(test_tone_level);
	RUN(test_tone_stop_and_unknown);
	RUN(test_cmd_is_zeroed_on_every_call);
	return haven_test_summary("test_protocol");
}
