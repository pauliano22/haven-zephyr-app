/* Tiny host-test harness -- no framework dependency (Unity/CMocka aren't
 * vendored in this repo and pulling one in for this pass felt like more
 * risk than value). Each TEST_CASE prints PASS/FAIL with the failing
 * expression; main() reports a final tally and a nonzero exit code on any
 * failure, matching CI's normal pass/fail convention.
 */
#ifndef HAVEN_TEST_HARNESS_H_
#define HAVEN_TEST_HARNESS_H_

#include <math.h>
#include <stdio.h>

static int haven_test_pass_count;
static int haven_test_fail_count;

#define CHECK(cond) \
	do { \
		if (cond) { \
			haven_test_pass_count++; \
		} else { \
			haven_test_fail_count++; \
			printf("  FAIL [%s:%d] %s\n", __FILE__, __LINE__, #cond); \
		} \
	} while (0)

#define CHECK_FLOAT_NEAR(a, b, eps) CHECK(fabsf((float)(a) - (float)(b)) <= (eps))

#define RUN(test_fn) \
	do { \
		printf("-- %s --\n", #test_fn); \
		test_fn(); \
	} while (0)

static inline int haven_test_summary(const char *suite_name)
{
	printf("\n%s: %d passed, %d failed\n", suite_name, haven_test_pass_count,
	       haven_test_fail_count);
	return haven_test_fail_count == 0 ? 0 : 1;
}

#endif /* HAVEN_TEST_HARNESS_H_ */
