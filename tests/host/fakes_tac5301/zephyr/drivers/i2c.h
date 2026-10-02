/* Host-test fake for <zephyr/drivers/i2c.h>, TAC5301-Q1 variant.
 *
 * The shared fakes/zephyr/drivers/i2c.h (used by the ADAU1860 tests)
 * hardcodes a 4-byte register address, matching that chip's control port.
 * The TAC5301-Q1 uses a single byte of register address per the real
 * datasheet (section 6.4.1) -- a different wire format, not just a
 * different address space -- so this is a separate fake rather than a
 * parameterised version of the shared one, to avoid any risk of changing
 * behaviour under the 120+ existing ADAU1860 tests.
 *
 * Built via `-Ifakes_tac5301 -Ifakes`: this directory's zephyr/drivers/i2c.h
 * is found first for that one header; everything else (device.h, kernel.h,
 * logging/log.h, sys/util.h, sys/byteorder.h) still resolves from the
 * shared fakes/ tree.
 */
#ifndef FAKE_ZEPHYR_DRIVERS_I2C_TAC5301_H_
#define FAKE_ZEPHYR_DRIVERS_I2C_TAC5301_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>

struct i2c_dt_spec {
	const struct device *bus;
	uint16_t addr;
};

extern const struct device haven_fake_i2c_bus_dev;
#define I2C_DT_SPEC_GET(node) { .bus = &haven_fake_i2c_bus_dev, .addr = 0x50 }

#define HAVEN_FAKE_I2C_LOG_MAX 512
#define HAVEN_FAKE_I2C_MAX_PAYLOAD 32

struct haven_fake_i2c_xfer {
	uint8_t reg;
	uint8_t data[HAVEN_FAKE_I2C_MAX_PAYLOAD];
	size_t len;
};

static struct haven_fake_i2c_xfer haven_fake_i2c_log[HAVEN_FAKE_I2C_LOG_MAX];
static size_t haven_fake_i2c_log_count;
static int haven_fake_i2c_fail_writes;

/* Which page each logged write happened on -- the real device is paged
 * (register 0x00 selects it); tests need this to disambiguate e.g. page-0
 * reg 0x73 (DSP_CFG1) from page-1 reg 0x73 (MICBIAS_CFG).
 */
static uint8_t haven_fake_i2c_current_page;
static uint8_t haven_fake_i2c_log_page[HAVEN_FAKE_I2C_LOG_MAX];

static inline void haven_fake_i2c_reset(void)
{
	haven_fake_i2c_log_count = 0;
	haven_fake_i2c_fail_writes = 0;
	haven_fake_i2c_current_page = 0;
}

static inline int i2c_write_dt(const struct i2c_dt_spec *spec, const uint8_t *buf, uint32_t len)
{
	(void)spec;
	if (haven_fake_i2c_fail_writes) {
		return -5; /* -EIO */
	}
	if (len < 1 || len - 1 > HAVEN_FAKE_I2C_MAX_PAYLOAD) {
		return -22; /* -EINVAL */
	}
	uint8_t reg = buf[0];

	if (reg == 0x00) { /* PAGE_CFG: track it, but still log the write itself. */
		haven_fake_i2c_current_page = len >= 2 ? buf[1] : 0;
	}
	if (haven_fake_i2c_log_count >= HAVEN_FAKE_I2C_LOG_MAX) {
		return -22;
	}
	struct haven_fake_i2c_xfer *x = &haven_fake_i2c_log[haven_fake_i2c_log_count];

	x->reg = reg;
	x->len = len - 1;
	memcpy(x->data, buf + 1, x->len);
	haven_fake_i2c_log_page[haven_fake_i2c_log_count] = haven_fake_i2c_current_page;
	haven_fake_i2c_log_count++;
	return 0;
}

static inline int i2c_write_read_dt(const struct i2c_dt_spec *spec, const void *write_buf,
				    size_t num_write, void *read_buf, size_t num_read)
{
	(void)spec;
	(void)write_buf;
	(void)num_write;
	/* Nothing in this driver reads back over I2C yet (no STATUS poll,
	 * unlike the ADAU1860) -- return zeros if that ever changes. */
	memset(read_buf, 0, num_read);
	return 0;
}

/* Test helpers: find the last write to `reg` on `page`, or count them. */
static inline const struct haven_fake_i2c_xfer *haven_fake_i2c_last_write_page(uint8_t page, uint8_t reg)
{
	for (size_t i = haven_fake_i2c_log_count; i > 0; i--) {
		if (haven_fake_i2c_log_page[i - 1] == page && haven_fake_i2c_log[i - 1].reg == reg) {
			return &haven_fake_i2c_log[i - 1];
		}
	}
	return NULL;
}

static inline size_t haven_fake_i2c_count_writes_page(uint8_t page, uint8_t reg)
{
	size_t n = 0;

	for (size_t i = 0; i < haven_fake_i2c_log_count; i++) {
		if (haven_fake_i2c_log_page[i] == page && haven_fake_i2c_log[i].reg == reg) {
			n++;
		}
	}
	return n;
}

#endif /* FAKE_ZEPHYR_DRIVERS_I2C_TAC5301_H_ */
