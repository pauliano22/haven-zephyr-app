#include "ack.h"

#include <stdio.h>

#include "ble_transport.h"

static const char *cmd_type_name(enum dsp_command_type type)
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
		return "UNKNOWN";
	}
}

void ack_send(enum dsp_command_type type)
{
	char msg[48];
	int len = snprintf(msg, sizeof(msg), "{\"type\":\"ACK\",\"cmd\":\"%s\"}\n",
			    cmd_type_name(type));

	/* A future cmd_type_name() entry longer than fits would truncate/
	 * misformat rather than send garbage -- snprintf's own return value
	 * (would-have-written length) makes that case detectable here rather
	 * than sending a corrupt ack.
	 */
	if (len > 0 && len < (int)sizeof(msg)) {
		ble_transport_send(msg, (size_t)len);
	}
}

void ack_send_error(void)
{
	static const char msg[] = "{\"type\":\"ERROR\"}\n";

	ble_transport_send(msg, sizeof(msg) - 1);
}
