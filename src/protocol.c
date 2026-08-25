/* Minimal, allocation-free parser for the fixed Haven JSON schema.
 *
 * Deliberately not a general JSON parser: the app is the only producer and
 * emits a known shape, so we scan for the keys we care about and clamp every
 * numeric field. Malformed input can only ever produce -EINVAL, never a
 * partially-applied command.
 */
#include "protocol.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static float clampf(float v, float lo, float hi)
{
	if (v < lo) {
		return lo;
	}
	if (v > hi) {
		return hi;
	}
	return v;
}

/* Find `"key"` after `from` and parse the number following its ':'.
 * Returns pointer just past the parsed number, or NULL if not found before
 * `end`.
 */
static const char *parse_number_field(const char *from, const char *end,
				      const char *key, float *out)
{
	const char *p = strstr(from, key);

	if (p == NULL || (end != NULL && p >= end)) {
		return NULL;
	}
	p = strchr(p + strlen(key), ':');
	if (p == NULL) {
		return NULL;
	}

	char *num_end;
	float v = strtof(p + 1, &num_end);

	if (num_end == p + 1) {
		return NULL;
	}
	*out = v;
	return num_end;
}

static int parse_multi_filter(const char *line, struct dsp_command *cmd)
{
	const char *bands = strstr(line, "\"bands\"");

	if (bands == NULL) {
		return -EINVAL;
	}

	const char *p = strchr(bands, '[');
	const char *arr_end = p != NULL ? strchr(p, ']') : NULL;

	if (p == NULL || arr_end == NULL) {
		return -EINVAL;
	}

	cmd->band_count = 0;
	while (cmd->band_count < PROTOCOL_MAX_BANDS) {
		const char *obj = strchr(p, '{');

		if (obj == NULL || obj > arr_end) {
			break;
		}
		const char *obj_end = strchr(obj, '}');

		if (obj_end == NULL || obj_end > arr_end) {
			return -EINVAL;
		}

		struct filter_band *b = &cmd->bands[cmd->band_count];
		float f0, q;

		if (parse_number_field(obj, obj_end, "\"f0\"", &f0) == NULL ||
		    parse_number_field(obj, obj_end, "\"Q\"", &q) == NULL) {
			return -EINVAL;
		}
		b->f0_hz = clampf(f0, PROTOCOL_F0_MIN_HZ, PROTOCOL_F0_MAX_HZ);
		b->q = clampf(q, PROTOCOL_Q_MIN, PROTOCOL_Q_MAX);

		/* Optional dampening depth; default = full notch. */
		float atten = PROTOCOL_ATTEN_MAX_DB;

		parse_number_field(obj, obj_end, "\"atten_db\"", &atten);
		b->atten_db = clampf(atten, 0.0f, PROTOCOL_ATTEN_MAX_DB);

		cmd->band_count++;
		p = obj_end + 1;
	}

	if (cmd->band_count == 0) {
		return -EINVAL;
	}
	cmd->type = DSP_CMD_MULTI_FILTER;
	return 0;
}

static int parse_bypass(const char *line, struct dsp_command *cmd)
{
	const char *p = strstr(line, "\"enabled\"");

	if (p == NULL) {
		return -EINVAL;
	}
	p = strchr(p, ':');
	if (p == NULL) {
		return -EINVAL;
	}
	while (*++p == ' ') {
	}

	if (strncmp(p, "true", 4) == 0) {
		cmd->bypass_enabled = true;
	} else if (strncmp(p, "false", 5) == 0) {
		cmd->bypass_enabled = false;
	} else {
		return -EINVAL;
	}
	cmd->type = DSP_CMD_BYPASS;
	return 0;
}

/* level_db is safety-critical (drives the LDL calibration tone) -- clamped
 * independently of whatever the app already clamped it to. See protocol.h.
 */
static int parse_tone_start(const char *line, struct dsp_command *cmd)
{
	float f0, level;

	if (parse_number_field(line, NULL, "\"f0\"", &f0) == NULL ||
	    parse_number_field(line, NULL, "\"level_db\"", &level) == NULL) {
		return -EINVAL;
	}
	cmd->tone_f0_hz = clampf(f0, PROTOCOL_F0_MIN_HZ, PROTOCOL_F0_MAX_HZ);
	cmd->tone_level_db = clampf(level, PROTOCOL_TONE_LEVEL_MIN_DB,
				   PROTOCOL_TONE_LEVEL_MAX_DB);
	cmd->type = DSP_CMD_TONE_START;
	return 0;
}

static int parse_tone_level(const char *line, struct dsp_command *cmd)
{
	float level;

	if (parse_number_field(line, NULL, "\"level_db\"", &level) == NULL) {
		return -EINVAL;
	}
	cmd->tone_level_db = clampf(level, PROTOCOL_TONE_LEVEL_MIN_DB,
				   PROTOCOL_TONE_LEVEL_MAX_DB);
	cmd->type = DSP_CMD_TONE_LEVEL;
	return 0;
}

int protocol_parse_line(const char *line, struct dsp_command *cmd)
{
	memset(cmd, 0, sizeof(*cmd));

	const char *type = strstr(line, "\"type\"");

	if (type == NULL) {
		return -EINVAL;
	}

	if (strstr(line, "\"MULTI_FILTER\"") != NULL) {
		return parse_multi_filter(line, cmd);
	}
	if (strstr(line, "\"BYPASS\"") != NULL) {
		return parse_bypass(line, cmd);
	}
	if (strstr(line, "\"TONE_START\"") != NULL) {
		return parse_tone_start(line, cmd);
	}
	if (strstr(line, "\"TONE_LEVEL\"") != NULL) {
		return parse_tone_level(line, cmd);
	}
	if (strstr(line, "\"TONE_STOP\"") != NULL) {
		cmd->type = DSP_CMD_TONE_STOP;
		return 0;
	}
	return -EINVAL;
}
