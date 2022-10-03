/* SPDX-License-Identifier: GPL-2.0 */

#ifndef LEDS_REALTEK_LED_REGFIELD_H
#define LEDS_REALTEK_LED_REGFIELD_H

#include <linux/device.h>
#include <linux/fwnode.h>
#include <linux/leds.h>
#include <linux/regmap.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0)
#include <linux/container_of.h>
#else
#include <linux/kernel.h>
#endif

/*
 * Register field LED
 *
 * Next to being able to turn an LED on or off, Realtek provides LED management
 * peripherals with hardware accelerated blinking modes with 50% duty cycle.
 */
struct regfield_led_blink_mode {
	u16 toggle_ms; /* Toggle interval in ms */
	u8 mode; /* ASIC mode bits */
};

struct regfield_led_modes {
	u8 off;
	u8 on;
	/*
	 * List of blink modes. Must be sorted by interval and terminated by an
	 * entry where regfield_led_blink_mode::toggle_ms equals zero.
	 */
	struct regfield_led_blink_mode blink[];
};

struct regfield_led {
	struct led_classdev cdev;
	const struct regfield_led_modes *modes;
	struct regmap_field *field;
	void (*commit)(struct regfield_led *led);
	bool active_low;
};

static inline struct regfield_led *to_regfield_led(struct led_classdev *cdev)
{
	return container_of(cdev, struct regfield_led, cdev);
}

int regfield_led_init(struct regfield_led *led, struct regmap_field *field,
		      struct fwnode_handle *led_node, const struct regfield_led_modes *modes);

#endif
