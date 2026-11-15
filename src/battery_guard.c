/* Low-battery shutdown (design review 2026-11, finding 4).
 *
 * The nPM1100 does not stop a lithium cell from over-discharging: its buck only stops near
 * 2.8 V and the board keeps drawing after that. This module reads the battery through the
 * VSYS divider every CONFIG_HAVEN_BATTERY_CHECK_PERIOD_S seconds and, after several low
 * readings in a row with no charger attached, drives SHPACT high. The nPM1100 then enters
 * ship mode: it disconnects the battery (the board loses power, so this never returns) and
 * stays off until the pod is put on its charger.
 *
 * NOT TESTED ON HARDWARE. Thresholds are datasheet-based guesses:
 *  - cutoff 3.30 V by default. The reading is VSYS under load, slightly below the cell voltage.
 *  - the v3 pouch cell has its own protection circuit; that is a backstop, not the plan.
 * Divider ratios come from the pod schematic: VSYS_SENSE = VSYS * 560k / (1M + 560k),
 * VBUS_DET = VBUS * 47k / (100k + 47k).
 */
#include "battery_guard.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(battery_guard, LOG_LEVEL_INF);

int battery_guard_step(int low_count, int vsys_mv, int vbus_mv, int cutoff_mv, int vbus_present_mv)
{
	if (vbus_mv >= vbus_present_mv) {
		return 0;	/* on the charger: VSYS is fed from VBUS, never shut down */
	}
	if (vsys_mv <= 0) {
		return 0;	/* failed reading: never shut down on bad data */
	}
	return (vsys_mv < cutoff_mv) ? low_count + 1 : 0;
}

#if defined(CONFIG_HAVEN_BATTERY_GUARD)

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>

#define USER_NODE DT_PATH(zephyr_user)

static const struct adc_dt_spec vsys_ch = ADC_DT_SPEC_GET_BY_NAME(USER_NODE, vsys_sense);
static const struct adc_dt_spec vbus_ch = ADC_DT_SPEC_GET_BY_NAME(USER_NODE, vbus_det);
static const struct gpio_dt_spec ship = GPIO_DT_SPEC_GET(USER_NODE, ship_mode_gpios);
/* nPM1100 CHG and ERR open-drain outputs, active low (pod self-test, 2026-11-15). */
static const struct gpio_dt_spec chg = GPIO_DT_SPEC_GET(USER_NODE, charger_chg_gpios);
static const struct gpio_dt_spec chg_err = GPIO_DT_SPEC_GET(USER_NODE, charger_err_gpios);

#define VSYS_NUM 1560	/* (1M + 560k) / 560k */
#define VSYS_DEN 560
#define VBUS_NUM 147	/* (100k + 47k) / 47k */
#define VBUS_DEN 47
#define VBUS_PRESENT_MV 3000
#define LOW_READINGS_NEEDED 3

static int last_mv;
static int last_vbus_mv = -1;
static bool chg_pins_ok;
static int low_count;

static int read_mv(const struct adc_dt_spec *ch, int num, int den)
{
	int16_t raw = 0;
	struct adc_sequence seq = { .buffer = &raw, .buffer_size = sizeof(raw) };
	int32_t mv;
	int err;

	adc_sequence_init_dt(ch, &seq);
	err = adc_read_dt(ch, &seq);
	if (err) {
		return err;
	}
	mv = raw < 0 ? 0 : raw;
	err = adc_raw_to_millivolts_dt(ch, &mv);
	if (err < 0) {
		return err;
	}
	return (int)((int64_t)mv * num / den);
}

static void check(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(check_work, check);

static void check(struct k_work *work)
{
	int vsys = read_mv(&vsys_ch, VSYS_NUM, VSYS_DEN);
	int vbus = read_mv(&vbus_ch, VBUS_NUM, VBUS_DEN);

	ARG_UNUSED(work);
	if (vsys > 0) {
		last_mv = vsys;
	}
	last_vbus_mv = vbus;
	low_count = battery_guard_step(low_count, vsys, vbus < 0 ? 0 : vbus,
				       CONFIG_HAVEN_BATTERY_CUTOFF_MV, VBUS_PRESENT_MV);
	if (low_count >= LOW_READINGS_NEEDED) {
		LOG_WRN("Battery %d mV below %d mV: entering ship mode", vsys, CONFIG_HAVEN_BATTERY_CUTOFF_MV);
		k_msleep(50);			/* let the log line out */
		gpio_pin_set_dt(&ship, 1);	/* nPM1100 needs SHPACT high for a while; hold it */
		k_msleep(1000);
		/* Still running: ship mode did not take (for example a charger was just attached). */
		gpio_pin_set_dt(&ship, 0);
		low_count = 0;
		LOG_ERR("Ship mode did not cut power");
	}
	k_work_reschedule(&check_work, K_SECONDS(CONFIG_HAVEN_BATTERY_CHECK_PERIOD_S));
}

int battery_guard_init(void)
{
	int err;

	if (!adc_is_ready_dt(&vsys_ch) || !adc_is_ready_dt(&vbus_ch) || !gpio_is_ready_dt(&ship)) {
		return -ENODEV;
	}
	err = gpio_pin_configure_dt(&ship, GPIO_OUTPUT_INACTIVE);
	if (err) {
		return err;
	}
	/* Charger status pins: only for the self-test report, so a failure here is not fatal. */
	chg_pins_ok = gpio_is_ready_dt(&chg) && gpio_is_ready_dt(&chg_err) &&
		      gpio_pin_configure_dt(&chg, GPIO_INPUT) == 0 &&
		      gpio_pin_configure_dt(&chg_err, GPIO_INPUT) == 0;
	err = adc_channel_setup_dt(&vsys_ch);
	if (err) {
		return err;
	}
	err = adc_channel_setup_dt(&vbus_ch);
	if (err) {
		return err;
	}
	k_work_reschedule(&check_work, K_SECONDS(2));
	LOG_INF("Battery guard on: cutoff %d mV, every %d s", CONFIG_HAVEN_BATTERY_CUTOFF_MV,
		CONFIG_HAVEN_BATTERY_CHECK_PERIOD_S);
	return 0;
}

int battery_guard_last_mv(void)
{
	return last_mv;
}

void battery_guard_fill_status(struct pod_status *st)
{
	int vsys = read_mv(&vsys_ch, VSYS_NUM, VSYS_DEN);	/* fresh reading, not the last periodic one */
	int vbus = read_mv(&vbus_ch, VBUS_NUM, VBUS_DEN);

	st->batt_mv = vsys > 0 ? vsys : last_mv;
	if (vbus < 0) {
		vbus = last_vbus_mv;
	}
	st->usb = vbus < 0 ? POD_UNKNOWN : (vbus >= VBUS_PRESENT_MV);
	if (chg_pins_ok) {
		st->charging = gpio_pin_get_dt(&chg) > 0;	/* _dt applies ACTIVE_LOW: 1 = asserted */
		st->chg_fault = gpio_pin_get_dt(&chg_err) > 0;
	} else {
		st->charging = POD_UNKNOWN;
		st->chg_fault = POD_UNKNOWN;
	}
}

#else /* !CONFIG_HAVEN_BATTERY_GUARD */

int battery_guard_init(void)
{
	return 0;
}

int battery_guard_last_mv(void)
{
	return 0;
}

void battery_guard_fill_status(struct pod_status *st)
{
	st->batt_mv = 0;		/* this board has no battery sensor (e.g. the dev kit) */
	st->usb = POD_UNKNOWN;
	st->charging = POD_UNKNOWN;
	st->chg_fault = POD_UNKNOWN;
}

#endif
