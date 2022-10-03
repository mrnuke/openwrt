// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/mfd/core.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

struct realtek_switchcore_ctrl;

struct realtek_switchcore_data {
	const struct mfd_cell *mfd_devices;
	unsigned int mfd_device_count;
	void (*probe_model_name)(const struct realtek_switchcore_ctrl *ctrl);
};

struct realtek_switchcore_ctrl {
	struct device *dev;
	struct regmap *map;
	const struct realtek_switchcore_data *data;
};

/*
 * Model name probe
 *
 * Reads the family-specific MODEL_NAME_INFO register
 * to identify the SoC model and revision
 */
#define MODEL_NAME_CHAR_XLATE(val)	((val) ? 'A' + (val) - 1 : '\0')

#define RTL83XX_MODEL_NAME_ID		GENMASK(31, 16)
#define RTL83XX_MODEL_NAME_CHAR		GENMASK(15, 11)

#define RTL83XX_CHIP_INFO_UNLOCK	GENMASK(31, 28)
#define RTL83XX_CHIP_INFO_UNLOCK_CODE	0xa
#define RTL83XX_CHIP_INFO_CHIP_REV	GENMASK(20, 16)
#define RTL83XX_CHIP_INFO_RLID		GENMASK(15, 0)

/* Maple registers */
#define RTL838X_REG_MODEL_NAME_INFO	0x00d4
#define RTL838X_REG_CHIP_INFO		0x00d8

#define RTL838X_REG_INT_RW_CTRL		0x0058
#define RTL838X_REG_MODE_DEFINE_CTL	0x1024

/* Cypress registers */
#define RTL839X_REG_MODEL_NAME_INFO	0x0ff0
#define RTL839X_REG_CHIP_INFO		0x0ff4

static void rtl83xx_read_chip_name(struct regmap *map, unsigned int reg,
				   unsigned int *model_id, char *model_first_char)
{
	u32 val = 0;

	regmap_read(map, reg, &val);
	*model_id = FIELD_GET(RTL83XX_MODEL_NAME_ID, val);
	*model_first_char = MODEL_NAME_CHAR_XLATE(FIELD_GET(RTL83XX_MODEL_NAME_CHAR, val));
}

static void rtl83xx_read_chip_info(struct regmap *map, unsigned int reg,
				   unsigned int *chip_rev, unsigned int *rl_id)
{
	u32 val = 0;

	val = FIELD_PREP(RTL83XX_CHIP_INFO_UNLOCK, RTL83XX_CHIP_INFO_UNLOCK_CODE);
	regmap_write(map, reg, val);

	regmap_read(map, reg, &val);
	*chip_rev = FIELD_GET(RTL83XX_CHIP_INFO_CHIP_REV, val);
	*rl_id = FIELD_GET(RTL83XX_CHIP_INFO_RLID, val);
}

static void rtl_swcore_chip_print(struct device *dev, unsigned int model_id, char model_suffix,
				  unsigned int chip_rev, unsigned int rl_id)
{
	dev_info(dev, "found RTL%04x%c rev. %c, RL:%04x\n",
		 model_id, model_suffix, 'A' + (char) chip_rev, rl_id);
}

static void rtl838x_probe_model_name(const struct realtek_switchcore_ctrl *ctrl)
{
	unsigned int model_id = 0;
	unsigned int chip_rev = 0;
	char model_suffix = ' ';
	unsigned int rl_id = 0;
	u32 val = 0;

	/*
	 * CHIP_INFO register requires global register lock to be disabled.
	 * Leave the lock disabled as a side effect, to allow other switch core R/W operations.
	 */
	regmap_write(ctrl->map, RTL838X_REG_INT_RW_CTRL, 0x3);

	rtl83xx_read_chip_name(ctrl->map, RTL838X_REG_MODEL_NAME_INFO, &model_id, &model_suffix);

	if (model_id == 0x8380) {
		regmap_read(ctrl->map, RTL838X_REG_MODE_DEFINE_CTL, &val);
		/*
		 * Undocumented bit which is only set on RTL8380M.
		 * Possibly related to the presence of QSGMII ports for external phy.
		 */
		if (!(val & BIT(23)))
			model_id = 0x8381;
	}

	rtl83xx_read_chip_info(ctrl->map, RTL838X_REG_CHIP_INFO, &chip_rev, &rl_id);
	/* 'A' cut is denoted by revision 1, etc. Non-838x revisions start at 0. */
	if (chip_rev > 0)
		chip_rev--;

	rtl_swcore_chip_print(ctrl->dev, model_id, model_suffix, chip_rev, rl_id);
}

static void rtl839x_probe_model_name(const struct realtek_switchcore_ctrl *ctrl)
{
	unsigned int model_id = 0;
	unsigned int chip_rev = 0;
	char model_suffix = ' ';
	unsigned int rl_id = 0;

	rtl83xx_read_chip_name(ctrl->map, RTL839X_REG_MODEL_NAME_INFO, &model_id, &model_suffix);
	rtl83xx_read_chip_info(ctrl->map, RTL839X_REG_CHIP_INFO, &chip_rev, &rl_id);
	rtl_swcore_chip_print(ctrl->dev, model_id, model_suffix, chip_rev, rl_id);
}

static const struct mfd_cell rtl838x_mfd_devices[] = {
	{
		.name = "realtek-switchcore-sys-led",
		.of_compatible = "realtek,maple-sys-led",
	},
	{
		.name = "realtek-switchcore-port-leds",
		.of_compatible = "realtek,maple-port-led",
	},
	{
		.name = "realtek-switchcore-pinctrl",
		.of_compatible = "realtek,maple-pinctrl",
	},
};

static const struct realtek_switchcore_data rtl838x_switchcore_data = {
	.mfd_devices = rtl838x_mfd_devices,
	.mfd_device_count = ARRAY_SIZE(rtl838x_mfd_devices),
	.probe_model_name = rtl838x_probe_model_name,
};

static const struct mfd_cell rtl839x_mfd_devices[] = {
	{
		.name = "realtek-switchcore-sys-led",
		.of_compatible = "realtek,cypress-sys-led",
	},
	{
		.name = "realtek-switchcore-port-leds",
		.of_compatible = "realtek,cypress-port-led",
	},
	{
		.name = "realtek-switchcore-pinctrl",
		.of_compatible = "realtek,cypress-pinctrl",
	},
};

static const struct realtek_switchcore_data rtl839x_switchcore_data = {
	.mfd_devices = rtl839x_mfd_devices,
	.mfd_device_count = ARRAY_SIZE(rtl839x_mfd_devices),
	.probe_model_name = rtl839x_probe_model_name,
};

static const struct of_device_id of_realtek_switchcore_match[] = {
	{
		.compatible = "realtek,maple-switchcore",
		.data = &rtl838x_switchcore_data,
	},
	{
		.compatible = "realtek,cypress-switchcore",
		.data = &rtl839x_switchcore_data,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_realtek_switchcore_match);

static int realtek_switchcore_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct realtek_switchcore_ctrl *ctrl;

	ctrl = devm_kzalloc(dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	ctrl->dev = dev;
	ctrl->data = device_get_match_data(dev);
	ctrl->map = syscon_node_to_regmap(dev->of_node);
	if (!ctrl->map)
		return dev_err_probe(dev, -ENXIO, "failed to get regmap\n");

	ctrl->data->probe_model_name(ctrl);

	return mfd_add_devices(dev, 0, ctrl->data->mfd_devices,
			       ctrl->data->mfd_device_count, NULL, 0, NULL);
}

static struct platform_driver realtek_switchcore_driver = {
	.probe = realtek_switchcore_probe,
	.driver = {
		.name = "realtek-switchcore",
		.of_match_table = of_realtek_switchcore_match
	}
};
module_platform_driver(realtek_switchcore_driver);

MODULE_AUTHOR("Sander Vanheule <sander@svanheule.net>");
MODULE_DESCRIPTION("Realtek SoC switch core driver");
MODULE_LICENSE("GPL v2");
