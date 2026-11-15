/* Pod self-test / status (2026-11-15).
 *
 * What a pod can tell about itself with no lab tools: battery voltage, whether a charger is on
 * the pins, whether the nPM1100 says it is charging or reports a charge fault, and whether the
 * audio chip answered at boot. main.c fills a struct pod_status and sends it to the app as
 * {"event":"status",...} (ack.c) on connect and every POD_STATUS_PERIOD_S while connected, and
 * logs it once at boot over RTT ("SELF TEST: ...").
 *
 * NOT checked here, on purpose: the microphone and the speaker. Neither can be judged from
 * the chip's registers alone; the bring-up plan's listening tests cover them.
 *
 * Pure C (no Zephyr), so the rules are host-tested (tests/host/test_pod_status.c).
 */
#ifndef HAVEN_POD_STATUS_H_
#define HAVEN_POD_STATUS_H_

#define POD_STATUS_PERIOD_S 30

/* Tri-state for things a board may not be able to report. */
#define POD_UNKNOWN (-1)

struct pod_status {
	int batt_mv;	/* battery (VSYS) in mV; 0 = no reading */
	int usb;	/* 1 = 5 V on the charge pins, 0 = not, POD_UNKNOWN */
	int charging;	/* nPM1100 CHG pin: 1 = charging, 0 = not, POD_UNKNOWN */
	int chg_fault;	/* nPM1100 ERR pin: 1 = fault, 0 = none, POD_UNKNOWN */
	int codec;	/* 1 = audio chip answered at boot, 0 = it did not, POD_UNKNOWN */
};

/* Rough charge left, 0-100 %, from the resting voltage of a single Li-ion/LiPo cell
 * (piecewise-linear table, 3.30 V = 0 %, 4.20 V = 100 %). Returns POD_UNKNOWN for no reading.
 * Under load or while charging the figure reads high or low by several percent.
 */
int pod_status_batt_pct(int batt_mv);

/* The most important thing wrong, as a short code for the app, or "none":
 *   "codec"          the audio chip did not answer (most serious: no sound at all)
 *   "charger_fault"  the nPM1100 reports a charge fault (battery or temperature)
 *   "battery_low"    under 3.40 V and not on the charger
 *   "no_battery"     no battery reading at all on a board that has the sensor
 */
const char *pod_status_problem(const struct pod_status *st);

#endif /* HAVEN_POD_STATUS_H_ */
