/* JSON control protocol shared with the Haven mobile app.
 *
 * Messages arrive over NUS as newline-terminated JSON:
 *   {"type":"MULTI_FILTER","bands":[{"f0":4500,"Q":10.0}, ...]}   (1..5 bands)
 *   {"type":"BYPASS","enabled":true}
 *   {"type":"TONE_START","f0":4000,"level_db":30}
 *   {"type":"TONE_LEVEL","level_db":42}
 *   {"type":"TONE_STOP"}
 *
 * Bands may optionally carry "atten_db" (positive dB of reduction) once the
 * app's dynamic-dampening feature ships; absent means full notch.
 *
 * TONE_* drives the LDL (Loudness Discomfort Level) calibration test —
 * safety-critical, see haven-app's docs/safety.md. level_db is clamped here
 * to PROTOCOL_TONE_LEVEL_MAX_DB independently of the app's own 85 dB cap
 * (src/constants/safety.ts) — this is a defense-in-depth *pair*, not a
 * shared value, so the two are intentionally not derived from one another.
 * The auto-stop watchdog (a frozen app or hostile peer must not be able to
 * hold a loud tone) lives in tone_safety.c, one layer past parsing/clamping.
 */
#ifndef HAVEN_PROTOCOL_H_
#define HAVEN_PROTOCOL_H_

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

/* Absolute ceiling for any commanded tone level. Nothing may exceed this,
 * regardless of what the app requests. See the file header re: this being
 * an independent value from the app's own MAX_TONE_LEVEL_DB, on purpose.
 */
#define PROTOCOL_TONE_LEVEL_MAX_DB  85.0f
#define PROTOCOL_TONE_LEVEL_MIN_DB   0.0f

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
	DSP_CMD_TONE_START,
	DSP_CMD_TONE_LEVEL,
	DSP_CMD_TONE_STOP,
};

struct dsp_command {
	enum dsp_command_type type;
	uint8_t band_count;
	struct filter_band bands[PROTOCOL_MAX_BANDS];
	bool bypass_enabled;
	/* Valid for DSP_CMD_TONE_START only. */
	float tone_f0_hz;
	/* Valid for DSP_CMD_TONE_START and DSP_CMD_TONE_LEVEL. */
	float tone_level_db;
};

/* Parse one newline-stripped JSON line into cmd.
 * Returns 0 on success, -EINVAL on malformed/unknown input.
 */
int protocol_parse_line(const char *line, struct dsp_command *cmd);

#endif /* HAVEN_PROTOCOL_H_ */
