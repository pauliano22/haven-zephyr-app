#include "wake_button.h"
#include "ble_transport.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>

LOG_MODULE_REGISTER(wake_button, LOG_LEVEL_INF);

#define SW0_NODE DT_ALIAS(sw0)

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET_OR(SW0_NODE, gpios, {0});
static struct gpio_callback button_cb_data;

/* GPIO ISR context -- keep this short and non-blocking. Deferring to the
 * system workqueue also lets ble_transport_wake_fast_advertising() safely
 * call into the Bluetooth stack, which isn't ISR-safe.
 */
static void wake_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	ble_transport_wake_fast_advertising();
}

static K_WORK_DEFINE(wake_work, wake_work_handler);

static void button_pressed(const struct device *dev, struct gpio_callback *cb,
			    uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	k_work_submit(&wake_work);
}

int wake_button_init(void)
{
	int err;

	if (!gpio_is_ready_dt(&button)) {
		LOG_ERR("Button device not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (err) {
		LOG_ERR("Failed to configure button pin (err %d)", err);
		return err;
	}

	err = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	if (err) {
		LOG_ERR("Failed to configure button interrupt (err %d)", err);
		return err;
	}

	gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb_data);

	LOG_INF("Wake button ready on %s pin %d", button.port->name, button.pin);
	return 0;
}
