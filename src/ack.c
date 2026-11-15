#include "ack.h"

#include <errno.h>
#include <stdio.h>

/* snprintf into buf; -ENOSPC if the message (plus NUL) did not fit. Every
 * caller passes a buffer of ACK_MAX_LEN, so this only trips on a schema
 * change that grows a message past the documented bound.
 */
#define FORMAT(buf, size, ...)                                              \
	({                                                                  \
		int n_ = snprintf((buf), (size), __VA_ARGS__);              \
		(n_ < 0 || (size_t)n_ >= (size)) ? -ENOSPC : n_;            \
	})

static long round_to_long(float v)
{
	return (long)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

static const char *command_name(enum dsp_command_type type)
{
	switch (type) {
	case DSP_CMD_MULTI_FILTER:
		return "MULTI_FILTER";
	case DSP_CMD_BYPASS:
		return "BYPASS";
	case DSP_CMD_TONE_START:
		return "TONE_START";
	case DSP_CMD_TONE_LEVEL:
		return "TONE_LEVEL";
	case DSP_CMD_TONE_STOP:
		return "TONE_STOP";
	default:
		return "?";
	}
}

int ack_format_command(const struct dsp_command *cmd, int dispatch_err, char *buf,
		       size_t size)
{
	const char *name = command_name(cmd->type);

	if (dispatch_err) {
		return FORMAT(buf, size, "{\"ack\":\"%s\",\"ok\":false,\"err\":\"dsp\",\"code\":%d}\n",
			      name, dispatch_err);
	}

	switch (cmd->type) {
	case DSP_CMD_MULTI_FILTER:
		return FORMAT(buf, size, "{\"ack\":\"%s\",\"ok\":true,\"bands\":%u}\n", name,
			      (unsigned int)cmd->band_count);
	case DSP_CMD_BYPASS:
		return FORMAT(buf, size, "{\"ack\":\"%s\",\"ok\":true,\"enabled\":%s}\n", name,
			      cmd->bypass_enabled ? "true" : "false");
	case DSP_CMD_TONE_START:
		return FORMAT(buf, size,
			      "{\"ack\":\"%s\",\"ok\":true,\"f0\":%ld,\"level_db\":%ld}\n", name,
			      round_to_long(cmd->tone_f0_hz), round_to_long(cmd->tone_level_db));
	case DSP_CMD_TONE_LEVEL:
		return FORMAT(buf, size, "{\"ack\":\"%s\",\"ok\":true,\"level_db\":%ld}\n", name,
			      round_to_long(cmd->tone_level_db));
	case DSP_CMD_TONE_STOP:
		return FORMAT(buf, size, "{\"ack\":\"%s\",\"ok\":true}\n", name);
	default:
		return FORMAT(buf, size, "{\"ack\":\"?\",\"ok\":false,\"err\":\"unknown\"}\n");
	}
}

int ack_format_rejected(char *buf, size_t size)
{
	return FORMAT(buf, size, "{\"ack\":\"?\",\"ok\":false,\"err\":\"parse\"}\n");
}

int ack_format_event_tone_watchdog(char *buf, size_t size)
{
	return FORMAT(buf, size, "{\"event\":\"tone_watchdog\"}\n");
}

int ack_format_event_boot(const char *fw_version, uint32_t fdsp_rate_hz,
			  const char *dac_source, char *buf, size_t size)
{
	return FORMAT(buf, size,
		      "{\"event\":\"boot\",\"fw\":\"%s\",\"fdsp_rate\":%lu,\"dac_source\":\"%s\"}\n",
		      fw_version, (unsigned long)fdsp_rate_hz, dac_source);
}

static const char *tri(int v)
{
	return v == POD_UNKNOWN ? "null" : (v ? "true" : "false");
}

int ack_format_event_status(const struct pod_status *st, char *buf, size_t size)
{
	int mv = st->batt_mv < 0 ? 0 : (st->batt_mv > 9999 ? 9999 : st->batt_mv);
	int pct = pod_status_batt_pct(mv);
	char pct_s[8];

	if (pct == POD_UNKNOWN) {
		snprintf(pct_s, sizeof(pct_s), "null");
	} else {
		snprintf(pct_s, sizeof(pct_s), "%d", pct);
	}
	return FORMAT(buf, size,
		      "{\"event\":\"status\",\"mv\":%d,\"pct\":%s,\"usb\":%s,\"chg\":%s,\"fault\":%s,"
		      "\"codec\":%s,\"problem\":\"%s\"}\n",
		      mv, pct_s, tri(st->usb), tri(st->charging), tri(st->chg_fault), tri(st->codec),
		      pod_status_problem(st));
}
