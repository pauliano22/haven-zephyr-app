/* Host tests for the low-battery decision rule in battery_guard.c (the ADC and GPIO parts
 * only build for the haven_v2 board and are not covered here).
 */
#include "test_harness.h"

#include "../../src/battery_guard.c"

static void test_low_readings_count_up(void)
{
	int n = 0;
	n = battery_guard_step(n, 3200, 0, 3300, 3000); CHECK(n == 1);
	n = battery_guard_step(n, 3250, 0, 3300, 3000); CHECK(n == 2);
	n = battery_guard_step(n, 3299, 0, 3300, 3000); CHECK(n == 3);
}

static void test_one_good_reading_resets(void)
{
	int n = 2;
	n = battery_guard_step(n, 3300, 0, 3300, 3000); CHECK(n == 0);
	n = battery_guard_step(2, 4100, 0, 3300, 3000); CHECK(n == 0);
}

static void test_never_on_charger(void)
{
	CHECK(battery_guard_step(2, 2900, 5000, 3300, 3000) == 0);
	CHECK(battery_guard_step(2, 2900, 3000, 3300, 3000) == 0);
}

static void test_bad_reading_never_shuts_down(void)
{
	CHECK(battery_guard_step(2, 0, 0, 3300, 3000) == 0);
	CHECK(battery_guard_step(2, -5, 0, 3300, 3000) == 0);
}

int main(void)
{
	RUN(test_low_readings_count_up);
	RUN(test_one_good_reading_resets);
	RUN(test_never_on_charger);
	RUN(test_bad_reading_never_shuts_down);
	return haven_test_summary("test_battery_guard");
}
