// SPDX-License-Identifier: GPL-2.0
/*
 * Base support for simple, monochromatic LEDs, configured by a short register
 * field.  The register field should allow the LED to be turned on or off, or
 * toggled at a predetermined rate with a 50% duty cycle.
 */

#include <linux/leds.h>
#include <linux/property.h>

#include "led-regfield.h"

static int regfield_led_set_mode(struct regfield_led *led, unsigned int mode)
{
	int err;

	err = regmap_field_write(led->field, mode);
	if (!err && led->commit)
		led->commit(led);

	return err;
}

static void regfield_led_brightness_set(struct led_classdev *led_cdev,
					enum led_brightness brightness)
{
	struct regfield_led *led = to_regfield_led(led_cdev);
	bool turn_off = brightness == 0;

	if ((!led->active_low && turn_off) || (led->active_low && !turn_off))
		regfield_led_set_mode(led, led->modes->off);
	else
		regfield_led_set_mode(led, led->modes->on);
}

static enum led_brightness regfield_led_brightness_get(struct led_classdev *led_cdev)
{
	struct regfield_led *led = to_regfield_led(led_cdev);
	u32 val = 0;

	regmap_field_read(led->field, &val);

	if ((!led->active_low && (val == led->modes->off)) ||
		(led->active_low && (val == led->modes->on)))
		return 0;
	else
		return 1;
}

static int regfield_led_blink_set(struct led_classdev *led_cdev, unsigned long *delay_on,
				  unsigned long *delay_off)
{
	struct regfield_led *led = to_regfield_led(led_cdev);
	const struct regfield_led_blink_mode *blink = led->modes->blink;
	u32 cycle_ms = *delay_on + *delay_off;
	int err;

	if (cycle_ms == 0)
		cycle_ms = 500;

	while (blink->toggle_ms && (blink + 1)->toggle_ms) {
		/*
		 * Split at the arithmetic mean of intervals, which compares
		 * the half cycle interval (cycle_ms / 2) to the mean toggle
		 * interval ((blink->toggle_ms + (blink + 1)->toggle_ms) / 2).
		 * Since the (/ 2) is common on both sides, it can be dropped.
		 */
		if (cycle_ms < (blink->toggle_ms + (blink + 1)->toggle_ms))
			break;
		blink++;
	}

	err = regfield_led_set_mode(led, blink->mode);
	if (err)
		return err;

	*delay_on = blink->toggle_ms;
	*delay_off = blink->toggle_ms;

	return 0;
}

int regfield_led_init(struct regfield_led *led, struct regmap_field *field,
		      struct fwnode_handle *fwnode, const struct regfield_led_modes *modes)
{
	if (IS_ERR_OR_NULL(field) || !modes)
		return -EINVAL;

	led->field = field;
	led->modes = modes;
	led->active_low = fwnode_property_read_bool(fwnode, "active-low");

	led->cdev.max_brightness = 1;
	led->cdev.brightness_set = regfield_led_brightness_set;
	led->cdev.brightness_get = regfield_led_brightness_get;
	led->cdev.blink_set = regfield_led_blink_set;

	return 0;
}
