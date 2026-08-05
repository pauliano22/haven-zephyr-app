/* JSON control protocol shared with the AcousticShield mobile app.
 *
 * Messages arrive over NUS as newline-terminated JSON:
 *   {"type":"MULTI_FILTER","bands":[{"f0":4500,"Q":10.0}, ...]}   (1..5 bands)
 *   {"type":"BYPASS","enabled":true}
 *
 * Bands may optionally carry "atten_db" (positive dB of reduction) once the
 * app's dynamic-dampening feature ships; absent means full notch.
 */
#ifndef ACOUSTICSHIELD_PROTOCOL_H_
#define ACOUSTICSHIELD_PROTOCOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROTOCOL_MAX_BANDS 5

/* Guardrails on incoming parameters — anything outside is clamped. */
#define PROTOCOL_F0_MIN_HZ    200.0f
#define PROTOCOL_F0_MAX_HZ   8000.0f
#define PROTOCOL_Q_MIN          1.0f
#define PROTOCOL_Q_MAX         20.0f
#define PROTOCOL_ATTEN_MAX_DB  40.0f

struct filter_band {
	float f0_hz;
	float q;
	/* dB of reduction at f0; PROTOCOL_ATTEN_MAX_DB == treat as full notch */
	float atten_db;
};

enum dsp_command_type {
	DSP_CMD_NONE = 0,
	DSP_CMD_MULTI_FILTER,
	DSP_CMD_BYPASS,
};

struct dsp_command {
	enum dsp_command_type type;
	uint8_t band_count;
	struct filter_band bands[PROTOCOL_MAX_BANDS];
	bool bypass_enabled;
};

/* Parse one newline-stripped JSON line into cmd.
 * Returns 0 on success, -EINVAL on malformed/unknown input.
 */
int protocol_parse_line(const char *line, struct dsp_command *cmd);

#endif /* ACOUSTICSHIELD_PROTOCOL_H_ */
