// SPDX-License-Identifier: GPL-2.0

#include <dt-bindings/leds/realtek-led-trigger.h>
#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/leds.h>
#include <linux/mfd/core.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include "led-regfield.h"

/*
 * Realtek switch port LED
 *
 * The switch ASIC can control multiple LEDs per phy, based on a number of
 * matching conditions. Alternatively, each individual LED output can also be
 * configured for manual control.
 */
enum rtl_led_output_mode {
	RTL_LED_OUTPUT_SERIAL		= 0,
	RTL_LED_OUTPUT_SCAN_SINGLE	= 1,
	RTL_LED_OUTPUT_SCAN_BICOLOR	= 2,
	RTL_LED_OUTPUT_DISABLED		= 3,
};

struct led_port_group {
	unsigned int index;
	struct regmap_field *setting;
	unsigned int size;
	/* bitmap to keep track of associated ports */
	unsigned long *ports;
};

#define GROUP_LIST_INDEX(cfg, grp, idx)		((cfg)->port_led_count * (grp) + (idx))

struct switch_port_led_ctrl;

struct switch_port_led {
	struct regfield_led led;
	struct switch_port_led_ctrl *ctrl;
	struct led_port_group *current_group;
	u32 trigger_flags;
	u8 port;
	u8 index;
	bool is_secondary;
};

struct switch_port_led_config {
	/* Number of switch ports with configurable LEDs */
	unsigned int port_count;
	/* Number of LEDs per port */
	unsigned int port_led_count;
	/* Number of groups the LEDs can be assigned to for status offloading */
	unsigned int group_count;
	/* Whether the secondary (SFP cage) LEDs can be controlled separately */
	bool independent_secondaries;
	/* Port LED on/off/blink modes */
	const struct regfield_led_modes *modes;
	/* reg_field storing the index-specific user mode */
	struct reg_field (*led_regfield)(unsigned int port, unsigned int index);
	/* reg_field storing the index-specific offloaded group setting */
	struct reg_field (*group_regfield)(unsigned int group, unsigned int index);
	/* Configure and start the peripheral */
	int (*init)(struct switch_port_led_ctrl *ctrl, enum rtl_led_output_mode mode);
	/* Optional - Latch the updated LED configuration */
	void (*led_commit)(struct regfield_led *led);
	/* Switch between HW offloading or user control */
	int (*set_hw_managed)(struct switch_port_led *led, bool hw_managed);
	/* Translate a generic trigger to a gen-specific one */
	int (*trigger_xlate)(struct switch_port_led *led, u32 trigger);
	/*
	 * Find the group the LED with this trigger setting can be assigned to.
	 * Can be either an existing group with identical settings, or an empty
	 * group. Return a group on success, or < 0 on failure.
	 */
	struct led_port_group *(*map_group)(struct switch_port_led *led, u32 trigger);
	/* Configure the LED for HW offloading according to the provided group settings */
	int (*assign_group)(struct switch_port_led *led, struct led_port_group *group);
};

struct switch_port_led_mask {
	/* TODO could use two :4 bit fields */
	u8 primary;
	u8 secondary;
};

struct switch_port_led_ctrl {
	struct device *dev;
	struct regmap *map;
	const struct switch_port_led_config *cfg;
	struct mutex lock;
	struct switch_port_led_mask *available_leds;
	struct led_port_group *groups;
};

static inline struct switch_port_led *to_switch_port_led(struct led_classdev *cdev)
{
	return container_of(to_regfield_led(cdev), struct switch_port_led, led);
}

static struct led_port_group *switch_port_led_get_group(
	struct switch_port_led *pled, unsigned int group)
{
	unsigned int i = GROUP_LIST_INDEX(pled->ctrl->cfg, group, pled->index);

	return &pled->ctrl->groups[i];
}

static struct led_port_group *rtl_generic_port_led_map_group(struct switch_port_led *led,
							     u32 trigger)
{
	struct switch_port_led_ctrl *ctrl = led->ctrl;
	int rtl_trg = ctrl->cfg->trigger_xlate(led, trigger);
	unsigned int i;
	u32 current_trg;

	if (rtl_trg < 0)
		return ERR_PTR(rtl_trg);

	for (i = 0; i < led->ctrl->cfg->group_count; i++) {
		struct led_port_group *group = switch_port_led_get_group(led, i);
		int err = regmap_field_read(group->setting, &current_trg);

		if (err)
			return ERR_PTR(err);

		if (current_trg == rtl_trg || bitmap_empty(group->ports, group->size))
			return group;
	}

	dev_warn(ctrl->dev, "no available group for (%d,%d,%d) with trigger 0x%02x\n",
		 led->port, led->index, led->is_secondary, rtl_trg);
	return ERR_PTR(-ENOSPC);
}

/* Maple registers */
#define RTL838X_REG_LED_MODE_SEL		0x1004
#define RTL838X_REG_LED_GLB_CTRL		0xa000
#define RTL838X_GLB_CTRL_COMBO_MODE		GENMASK(8, 7)
#define RTL838X_GLB_CTRL_HIGH_PORTS		GENMASK(5, 3)
#define RTL838X_GLB_CTRL_LOW_PORTS		GENMASK(2, 0)
#define RTL838X_REG_LED_MODE_CTRL		0xa004
#define RTL838X_REG_LED_P_EN_CTRL		0xa008
#define RTL838X_REG_LED_SW_P_EN_CTRL(index)	(0xa010 + 4 * (index))
#define RTL838X_REG_LED_SW_CTRL(port)		(0xa01c + 4 * (port))
#define RTL838X_SW_SETTING_WIDTH		3

#define RTL838X_PORT_LED_COUNT			3
#define RTL838X_GROUP_SETTING_WIDTH		5
#define RTL838X_GROUP_SETTING_SHIFT(grp, idx)	\
	(RTL838X_GROUP_SETTING_WIDTH * ((idx) + RTL838X_PORT_LED_COUNT * (grp)))

#define RTL838X_PORT_COMBO_LOW			20
#define RTL838X_PORT_COMBO_HIGH			24

/* Cypress registers */
#define RTL839X_REG_LED_GLB_CTRL		0x00e4
#define RTL839X_GLB_CTRL_LED_EN			BIT(5)
#define RTL839X_GLB_CTRL_LED_NUM_SEL		GENMASK(3, 2)
#define RTL839X_GLB_CTRL_LED_IF_SEL		GENMASK(1, 0)
#define RTL839X_REG_LED_COPR_SET_SEL_CTRL(port)	(0x00f0 + 4 * ((port) / 16))
#define RTL839X_REG_LED_FIB_SET_SEL_CTRL(port)	(0x0100 + 4 * ((port) / 16))
#define RTL839X_REG_LED_COPR_PMASK_CTRL(port)	(0x0110 + 4 * ((port) / 32))
#define RTL839X_REG_LED_FIB_PMASK_CTRL(port)	(0x0118 + 4 * ((port) / 32))
#define RTL839X_REG_LED_COMBO_CTRL(port)	(0x0120 + 4 * ((port) / 32))
#define RTL839X_REG_LED_SW_CTRL			0x0128
#define RTL839X_REG_LED_SW_P_EN_CTRL(port)	(0x012c + 4 * ((port) / 10))
#define RTL839X_REG_LED_SW_P_CTRL(port)		(0x0144 + 4 * (port))

#define RTL839X_PORT_LED_COUNT			3
#define RTL839X_SW_SETTING_WIDTH		3
#define RTL839X_GROUP_SETTING_WIDTH		5
#define RTL839X_GROUP_SETTING_REG(grp)		(0x00ec - 4 * ((grp) / 2))
#define RTL839X_GROUP_SETTING_SHIFT(grp, idx)	\
	(RTL839X_GROUP_SETTING_WIDTH * ((idx) + RTL839X_PORT_LED_COUNT * ((grp) % 2)))

/* Maple and Cypress have mostly the same trigger configuration values */
enum rtl83xx_port_trigger {
	RTL83XX_TRIG_LINK_ACT		= 0,
	RTL83XX_TRIG_LINK		= 1,
	RTL83XX_TRIG_ACT		= 2,
	RTL83XX_TRIG_ACT_RX		= 3,
	RTL83XX_TRIG_ACT_TX		= 4,
	RTL83XX_TRIG_DUPLEX_MODE	= 6,
	RTL83XX_TRIG_LINK_1G		= 7,
	RTL83XX_TRIG_LINK_100M		= 8,
	RTL83XX_TRIG_LINK_10M		= 9,
	RTL83XX_TRIG_LINK_ACT_1G	= 10,
	RTL83XX_TRIG_LINK_ACT_100M	= 11,
	RTL83XX_TRIG_LINK_ACT_10M	= 12,
	RTL83XX_TRIG_LINK_ACT_1G_100M	= 13,
	RTL83XX_TRIG_LINK_ACT_1G_10M	= 14,
	RTL83XX_TRIG_LINK_ACT_100M_10M	= 15,
	RTL83XX_TRIG_LINK_ACT_10G	= 21,
	RTL83XX_TRIG_DISABLED		= 31,
};

static int rtl83xx_port_trigger_xlate(u32 port_led_trigger)
{
	switch (port_led_trigger) {
	case RTL_LED_NONE:
		return RTL83XX_TRIG_DISABLED;
	case RTL_LED_ACT_RX:
		return RTL83XX_TRIG_ACT_RX;
	case RTL_LED_ACT_TX:
		return RTL83XX_TRIG_ACT_TX;
	case RTL_LED_ACT:
		return RTL83XX_TRIG_ACT;
	case RTL_LED_LINK_10 | RTL_LED_LINK_100 | RTL_LED_LINK_1000:
		return RTL83XX_TRIG_LINK;
	case RTL_LED_LINK_10:
		return RTL83XX_TRIG_LINK_10M;
	case RTL_LED_LINK_100:
		return RTL83XX_TRIG_LINK_100M;
	case RTL_LED_LINK_1000:
		return RTL83XX_TRIG_LINK_1G;
	case RTL_LED_ACT | RTL_LED_LINK_10 | RTL_LED_LINK_100 | RTL_LED_LINK_1000:
		return RTL83XX_TRIG_LINK_ACT;
	case RTL_LED_ACT | RTL_LED_LINK_10:
		return RTL83XX_TRIG_LINK_ACT_10M;
	case RTL_LED_ACT | RTL_LED_LINK_100:
		return RTL83XX_TRIG_LINK_ACT_100M;
	case RTL_LED_ACT | RTL_LED_LINK_1000:
		return RTL83XX_TRIG_LINK_ACT_1G;
	case RTL_LED_ACT | RTL_LED_LINK_10000:
		return RTL83XX_TRIG_LINK_ACT_10G;
	case RTL_LED_ACT | RTL_LED_LINK_10 | RTL_LED_LINK_100:
		return RTL83XX_TRIG_LINK_ACT_100M_10M;
	case RTL_LED_ACT | RTL_LED_LINK_10 | RTL_LED_LINK_1000:
		return RTL83XX_TRIG_LINK_ACT_1G_10M;
	case RTL_LED_ACT | RTL_LED_LINK_100 | RTL_LED_LINK_1000:
		return RTL83XX_TRIG_LINK_ACT_1G_100M;
	default:
		return -EINVAL;
	}
}

/*
 * SoC specific implementation for RTL8380 series (Maple)
 */
static int rtl838x_port_trigger_xlate(struct switch_port_led *led, u32 port_led_trigger)
{
	if (port_led_trigger & (RTL_LED_LINK_2500 | RTL_LED_LINK_5000 | RTL_LED_LINK_10000))
		return -EINVAL;

	return rtl83xx_port_trigger_xlate(port_led_trigger);
}

/*
 * Maple/RTL838x has two static groups:
 *   - group 0: ports 0-23
 *   - group 1: ports 24-27 (high combo ports)
 *
 * When both groups need the same setting, the generic implementation would
 * always return the first group. However, high ports can only be controlled
 * via the second group, so we need an override of the generic implementation.
 */
static struct led_port_group *rtl838x_port_led_map_group(struct switch_port_led *led, u32 trigger)
{
	int rtl_trigger = rtl838x_port_trigger_xlate(led, trigger);
	struct switch_port_led_ctrl *ctrl = led->ctrl;
	struct led_port_group *group;
	u32 current_trigger;
	int err;

	if (rtl_trigger < 0)
		return ERR_PTR(rtl_trigger);

	if (led->port < RTL838X_PORT_COMBO_HIGH)
		group = switch_port_led_get_group(led, 0);
	else
		group = switch_port_led_get_group(led, 1);

	err = regmap_field_read(group->setting, &current_trigger);
	if (err)
		return ERR_PTR(err);

	if (current_trigger != rtl_trigger && !bitmap_empty(group->ports, group->size)) {
		dev_warn(ctrl->dev, "cannot map (%d,%d) to group %d: 0x%02x != 0x%02x\n",
			 led->port, led->index, group->index, current_trigger, rtl_trigger);
		return ERR_PTR(-ENOSPC);
	}

	return group;
}

int rtl838x_port_led_assign_group(struct switch_port_led *led, struct led_port_group *group)
{
	/*
	 * Since group assignments are static on Maple, this is a no-op.
	 * rtl838x_port_led_map_group() will provide the correct group assignments.
	 */
	return 0;
}

static int rtl838x_port_led_set_hw_managed(struct switch_port_led *led, bool hw_managed)
{
	unsigned int reg = RTL838X_REG_LED_SW_P_EN_CTRL(led->index);
	u32 val = hw_managed ? 0 : BIT(led->port);

	return regmap_update_bits(led->ctrl->map, reg, BIT(led->port), val);
}

static struct reg_field rtl838x_port_led_regfield(unsigned int port, unsigned int index)
{
	unsigned int reg = RTL838X_REG_LED_SW_CTRL(port);
	unsigned int shift = index * RTL838X_SW_SETTING_WIDTH;

	return (struct reg_field) REG_FIELD(reg, shift, shift + RTL838X_SW_SETTING_WIDTH - 1);
}

static struct reg_field rtl838x_port_led_group_regfield(unsigned int group, unsigned int index)
{
	unsigned int reg = RTL838X_REG_LED_MODE_CTRL;
	unsigned int shift = RTL838X_GROUP_SETTING_SHIFT(group, index);

	return (struct reg_field) REG_FIELD(reg, shift, shift + RTL838X_GROUP_SETTING_WIDTH - 1);
}

static int rtl838x_port_led_init(struct switch_port_led_ctrl *ctrl, enum rtl_led_output_mode mode)
{
	unsigned int led_possible_mask_high = 0;
	unsigned int led_possible_mask_low = 0;
	unsigned int combo_port_min = ctrl->cfg->port_count;
	unsigned int combo_port_max = 0;
	unsigned int combo_port_val = 0;
	unsigned int port;
	u32 glb_ctrl_mask;
	u32 glb_ctrl_val;
	int err;

	/* Disable all LEDs, (re-)enable when configuring */
	regmap_write(ctrl->map, RTL838X_REG_LED_P_EN_CTRL, 0);

	for (port = 0; port < ctrl->cfg->port_count; port++) {
		const struct switch_port_led_mask *led_masks = &ctrl->available_leds[port];
		u32 port_mask = led_masks->primary | led_masks->secondary;

		if (!port_mask)
			continue;

		if (port < RTL838X_PORT_COMBO_HIGH)
			led_possible_mask_low |= port_mask;
		else
			led_possible_mask_high |= port_mask;

		if (led_masks->primary && led_masks->secondary) {
			combo_port_min = min(combo_port_min, port);
			combo_port_max = max(combo_port_max, port);
		}

		/* Enable a port if any of its LED are used */
		err = regmap_update_bits(ctrl->map, RTL838X_REG_LED_P_EN_CTRL,
					 BIT(port), BIT(port));
		if (err)
			return err;
	}

	/*
	 * Combo ports are allowed in either [20, 23] or [24, 27].
	 * Setting the combo port field to a non-zero value, will cause extra
	 * LED values to be scanned out. The field value determines if these
	 * follow the primary LED data for port 23 of 27. The number of extra
	 * LEDs depends on the number of ports that is enabled (LED wise) in
	 * the applicable range.
	 */
	if (combo_port_min < RTL838X_PORT_COMBO_LOW) {
		dev_err(ctrl->dev, "combo ports < %d not supported\n", RTL838X_PORT_COMBO_LOW);
		return -EINVAL;
	}
	if (combo_port_min < RTL838X_PORT_COMBO_HIGH && combo_port_max >= RTL838X_PORT_COMBO_HIGH) {
		dev_err(ctrl->dev, "illegal combo port combination\n");
		return -EINVAL;
	}

	if (combo_port_min < RTL838X_PORT_COMBO_HIGH)
		combo_port_val = 1;
	else if (combo_port_min < ctrl->cfg->port_count)
		combo_port_val = 2;

	glb_ctrl_mask = RTL838X_GLB_CTRL_COMBO_MODE;
	glb_ctrl_val = FIELD_PREP(RTL838X_GLB_CTRL_COMBO_MODE, combo_port_val);

	/*
	 * The number-of-LEDs-per-port fields require a mask instead of a number.
	 * All lowest bits must be set, so e.g. BIT(1) is disallowed.
	 *
	 * According to the SDK, the high port mask cannot be empty, even if
	 * none of the LEDs are used. If no LEDs are configured, we must use the
	 * value of the low port mask.
	 */
	if (!led_possible_mask_high)
		led_possible_mask_high = led_possible_mask_low;

	if (led_possible_mask_low)
		led_possible_mask_low = GENMASK(fls(led_possible_mask_low) - 1, 0);
	if (led_possible_mask_high)
		led_possible_mask_high = GENMASK(fls(led_possible_mask_high) - 1, 0);

	glb_ctrl_mask |= RTL838X_GLB_CTRL_HIGH_PORTS | RTL838X_GLB_CTRL_LOW_PORTS;
	glb_ctrl_val |= FIELD_PREP(RTL838X_GLB_CTRL_LOW_PORTS, led_possible_mask_low);
	glb_ctrl_val |= FIELD_PREP(RTL838X_GLB_CTRL_HIGH_PORTS, led_possible_mask_high);

	err = regmap_update_bits(ctrl->map, RTL838X_REG_LED_GLB_CTRL, glb_ctrl_mask, glb_ctrl_val);
	if (err)
		return err;

	/* Set mode to enable output */
	return regmap_write(ctrl->map, RTL838X_REG_LED_MODE_SEL, mode);
}

static const struct regfield_led_modes rtl838x_port_led_modes = {
	.off = 0,
	.on = 5,
	/* Modes 6 and 7 appear to be a late additions to the list */
	.blink  = {
		{  32, 1},
		{  64, 2},
		{ 128, 3},
		{ 256, 6},
		{ 512, 4},
		{1024, 7},
		{ /* sentinel */ }
	},
};

static const struct switch_port_led_config rtl838x_port_led_config = {
	.port_count = 28,
	.port_led_count = 3,
	.group_count = 2,
	.independent_secondaries = false,
	.modes = &rtl838x_port_led_modes,
	.led_regfield = rtl838x_port_led_regfield,
	.group_regfield = rtl838x_port_led_group_regfield,
	.init = rtl838x_port_led_init,
	.set_hw_managed = rtl838x_port_led_set_hw_managed,
	.trigger_xlate = rtl838x_port_trigger_xlate,
	.map_group = rtl838x_port_led_map_group,
	.assign_group = rtl838x_port_led_assign_group,
};

/*
 * SoC specific implementation for RTL8390 series (Cypress)
 */
static void rtl839x_port_led_commit(struct regfield_led *rled)
{
	const struct switch_port_led *led = container_of(rled, struct switch_port_led, led);

	/*
	 * Could trigger the latching with delayed work,
	 * but that's probably not worth the overhead
	 */
	regmap_write(led->ctrl->map, RTL839X_REG_LED_SW_CTRL, 1);
}

static int rtl839x_port_trigger_xlate(struct switch_port_led *led, u32 port_led_trigger)
{
	if (port_led_trigger & (RTL_LED_LINK_2500 | RTL_LED_LINK_5000))
		return -EINVAL;

	return rtl83xx_port_trigger_xlate(port_led_trigger);
}

int rtl839x_port_led_assign_group(struct switch_port_led *led, struct led_port_group *group)
{
	unsigned int shift = 2 * (led->port % 16);
	u32 mask = GENMASK(1, 0) << shift;
	u32 val = group->index << shift;
	unsigned int reg_set;

	if (led->is_secondary)
		reg_set = RTL839X_REG_LED_FIB_SET_SEL_CTRL(led->port);
	else
		reg_set = RTL839X_REG_LED_COPR_SET_SEL_CTRL(led->port);

	return regmap_update_bits(led->ctrl->map, reg_set, mask, val);
}

static int rtl839x_port_led_set_hw_managed(struct switch_port_led *led, bool hw_managed)
{
	u32 port_field_mask = BIT(3 * (led->port % 10) + led->index);
	int reg = RTL839X_REG_LED_SW_P_EN_CTRL(led->port);
	u32 val = hw_managed ? 0 : port_field_mask;

	return regmap_update_bits(led->ctrl->map, reg, port_field_mask, val);
}

static struct reg_field rtl839x_port_led_regfield(unsigned int port, unsigned int index)
{
	unsigned int reg = RTL839X_REG_LED_SW_P_CTRL(port);
	unsigned int shift = index * RTL839X_SW_SETTING_WIDTH;

	return (struct reg_field) REG_FIELD(reg, shift, shift + RTL839X_SW_SETTING_WIDTH - 1);
}

static struct reg_field rtl839x_port_led_group_regfield(unsigned int group, unsigned int index)
{
	unsigned int reg = RTL839X_GROUP_SETTING_REG(group);
	unsigned int shift = RTL839X_GROUP_SETTING_SHIFT(group, index);

	return (struct reg_field) REG_FIELD(reg, shift, shift + RTL839X_GROUP_SETTING_WIDTH - 1);
}

static int rtl839x_port_led_init(struct switch_port_led_ctrl *ctrl, enum rtl_led_output_mode mode)
{
	u32 led_count = 0;
	unsigned int port;
	u32 reg_mask;
	u32 reg_val;
	u32 pmask;
	int err;

	/* Clear {COPR,FIB}_PMASK and COMBO_CTRL registers to disable all LEDs */
	for (port = 0; port < ctrl->cfg->port_count; port += 32) {
		regmap_write(ctrl->map, RTL839X_REG_LED_COPR_PMASK_CTRL(port), 0);
		regmap_write(ctrl->map, RTL839X_REG_LED_FIB_PMASK_CTRL(port), 0);
		regmap_write(ctrl->map, RTL839X_REG_LED_COMBO_CTRL(port), 0);
	}

	for (port = 0; port < ctrl->cfg->port_count; port++) {
		struct switch_port_led_mask *led_mask = &ctrl->available_leds[port];

		if (!led_mask->primary && !led_mask->secondary)
			continue;

		led_count = max_t(u32, led_count, fls(led_mask->primary | led_mask->secondary));
		pmask = BIT(port % 32);

		/*
		 * SDK will only set the COPR_PMASK bit if an RJ45 port is
		 * present, and FIB_PMASK if an SFP cage is present.
		 * Here instead, always trigger on both port types (i.e. set
		 * COPR_PMASK and FIB_PMASK), but tell the hardware there is
		 * only one LED for our (fake) combo port by also setting
		 * COMBO_CTRL.
		 * A real combo port with one LED should thus only ever need to
		 * specify a primary LED, consistent with the physical LED
		 * layout.
		 */
		err = regmap_update_bits(ctrl->map, RTL839X_REG_LED_COPR_PMASK_CTRL(port),
					 pmask, pmask);
		if (err)
			return err;
		err = regmap_update_bits(ctrl->map, RTL839X_REG_LED_FIB_PMASK_CTRL(port),
					 pmask, pmask);
		if (err)
			return err;

		if (led_mask->primary && led_mask->secondary)
			continue;

		err = regmap_update_bits(ctrl->map, RTL839X_REG_LED_COMBO_CTRL(port), pmask, pmask);
		if (err)
			return err;
	}

	reg_mask = RTL839X_GLB_CTRL_LED_EN;
	reg_mask |= RTL839X_GLB_CTRL_LED_NUM_SEL;
	reg_mask |= RTL839X_GLB_CTRL_LED_IF_SEL;
	reg_val = RTL839X_GLB_CTRL_LED_EN;
	reg_val |= FIELD_PREP(RTL839X_GLB_CTRL_LED_NUM_SEL, led_count);
	reg_val |= FIELD_PREP(RTL839X_GLB_CTRL_LED_IF_SEL, mode);

	return regmap_update_bits(ctrl->map, RTL839X_REG_LED_GLB_CTRL, reg_mask, reg_val);
}

static const struct regfield_led_modes rtl839x_port_led_modes = {
	.off = 0,
	.on = 7,
	.blink = {
		{  32, 1},
		{  64, 2},
		{ 128, 3},
		{ 256, 4},
		{ 512, 5},
		{1024, 6},
		{ /* sentinel */ }
	},
};

static const struct switch_port_led_config rtl839x_port_led_config = {
	.port_count = 52,
	.port_led_count = 3,
	.group_count = 4,
	.independent_secondaries = true,
	.modes = &rtl839x_port_led_modes,
	.led_regfield = rtl839x_port_led_regfield,
	.group_regfield = rtl839x_port_led_group_regfield,
	.led_commit = rtl839x_port_led_commit,
	.init = rtl839x_port_led_init,
	.set_hw_managed = rtl839x_port_led_set_hw_managed,
	.trigger_xlate = rtl839x_port_trigger_xlate,
	.map_group = rtl_generic_port_led_map_group,
	.assign_group = rtl839x_port_led_assign_group,
};

/*
 * Custom LED trigger interface
 */
static struct led_hw_trigger_type switch_port_rtl_hw_trigger_type;

static ssize_t rtl_hw_trigger_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct switch_port_led *pled = to_switch_port_led(cdev);

	return sprintf(buf, "%x\n", pled->trigger_flags);
}

/*
 * Add an LED to a group, leaving the old group as required.
 * To enable HW offloading, the HW trigger must be enabled separately.
 */
static int rtl_hw_trigger_assign(struct switch_port_led *led, int trigger)
{
	const struct switch_port_led_config *cfg = led->ctrl->cfg;
	struct led_port_group *group;
	u32 rtl_trigger;
	int err;

	rtl_trigger = cfg->trigger_xlate(led, trigger);
	if (rtl_trigger < 0)
		return rtl_trigger;

	/*
	 * Need to leave old group first, since we may need this to allocate a
	 * new group. On assignment failure, this needs to be rolled back.
	 */
	if (led->current_group)
		bitmap_clear(led->current_group->ports, led->port, 1);

	group = cfg->map_group(led, trigger);
	if (IS_ERR(group)) {
		err = PTR_ERR(group);
		goto err_out;
	}

	if (bitmap_empty(group->ports, group->size)) {
		err = regmap_field_write(group->setting, rtl_trigger);
		if (err)
			goto err_out;
	}

	err = cfg->assign_group(led, group);
	if (err)
		goto err_out;

	bitmap_set(group->ports, led->port, 1);
	led->current_group = group;

	return 0;

err_out:
	if (led->current_group)
		bitmap_set(led->current_group->ports, led->port, 1);

	return err;
}

static ssize_t rtl_hw_trigger_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct switch_port_led *pled = to_switch_port_led(cdev);
	struct switch_port_led_ctrl *ctrl = pled->ctrl;
	int err = 0;
	int trigger;
	int nchars;
	int value;

	if (sscanf(buf, "%x%n", &value, &nchars) != 1 || nchars + 1 < count)
		return -EINVAL;

	if (pled->trigger_flags == value)
		return count;

	trigger = ctrl->cfg->trigger_xlate(pled, value);
	if (trigger < 0)
		return trigger;

	mutex_lock(&ctrl->lock);

	if (pled->current_group) {
		err = rtl_hw_trigger_assign(pled, value);
		if (err)
			goto out;
	}

	pled->trigger_flags = value;

out:
	mutex_unlock(&ctrl->lock);

	if (err)
		return err;

	return count;
}
static DEVICE_ATTR_RW(rtl_hw_trigger);

/* TODO Change to tx/rx/link sysfs attributes like netdev? */
static struct attribute *rtl_hw_trigger_attrs[] = {
	&dev_attr_rtl_hw_trigger.attr,
	NULL
};
ATTRIBUTE_GROUPS(rtl_hw_trigger);

static int switch_port_led_trigger_activate(struct led_classdev *led_cdev)
{
	struct switch_port_led *pled = to_switch_port_led(led_cdev);
	int err = 0;

	mutex_lock(&pled->ctrl->lock);

	err = rtl_hw_trigger_assign(pled, pled->trigger_flags);
	if (err)
		goto out;

	err = pled->ctrl->cfg->set_hw_managed(pled, true);

out:
	mutex_unlock(&pled->ctrl->lock);

	return err;
}

static void switch_port_led_trigger_deactivate(struct led_classdev *led_cdev)
{
	struct switch_port_led *pled = to_switch_port_led(led_cdev);

	mutex_lock(&pled->ctrl->lock);

	if (pled->ctrl->cfg->set_hw_managed(pled, false))
		goto out;

	bitmap_clear(pled->current_group->ports, pled->port, 1);
	pled->current_group = NULL;

out:
	mutex_unlock(&pled->ctrl->lock);
}

static struct led_trigger switch_port_rtl_hw_trigger = {
	.name = "realtek-switchport",
	.activate = switch_port_led_trigger_activate,
	.deactivate = switch_port_led_trigger_deactivate,
	.trigger_type = &switch_port_rtl_hw_trigger_type,
};

static int switch_port_register_classdev(struct switch_port_led *pled, struct fwnode_handle *fwnode)
{
	struct led_init_data init_data = {};
	struct regmap_field *field;

	field = devm_regmap_field_alloc(pled->ctrl->dev, pled->ctrl->map,
					pled->ctrl->cfg->led_regfield(pled->port, pled->index));
	if (IS_ERR(field))
		return PTR_ERR(field);

	regfield_led_init(&pled->led, field, fwnode, pled->ctrl->cfg->modes);

	pled->led.commit = pled->ctrl->cfg->led_commit;
	pled->led.cdev.trigger_type = &switch_port_rtl_hw_trigger_type;
	pled->led.cdev.groups = rtl_hw_trigger_groups;

	init_data.fwnode = fwnode;

	return devm_led_classdev_register_ext(pled->ctrl->dev, &pled->led.cdev, &init_data);
}

static struct switch_port_led *switch_port_led_probe_single(
	struct switch_port_led_ctrl *ctrl, struct device_node *np)
{
	struct fwnode_handle *fwnode = of_fwnode_handle(np);
	struct switch_port_led *pled;
	unsigned int port_index;
	unsigned int led_index;
	const __be32 *addr;
	bool is_secondary;
	u32 trigger = 0;
	int err;

	addr = of_get_address(np, 0, NULL, NULL);
	if (!addr) {
		dev_warn(ctrl->dev, "failed to read led address\n");
		return ERR_PTR(-ENODEV);
	}

	port_index = of_read_number(addr, 1);
	led_index = of_read_number(addr + 1, 1);
	is_secondary = of_read_number(addr + 2, 1);

	if (port_index >= ctrl->cfg->port_count) {
		dev_warn(ctrl->dev, "invalid port number %d\n", port_index);
		return ERR_PTR(-ENODEV);
	}
	if (led_index >= ctrl->cfg->port_led_count) {
		dev_warn(ctrl->dev, "invalid LED index %d\n", led_index);
		return ERR_PTR(-ENODEV);
	}

	/* Even if LED is disabled, use it to count the number of used outputs. */
	if (!is_secondary)
		ctrl->available_leds[port_index].primary |= BIT(led_index);
	else
		ctrl->available_leds[port_index].secondary |= BIT(led_index);

	if (!fwnode_device_is_available(fwnode))
		return NULL;

	/*
	 * On Cypress and newer, secondary LEDs can be software controlled and
	 * have an independent hardware trigger. On Maple this is not possible.
	 * We should not register a classdev for secondary LEDs on Maple.
	 */
	if (is_secondary && !ctrl->cfg->independent_secondaries)
		return NULL;

	pled = devm_kzalloc(ctrl->dev, sizeof(*pled), GFP_KERNEL);
	if (!pled)
		return ERR_PTR(-ENOMEM);

	pled->ctrl = ctrl;
	pled->port = port_index;
	pled->index = led_index;
	pled->is_secondary = is_secondary;

	/* Optional trigger, matching on-device labeling */
	fwnode_property_read_u32(fwnode, "realtek,port-led-trigger", &trigger);
	if (ctrl->cfg->trigger_xlate(pled, trigger) < 0)
		dev_warn(ctrl->dev, "invalid LED trigger %x", trigger);
	else
		pled->trigger_flags = trigger;

	ctrl->cfg->set_hw_managed(pled, false);

	err = switch_port_register_classdev(pled, fwnode);
	if (err)
		return ERR_PTR(err);

	return pled;
}

static int realtek_port_led_probe(struct platform_device *pdev)
{
	struct switch_port_led_ctrl *ctrl;
	struct device *dev = &pdev->dev;
	struct device_node *np, *child;
	unsigned int member_map_count;
	enum rtl_led_output_mode mode;
	struct switch_port_led *pled;
	unsigned int i_grp, i_led;
	const char *mode_name;
	int err;

	np = dev->of_node;

	ctrl = devm_kzalloc(dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	mutex_init(&ctrl->lock);

	ctrl->dev = dev;
	ctrl->cfg = device_get_match_data(dev);
	ctrl->map = syscon_node_to_regmap(of_get_parent(np));
	if (IS_ERR_OR_NULL(ctrl->map))
		return dev_err_probe(dev, PTR_ERR(ctrl->map), "failed to find parent regmap\n");

	err = fwnode_property_read_string(dev_fwnode(dev), "realtek,output-mode", &mode_name);
	if (err)
		return dev_err_probe(dev, err, "failed to read realtek,output-mode\n");

	if (strcmp(mode_name, "serial") == 0)
		mode = RTL_LED_OUTPUT_SERIAL;
	else if (strcmp(mode_name, "single-color-scan") == 0)
		mode = RTL_LED_OUTPUT_SCAN_SINGLE;
	else if (strcmp(mode_name, "bi-color-scan") == 0)
		mode = RTL_LED_OUTPUT_SCAN_BICOLOR;
	else
		return dev_err_probe(dev, -EINVAL, "realtek,output-mode invalid\n");

	member_map_count = ctrl->cfg->port_led_count * ctrl->cfg->group_count;
	ctrl->groups = devm_kcalloc(dev, member_map_count, sizeof(*ctrl->groups), GFP_KERNEL);
	if (!ctrl->groups)
		return -ENOMEM;

	ctrl->available_leds = devm_kcalloc(dev, ctrl->cfg->port_count,
					    sizeof(*ctrl->available_leds), GFP_KERNEL);
	if (!ctrl->available_leds)
		return -ENOMEM;

	for (i_grp = 0; i_grp < ctrl->cfg->group_count; i_grp++) {
		for (i_led = 0; i_led < ctrl->cfg->port_led_count; i_led++) {
			struct reg_field group_field = ctrl->cfg->group_regfield(i_grp, i_led);
			struct led_port_group *group;

			group = &ctrl->groups[GROUP_LIST_INDEX(ctrl->cfg, i_grp, i_led)];
			group->index = i_grp;
			group->size = ctrl->cfg->port_count;
			group->setting = devm_regmap_field_alloc(dev, ctrl->map, group_field);
			if (!group->setting)
				return -ENOMEM;

			group->ports = devm_bitmap_zalloc(dev, ctrl->cfg->port_count, GFP_KERNEL);
			if (!group->ports)
				return -ENOMEM;
		}
	}

	err = devm_led_trigger_register(dev, &switch_port_rtl_hw_trigger);
	if (err)
		return dev_err_probe(dev, err, "failed to register private trigger");

	for_each_child_of_node(np, child) {
		if (of_n_addr_cells(child) != 3) {
			of_node_put(child);
			return dev_err_probe(dev, -EINVAL, "#address-cells (%d) is not 3\n",
					     (u32) of_n_addr_cells(child));
		}

		if (of_n_size_cells(child) != 0) {
			of_node_put(child);
			return dev_err_probe(dev, -EINVAL, "#size-cells (%d) is not 0\n",
					     (u32) of_n_size_cells(child));
		}

		if (!of_node_name_eq(child, "led")) {
			dev_dbg(dev, "skipping unsupported node %s\n", of_node_full_name(child));
			continue;
		}

		pled = switch_port_led_probe_single(ctrl, child);
		if (IS_ERR(pled))
			dev_warn(dev, "failed to register led: %ld\n", PTR_ERR(pled));
	}

	return ctrl->cfg->init(ctrl, mode);
}

static const struct of_device_id of_switch_port_led_match[] = {
	{
		.compatible = "realtek,maple-port-led",
		.data = &rtl838x_port_led_config,
	},
	{
		.compatible = "realtek,cypress-port-led",
		.data = &rtl839x_port_led_config,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_switch_port_led_match);

static struct platform_driver realtek_switch_port_led_driver = {
	.probe = realtek_port_led_probe,
	.driver = {
		.name = "realtek-switch-port-led",
		.of_match_table = of_switch_port_led_match,
	}
};
module_platform_driver(realtek_switch_port_led_driver);

MODULE_AUTHOR("Sander Vanheule <sander@svanheule.net>");
MODULE_DESCRIPTION("Realtek SoC switch port LED driver");
MODULE_LICENSE("GPL v2");
