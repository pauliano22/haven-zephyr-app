/* Haven nRF52 firmware
 *
 * Pipeline: NUS write → line assembler → protocol_parse_line() → ADAU1860
 * driver. Parsing and DSP dispatch run on the system workqueue via the BLE
 * receive path; nothing here blocks.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "adau1860.h"
#include "ble_transport.h"
#include "protocol.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static void handle_line(const char *line)
{
	struct dsp_command cmd;
	int err = protocol_parse_line(line, &cmd);

	if (err) {
		LOG_WRN("Rejected payload: %s", line);
		return;
	}

	switch (cmd.type) {
	case DSP_CMD_MULTI_FILTER:
		LOG_INF("MULTI_FILTER: %u band(s)", cmd.band_count);
		adau1860_set_bypass(false);
		adau1860_apply_filters(cmd.bands, cmd.band_count);
		break;
	case DSP_CMD_BYPASS:
		adau1860_set_bypass(cmd.bypass_enabled);
		break;
	default:
		break;
	}
}

int main(void)
{
	LOG_INF("Haven firmware boot");

	int err = adau1860_init();

	if (err) {
		LOG_ERR("ADAU1860 init failed (err %d)", err);
	}

	err = ble_transport_init(handle_line);
	if (err) {
		LOG_ERR("BLE init failed (err %d)", err);
		return err;
	}

	LOG_INF("Ready — waiting for app connection");
	return 0;
}
