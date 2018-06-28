// SPDX-License-Identifier: GPL-2.0+
// Copyright 2018 IBM Corp.

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

struct bmc_ctrl {
	struct device *dev;
	struct regmap *map;
	u32 mask;
	u32 shift;
	bool ro;
	const char *label;
	struct kobj_attribute label_attr;
	union {
		struct {
			u32 value;
			struct kobj_attribute value_attr;
		};
		struct {
			u32 read;
			u32 set;
			u32 clear;
			struct kobj_attribute read_attr;
			struct kobj_attribute set_attr;
			struct kobj_attribute clear_attr;
		};
	};
};

static ssize_t bmc_misc_rmw_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	struct bmc_ctrl *ctrl;
	unsigned int val;
	int rc;

	ctrl = container_of(attr, struct bmc_ctrl, value_attr);
	rc = regmap_read(ctrl->map, ctrl->value, &val);
	if (rc)
		return rc;

	val = (val & ctrl->mask) >> ctrl->shift;

	return sprintf(buf, "%u\n", val);
}

static ssize_t bmc_misc_rmw_store(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 const char *buf, size_t count)
{
	struct bmc_ctrl *ctrl;
	long val;
	int rc;

	rc = kstrtol(buf, 0, &val);
	if (rc)
		return rc;

	ctrl = container_of(attr, struct bmc_ctrl, value_attr);
	val <<= ctrl->shift;
	rc = regmap_update_bits(ctrl->map, ctrl->value, ctrl->mask, val);

	return rc < 0 ? rc : count;
}

static void bmc_misc_add_rmw_attrs(struct bmc_ctrl *ctrl,
				   struct attribute *attrs[5])
{
	sysfs_attr_init(&ctrl->attr.attr);
	ctrl->value_attr.attr.name = "value";
	ctrl->value_attr.attr.mode = 0664;
	ctrl->value_attr.show = bmc_misc_rmw_show;
	ctrl->value_attr.store = bmc_misc_rmw_store;
	attrs[1] = &ctrl->value_attr.attr;
	attrs[2] = NULL;
}

static int bmc_misc_init_rmw(struct bmc_ctrl *ctrl, struct device_node *node,
			     struct attribute *attrs[5])
{
	u32 val;
	int rc;

	rc = of_property_read_u32(node, "offset", &ctrl->value);
	if (rc < 0)
		return rc;

	if (!of_property_read_u32(node, "default-value", &val)) {
		val = (val << ctrl->shift) & ctrl->mask;
		regmap_update_bits(ctrl->map, ctrl->value, ctrl->mask, val);
	}

	bmc_misc_add_rmw_attrs(ctrl, attrs);

	return 0;
}

static ssize_t bmc_misc_sc_read_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	struct bmc_ctrl *ctrl;
	unsigned int val;
	int rc;

	ctrl = container_of(attr, struct bmc_ctrl, read_attr);
	rc = regmap_read(ctrl->map, ctrl->read, &val);
	if (rc)
		return rc;

	val = (val & ctrl->mask) >> ctrl->shift;

	return sprintf(buf, "%u\n", val);

}

static ssize_t bmc_misc_sc_set_store(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    const char *buf, size_t count)
{
	struct bmc_ctrl *ctrl;
	long val;
	int rc;

	rc = kstrtol(buf, 0, &val);
	if (rc)
		return rc;

	ctrl = container_of(attr, struct bmc_ctrl, set_attr);
	val = (val << ctrl->shift) & ctrl->mask;
	rc = regmap_write(ctrl->map, ctrl->set, val);

	return rc < 0 ? rc : count;
}

static ssize_t bmc_misc_sc_clear_store(struct kobject *kobj,
					      struct kobj_attribute *attr,
					      const char *buf, size_t count)
{
	struct bmc_ctrl *ctrl;
	long val;
	int rc;

	rc = kstrtol(buf, 0, &val);
	if (rc)
		return rc;

	ctrl = container_of(attr, struct bmc_ctrl, clear_attr);
	val = (val << ctrl->shift) & ctrl->mask;
	rc = regmap_write(ctrl->map, ctrl->clear, val);

	return rc < 0 ? rc : count;
}

static void bmc_misc_add_sc_attrs(struct bmc_ctrl *ctrl,
				  struct attribute *attrs[5])
{
	sysfs_attr_init(&ctrl->read_attr.attr);
	ctrl->read_attr.attr.name = "value";
	ctrl->read_attr.attr.mode = 0444;
	ctrl->read_attr.show = bmc_misc_sc_read_show;
	attrs[1] = &ctrl->read_attr.attr;

	sysfs_attr_init(&ctrl->set_attr.attr);
	ctrl->set_attr.attr.name = "set";
	ctrl->set_attr.attr.mode = 0200;
	ctrl->set_attr.store = bmc_misc_sc_set_store;
	attrs[2] = &ctrl->set_attr.attr;

	sysfs_attr_init(&ctrl->clear_attr.attr);
	ctrl->clear_attr.attr.name = "clear";
	ctrl->clear_attr.attr.mode = 0200;
	ctrl->clear_attr.store = bmc_misc_sc_clear_store;
	attrs[3] = &ctrl->clear_attr.attr;

	attrs[4] = NULL;
}

static int bmc_misc_init_sc(struct bmc_ctrl *ctrl, struct device_node *node,
			    struct attribute *attrs[5])
{
	int rc;

	rc = of_property_read_u32_array(node, "offset", &ctrl->read, 3);
	if (rc < 0)
		return rc;

	if (of_property_read_bool(node, "default-set"))
		regmap_write(ctrl->map, ctrl->set, ctrl->mask);
	else if (of_property_read_bool(node, "default-clear"))
		regmap_write(ctrl->map, ctrl->clear, ctrl->mask);

	bmc_misc_add_sc_attrs(ctrl, attrs);

	return 0;
}

static ssize_t bmc_misc_label_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	struct bmc_ctrl *ctrl = container_of(attr, struct bmc_ctrl, label_attr);

	return sprintf(buf, "%s\n", ctrl->label);
}

static int bmc_misc_init_dt(struct bmc_ctrl *ctrl, struct device_node *node,
			    struct attribute *attrs[5])
{
	int rc;

	/* Example children:
	 *
	 * ilpc2ahb_disable {
	 *      compatible = "bmc-misc-control";
	 *      reg = <0x80>;
	 *      bit-mask = <0x1>;
	 *      bit-shift = <6>;
	 *      label = "ilpc2ahb-disable";
	 * };
	 *
	 * lpc_sio_decode_disable {
	 *      compatible = "bmc-misc-control";
	 *      // Write-1-set/Write-1-clear semantics
	 *      set-clear;
	 *      default-set;
	 *      reg = <0x70 0x70 0x7c>
	 *      bit-mask = <0x1>;
	 *      bit-shift = <6>;
	 *      label = "lpc-sio-decode-disable";
	 * };
	 *
	 * vga_scratch_1 {
	 *	compatible = "bmc-misc-control";
	 *	read-only;
	 *	reg = <0x50>;
	 *	bit-mask = <0xffffffff>;
	 *	bit-shift = <0>;
	 *	label = "vga-scratch-1";
	 * };
	 */
	rc = of_property_read_u32(node, "mask", &ctrl->mask);
	if (rc < 0)
		return rc;

	ctrl->shift = __ffs(ctrl->mask);
	ctrl->ro = of_property_read_bool(node, "read-only");

	rc = of_property_read_string(node, "label", &ctrl->label);
	if (rc < 0)
		return rc;

	ctrl->label_attr.attr.name = "label";
	ctrl->label_attr.attr.mode = 0444;
	ctrl->label_attr.show = bmc_misc_label_show;
	attrs[0] = &ctrl->label_attr.attr;

	if (of_property_read_bool(node, "set-clear"))
		return bmc_misc_init_sc(ctrl, node, attrs);

	return bmc_misc_init_rmw(ctrl, node, attrs);
}

static struct class bmc_class = {
	.name =		"bmc",
};

static int bmc_misc_probe(struct platform_device *pdev)
{
	struct attribute *attrs[5];
	struct attribute **attr;
	struct bmc_ctrl *ctrl;
	int rc;

	ctrl = devm_kzalloc(&pdev->dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	ctrl->map = syscon_node_to_regmap(pdev->dev.parent->of_node);

	rc = bmc_misc_init_dt(ctrl, pdev->dev.of_node, attrs);
	if (rc < 0)
		return rc;

	ctrl->dev = device_create(&bmc_class, &pdev->dev, 0, ctrl, "%s",
			     ctrl->label);
	if (IS_ERR(ctrl->dev))
		return PTR_ERR(ctrl->dev);

	for (attr = &attrs[0]; *attr; attr++) {
		rc = sysfs_create_file(&ctrl->dev->kobj, *attr);
		if (rc < 0)
			/* FIXME: Cleanup */
			return rc;
	}

	return 0;
}

static const struct of_device_id bmc_misc_match[] = {
	{ .compatible = "bmc-misc-ctrl" },
	{ },
};

static struct platform_driver bmc_misc = {
	.driver = {
		.name		=  "bmc-misc-ctrl",
		.of_match_table = bmc_misc_match,
	},
	.probe = bmc_misc_probe,
};

static int bmc_misc_ctrl_init(void)
{
	int rc;

	rc = class_register(&bmc_class);
	if (rc < 0)
		return rc;

	return platform_driver_register(&bmc_misc);
}
module_init(bmc_misc_ctrl_init);

static void bmc_misc_ctrl_exit(void)
{
	platform_driver_unregister(&bmc_misc);
	class_unregister(&bmc_class);
}
module_exit(bmc_misc_ctrl_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andrew Jeffery <andrew@aj.id.au>");
