/* Pod self-test / status rules. See pod_status.h. */
#include "pod_status.h"

#define LOW_MV 3400

/* Resting-voltage curve of a typical LiPo cell, (mV, %). Coarse on purpose: a 40 mAh pouch
 * cell's real curve is not measured, and the app shows this as a rough figure.
 */
static const int curve[][2] = {
	{ 3300, 0 }, { 3500, 5 }, { 3600, 10 }, { 3700, 25 }, { 3750, 40 },
	{ 3800, 50 }, { 3900, 65 }, { 4000, 78 }, { 4100, 90 }, { 4200, 100 },
};

int pod_status_batt_pct(int batt_mv)
{
	const int n = (int)(sizeof(curve) / sizeof(curve[0]));

	if (batt_mv <= 0) {
		return POD_UNKNOWN;
	}
	if (batt_mv <= curve[0][0]) {
		return 0;
	}
	if (batt_mv >= curve[n - 1][0]) {
		return 100;
	}
	for (int i = 1; i < n; i++) {
		if (batt_mv <= curve[i][0]) {
			int v0 = curve[i - 1][0], v1 = curve[i][0];
			int p0 = curve[i - 1][1], p1 = curve[i][1];

			return p0 + (p1 - p0) * (batt_mv - v0) / (v1 - v0);
		}
	}
	return 100;
}

const char *pod_status_problem(const struct pod_status *st)
{
	if (st->codec == 0) {
		return "codec";
	}
	if (st->chg_fault == 1) {
		return "charger_fault";
	}
	if (st->batt_mv <= 0 && st->usb != POD_UNKNOWN) {
		return "no_battery";	/* the board has the sensor but read nothing */
	}
	if (st->batt_mv > 0 && st->batt_mv < LOW_MV && st->usb != 1) {
		return "battery_low";
	}
	return "none";
}
