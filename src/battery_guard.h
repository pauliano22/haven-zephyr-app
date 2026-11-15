/* Low-battery shutdown for boards with an nPM1100 (haven_v2 / v3 pod).
 * See battery_guard.c. On boards without the battery-sense channel this is a no-op.
 */
#ifndef HAVEN_BATTERY_GUARD_H_
#define HAVEN_BATTERY_GUARD_H_

#include "pod_status.h"

int battery_guard_init(void);

/* Last battery reading in millivolts, or 0 if none yet / not supported. */
int battery_guard_last_mv(void);

/* Fill the battery and charger fields of a pod status report (pod_status.h); POD_UNKNOWN where
 * the board cannot tell. Takes a fresh ADC reading; call from thread context.
 */
void battery_guard_fill_status(struct pod_status *st);

/* Pure decision logic, separated so it can be unit-tested on the host:
 * returns the new count of consecutive low readings (0 resets). Shut down when it reaches `need`.
 */
int battery_guard_step(int low_count, int vsys_mv, int vbus_mv, int cutoff_mv, int vbus_present_mv);

#endif /* HAVEN_BATTERY_GUARD_H_ */
