/* Haven nRF5340 firmware
 *
 * Pipeline: NUS write → line assembler → protocol_parse_line() → ADAU1860
 * driver → one JSON ack back over NUS TX. Parsing, DSP dispatch and the ack
 * run in the BLE receive path (Bluetooth RX thread); nothing here blocks.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if __has_include(<app_version.h>)
#include <app_version.h>
#endif
#ifndef APP_VERSION_STRING
#define APP_VERSION_STRING "dev"
#endif

#include "ack.h"
#include "adau1860_control.h"
#include "ble_transport.h"
#include "gatt_audio_service.h"
#include "mock_audio_pipeline.h"
#include "protocol.h"
#include "settings_store.h"
#ifdef CONFIG_HAVEN_CODEC_TAC5301
#include "tac5301_control.h"
#endif
#include "tone_gen.h"
#include "tone_safety.h"
#include "wake_button.h"
#include "battery_guard.h"
#include "pod_status.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* Best-effort: an ack the link can't take right now (-ENOMEM/-EAGAIN when
 * the TX buffer pool is exhausted, -ENOTCONN if the phone left between the
 * write and the reply) is dropped, not retried -- the app treats acks as
 * confirmation, not as state, and a retry loop in the BT RX path would be
 * worse than a missing ack.
 */
static void send_line(const char *buf, int len)
{
	if (len <= 0) {
		return;
	}
	int err = ble_transport_send(buf, (size_t)len);

	if (err) {
		LOG_DBG("ack dropped (err %d)", err);
	}
}

static int dispatch(const struct dsp_command *cmd)
{
	int err = 0;

	switch (cmd->type) {
	case DSP_CMD_MULTI_FILTER:
		LOG_INF("MULTI_FILTER: %u band(s)", cmd->band_count);
		err = adau1860_control_set_bypass(false);
		if (!err) {
			err = adau1860_control_apply_filters(cmd->bands, cmd->band_count);
		}
		break;
	case DSP_CMD_BYPASS:
		err = adau1860_control_set_bypass(cmd->bypass_enabled);
		break;
	case DSP_CMD_TONE_START:
		tone_safety_start(cmd->tone_f0_hz, cmd->tone_level_db);
		break;
	case DSP_CMD_TONE_LEVEL:
		tone_safety_set_level(cmd->tone_level_db);
		break;
	case DSP_CMD_TONE_STOP:
		tone_safety_stop();
		break;
	default:
		break;
	}
	return err;
}

static void handle_line(const char *line)
{
	struct dsp_command cmd;
	char ack[ACK_MAX_LEN];
	int err = protocol_parse_line(line, &cmd);

	if (err) {
		LOG_WRN("Rejected payload: %s", line);
		send_line(ack, ack_format_rejected(ack, sizeof(ack)));
		return;
	}

	err = dispatch(&cmd);
	send_line(ack, ack_format_command(&cmd, err, ack, sizeof(ack)));
}

/* Pod self-test / status (pod_status.h). codec_state is set once in main(): whether the audio
 * chip answered its init sequence. The report is built in the system work queue (it takes ADC
 * readings), sent on connect and every POD_STATUS_PERIOD_S while a phone is connected.
 */
static int codec_state = POD_UNKNOWN;
static atomic_t connected;

static void fill_status(struct pod_status *st)
{
	battery_guard_fill_status(st);
	st->codec = codec_state;
}

static void status_tick(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(status_work, status_tick);

static void status_tick(struct k_work *work)
{
	struct pod_status st;
	char ev[ACK_MAX_LEN];

	ARG_UNUSED(work);
	if (!atomic_get(&connected)) {
		return;
	}
	fill_status(&st);
	send_line(ev, ack_format_event_status(&st, ev, sizeof(ev)));
	k_work_reschedule(&status_work, K_SECONDS(POD_STATUS_PERIOD_S));
}

/* tone_safety.c auto-silenced a tone (no keep-alive). Tell the app, which
 * otherwise only learns its own ramp state machine has gone quiet.
 */
static void on_tone_watchdog(void)
{
	char ev[ACK_MAX_LEN];

	send_line(ev, ack_format_event_tone_watchdog(ev, sizeof(ev)));
}

/* The app subscribes to NUS TX right after connecting; the boot event is
 * how it learns which firmware (and which DAC source / FastDSP rate) it is
 * talking to, so a smoke-test build can never masquerade as the product.
 */
static void on_ble_connected(void)
{
	char ev[ACK_MAX_LEN];

	adau1860_control_on_ble_connected();
	send_line(ev, ack_format_event_boot(APP_VERSION_STRING,
					     (uint32_t)ADAU1860_FDSP_RATE_HZ,
					     IS_ENABLED(CONFIG_HAVEN_DAC_SOURCE_DMIC_DIRECT)
						     ? "dmic_direct"
						     : "fdsp",
					     ev, sizeof(ev)));
	atomic_set(&connected, 1);
	k_work_reschedule(&status_work, K_MSEC(500));	/* after the app has subscribed */
}

/* Losing the BLE link must silence any active tone immediately, the same
 * way haven-app's own useLdlTone does on its side (docs/safety.md) --
 * independently, not because the app told us to. A plain wrapper here
 * (rather than teaching adau1860_control.c about tone_safety.c) keeps that
 * module's only dependency on tone_safety.h one-directional.
 */
static void on_ble_disconnected(void)
{
	adau1860_control_on_ble_disconnected();
	tone_safety_stop();
	atomic_set(&connected, 0);
	(void)k_work_cancel_delayable(&status_work);
}

int main(void)
{
	LOG_INF("Haven firmware boot (%s)", APP_VERSION_STRING);

	int err;

#ifdef CONFIG_HAVEN_CODEC_TAC5301
	/* Bench build: everything else in this file (BLE/GATT, tone_gen's
	 * I2S0-master clock, mock pipeline) boots unchanged. tone_gen's I2S0
	 * clock has to run even though the TAC5301 ignores its data -- it's the
	 * BCLK/FSYNC the TAC5301's PLL locks to (TAC5301_BENCH_EXPERIMENT.md
	 * step 2). The ADAU1860 calls elsewhere (BLE command handlers,
	 * tone_safety.c) are still linked and would just fail with no device at
	 * 0x64 -- no BLE client is expected during this bench test.
	 */
	err = tac5301_control_init();
	if (err) {
		LOG_ERR("TAC5301 control init failed (err %d)", err);
	}
#else
	err = adau1860_control_init();
	if (err) {
		LOG_ERR("ADAU1860 control init failed (err %d)", err);
	}
#endif
	codec_state = err ? 0 : 1;

	/* nRF-side LDL tone generator (I2S0 master). Independent of the codec
	 * init above: on a DK without a codec it still streams, which is how
	 * the I2S clocking gets checked with a scope. */
	err = tone_gen_init();
	if (err) {
		LOG_WRN("Tone generator init failed (err %d) -- TONE_* will be no-ops", err);
	}

#ifdef CONFIG_HAVEN_CODEC_TAC5301
	/* The TAC5301's hear-through path only runs while BCLK / FSYNC are present, and tone_gen stops
	 * the I2S link whenever no tone is playing. For the bench build, stream silence from boot so the
	 * clocks never stop. Called directly (not through the TONE_* command path), so the codec's mixer
	 * stays on the mic-to-speaker loopback and the tone watchdog is not involved. A TONE_START /
	 * TONE_STOP over Bluetooth would end this keep-alive: do not connect the app during the bench test.
	 * Found missing on 2026-11-14, before any hardware run. */
	if (!err) {
		err = tone_gen_start(1000.0f, 0);
		if (err) {
			LOG_ERR("TAC5301 bench: could not start the clock keep-alive (err %d)", err);
		} else {
			LOG_INF("TAC5301 bench: I2S clocks running continuously (silent keep-alive)");
		}
		k_msleep(500); /* clock lock and mic-bias ramp */
		(void)tac5301_control_log_status();
	}
#endif

	tone_safety_set_watchdog_cb(on_tone_watchdog);
	ble_transport_set_conn_callbacks(on_ble_connected, on_ble_disconnected);

	err = ble_transport_init(handle_line);
	if (err) {
		LOG_ERR("BLE init failed (err %d)", err);
		return err;
	}

	/* BT_GATT_SERVICE_DEFINE registers the service automatically inside
	 * bt_enable() (called from ble_transport_init() above); this just
	 * confirms it and logs the bench-default parameter values.
	 */
	gatt_audio_service_init();
	mock_audio_pipeline_init();

	/* After mock_audio_pipeline_init() so a restored value's callback
	 * (registered by that call) actually fires -- otherwise the pipeline
	 * would start from its own hardcoded defaults instead of whatever
	 * was last saved.
	 */
	haven_settings_init();

	err = wake_button_init();
	if (err) {
		LOG_WRN("Wake button init failed (err %d) -- continuing without it", err);
	}

	err = battery_guard_init();
	if (err) {
		LOG_ERR("Battery guard init failed (err %d) -- NO low-battery shutdown", err);
	}

	{	/* one self-test line in the RTT log, for the programming / bring-up bench */
		struct pod_status st;

		fill_status(&st);
		LOG_INF("SELF TEST: battery %d mV (%d %%), usb %d, charging %d, charger fault %d, "
			"audio chip %d -> problem: %s (1 yes, 0 no, -1 cannot tell)",
			st.batt_mv, pod_status_batt_pct(st.batt_mv), st.usb, st.charging,
			st.chg_fault, st.codec, pod_status_problem(&st));
	}

	LOG_INF("Ready — waiting for app connection");
	return 0;
}
