/* Host tests for the pod self-test rules (pod_status.c) and its app message (ack.c's
 * ack_format_event_status). Both production files are included as-is.
 */
#include "test_harness.h"

#include <errno.h>
#include <string.h>

#include "../../src/ack.c"
#include "../../src/pod_status.c"
#include "../../src/protocol.c"

static struct pod_status healthy(void)
{
	struct pod_status st = { .batt_mv = 3850, .usb = 0, .charging = 0, .chg_fault = 0, .codec = 1 };
	return st;
}

static void test_percent_curve(void)
{
	CHECK(pod_status_batt_pct(0) == POD_UNKNOWN);
	CHECK(pod_status_batt_pct(-5) == POD_UNKNOWN);
	CHECK(pod_status_batt_pct(3000) == 0);
	CHECK(pod_status_batt_pct(3300) == 0);
	CHECK(pod_status_batt_pct(3800) == 50);
	CHECK(pod_status_batt_pct(3850) == 57);
	CHECK(pod_status_batt_pct(4200) == 100);
	CHECK(pod_status_batt_pct(4400) == 100);
	/* never goes down as the voltage goes up */
	for (int mv = 3200, prev = 0; mv <= 4300; mv += 10) {
		int p = pod_status_batt_pct(mv);
		CHECK(p >= prev);
		prev = p;
	}
}

static void test_problem_priority(void)
{
	struct pod_status st = healthy();

	CHECK(strcmp(pod_status_problem(&st), "none") == 0);
	st.batt_mv = 3350;
	CHECK(strcmp(pod_status_problem(&st), "battery_low") == 0);
	st.usb = 1;					/* low but on the charger: fine */
	CHECK(strcmp(pod_status_problem(&st), "none") == 0);
	st.chg_fault = 1;
	CHECK(strcmp(pod_status_problem(&st), "charger_fault") == 0);
	st.codec = 0;					/* no sound at all outranks everything */
	CHECK(strcmp(pod_status_problem(&st), "codec") == 0);
}

static void test_unknowns_are_not_problems(void)
{
	/* the dev kit: no battery sensor, no charger pins, codec unknown */
	struct pod_status st = { .batt_mv = 0, .usb = POD_UNKNOWN, .charging = POD_UNKNOWN,
				 .chg_fault = POD_UNKNOWN, .codec = POD_UNKNOWN };

	CHECK(strcmp(pod_status_problem(&st), "none") == 0);
	/* the pod: sensor present but read nothing */
	st.usb = 0;
	CHECK(strcmp(pod_status_problem(&st), "no_battery") == 0);
}

static void test_status_message(void)
{
	char buf[ACK_MAX_LEN];
	struct pod_status st = healthy();
	int n = ack_format_event_status(&st, buf, sizeof(buf));

	CHECK(n > 0);
	CHECK(strcmp(buf, "{\"event\":\"status\",\"mv\":3850,\"pct\":57,\"usb\":false,\"chg\":false,"
			  "\"fault\":false,\"codec\":true,\"problem\":\"none\"}\n") == 0);

	st = (struct pod_status){ .batt_mv = 0, .usb = POD_UNKNOWN, .charging = POD_UNKNOWN,
				  .chg_fault = POD_UNKNOWN, .codec = POD_UNKNOWN };
	CHECK(ack_format_event_status(&st, buf, sizeof(buf)) > 0);
	CHECK(strcmp(buf, "{\"event\":\"status\",\"mv\":0,\"pct\":null,\"usb\":null,\"chg\":null,"
			  "\"fault\":null,\"codec\":null,\"problem\":\"none\"}\n") == 0);
}

static void test_longest_status_fits(void)
{
	char buf[ACK_MAX_LEN];
	/* every field at its longest: 4-digit mV, 100 %, all "false", the longest problem code */
	struct pod_status st = { .batt_mv = 4200, .usb = 0, .charging = 0, .chg_fault = 1, .codec = 1 };
	int n;

	st.chg_fault = 1;
	n = ack_format_event_status(&st, buf, sizeof(buf));
	CHECK(n > 0 && n < ACK_MAX_LEN);
	st.batt_mv = 123456;				/* nonsense reading is clamped, still fits */
	st.codec = 0;
	n = ack_format_event_status(&st, buf, sizeof(buf));
	CHECK(n > 0 && strstr(buf, "\"mv\":9999") != NULL);
	CHECK(ack_format_event_status(&st, buf, 20) == -ENOSPC);
}

int main(void)
{
	RUN(test_percent_curve);
	RUN(test_problem_priority);
	RUN(test_unknowns_are_not_problems);
	RUN(test_status_message);
	RUN(test_longest_status_fits);
	return haven_test_summary("test_pod_status");
}
