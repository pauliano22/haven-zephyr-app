/* Host test for ack.c -- the real production file, compiled unmodified.
 * ble_transport_send() is faked here (recording what was sent) rather than
 * linking the real ble_transport.c, which pulls in the actual Bluetooth/NUS
 * stack -- same isolation approach as test_settings_dispatch.c fakes
 * gatt_audio_service.c's setters.
 */
#include "test_harness.h"

#include <string.h>

#include "../../src/protocol.h"

static char last_sent[64];
static size_t last_sent_len;
static int send_calls;

int ble_transport_send(const char *data, size_t len)
{
	send_calls++;
	last_sent_len = len < sizeof(last_sent) - 1 ? len : sizeof(last_sent) - 1;
	memcpy(last_sent, data, last_sent_len);
	last_sent[last_sent_len] = '\0';
	return 0;
}

#include "../../src/ack.c"

static void reset(void)
{
	send_calls = 0;
	last_sent[0] = '\0';
	last_sent_len = 0;
}

static void test_ack_multi_filter(void)
{
	reset();
	ack_send(DSP_CMD_MULTI_FILTER);
	CHECK(send_calls == 1);
	CHECK(strcmp(last_sent, "{\"type\":\"ACK\",\"cmd\":\"MULTI_FILTER\"}\n") == 0);
	/* The wire format is newline-terminated JSON, same convention as every
	 * app -> device message (docs/ble-protocol.md) -- the app's line
	 * assembler on the other end depends on this, not just cosmetics.
	 */
	CHECK(last_sent[last_sent_len - 1] == '\n');
}

static void test_ack_every_command_type_has_a_real_name(void)
{
	const enum dsp_command_type types[] = {
		DSP_CMD_MULTI_FILTER, DSP_CMD_BYPASS, DSP_CMD_TONE_START,
		DSP_CMD_TONE_LEVEL,   DSP_CMD_TONE_STOP,
	};

	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		reset();
		ack_send(types[i]);
		CHECK(send_calls == 1);
		CHECK(strstr(last_sent, "\"cmd\":\"UNKNOWN\"") == NULL);
	}
}

static void test_ack_unknown_type_does_not_crash_or_send_garbage(void)
{
	reset();
	ack_send(DSP_CMD_NONE);
	CHECK(send_calls == 1);
	CHECK(strcmp(last_sent, "{\"type\":\"ACK\",\"cmd\":\"UNKNOWN\"}\n") == 0);
}

static void test_ack_send_error(void)
{
	reset();
	ack_send_error();
	CHECK(send_calls == 1);
	CHECK(strcmp(last_sent, "{\"type\":\"ERROR\"}\n") == 0);
}

int main(void)
{
	RUN(test_ack_multi_filter);
	RUN(test_ack_every_command_type_has_a_real_name);
	RUN(test_ack_unknown_type_does_not_crash_or_send_garbage);
	RUN(test_ack_send_error);
	return haven_test_summary("test_ack");
}
