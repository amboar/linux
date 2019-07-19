// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2019 IBM Corp. */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/platform_device.h>

#define DRV_NAME "mdio-aspeed"

#define ASPEED_MDIO_CTRL		0x0
#define   ASPEED_MDIO_CTRL_FIRE		BIT(31)
#define   ASPEED_MDIO_CTRL_ST		BIT(28)
#define     ASPEED_MDIO_CTRL_ST_C22	0
#define     ASPEED_MDIO_CTRL_ST_C45	1
#define   ASPEED_MDIO_CTRL_OP		GENMASK(27, 26)
#define     MDIO_C22_OP_WRITE		0b01
#define     MDIO_C22_OP_READ		0b10
#define   ASPEED_MDIO_CTRL_PHYAD	GENMASK(25, 21)
#define   ASPEED_MDIO_CTRL_REGAD	GENMASK(20, 16)
#define   ASPEED_MDIO_CTRL_MIIWDATA	GENMASK(15, 0)

#define ASPEED_MDIO_DATA		0x4
#define   ASPEED_MDIO_DATA_MDC_THRES	GENMASK(31, 24)
#define   ASPEED_MDIO_DATA_MDIO_EDGE	BIT(23)
#define   ASPEED_MDIO_DATA_MDIO_LATCH	GENMASK(22, 20)
#define   ASPEED_MDIO_DATA_IDLE		BIT(16)
#define   ASPEED_MDIO_DATA_MIIRDATA	GENMASK(15, 0)

#define ASPEED_MDIO_RETRIES		10

struct aspeed_mdio {
	void __iomem *base;
};

static int aspeed_mdio_read(struct mii_bus *bus, int addr, int regnum)
{
	struct aspeed_mdio *ctx = bus->priv;
	u32 ctrl;
	int i;

	dev_info(&bus->dev, "%s: addr: %d, regnum: %d\n", __func__, addr,
		regnum);

	/* Just clause 22 for the moment */
	ctrl = ASPEED_MDIO_CTRL_FIRE
		| FIELD_PREP(ASPEED_MDIO_CTRL_ST, ASPEED_MDIO_CTRL_ST_C22)
		| FIELD_PREP(ASPEED_MDIO_CTRL_OP, MDIO_C22_OP_READ)
		| FIELD_PREP(ASPEED_MDIO_CTRL_PHYAD, addr)
		| FIELD_PREP(ASPEED_MDIO_CTRL_REGAD, regnum);

	iowrite32(ctrl, ctx->base + ASPEED_MDIO_CTRL);

	for (i = 0; i < ASPEED_MDIO_RETRIES; i++) {
		u32 data;

		data = ioread32(ctx->base + ASPEED_MDIO_DATA);
		if (data & ASPEED_MDIO_DATA_IDLE)
			return FIELD_GET(ASPEED_MDIO_DATA_MIIRDATA, data);

		udelay(100);
	}

	dev_err(&bus->dev, "MDIO read failed\n");
	return -EIO;
}

static int aspeed_mdio_write(struct mii_bus *bus, int addr, int regnum, u16 val)
{
	struct aspeed_mdio *ctx = bus->priv;
	u32 ctrl;
	int i;

	dev_info(&bus->dev, "%s: addr: %d, regnum: %d, val: 0x%x\n",
		__func__, addr, regnum, val);

	/* Just clause 22 for the moment */
	ctrl = ASPEED_MDIO_CTRL_FIRE
		| FIELD_PREP(ASPEED_MDIO_CTRL_ST, ASPEED_MDIO_CTRL_ST_C22)
		| FIELD_PREP(ASPEED_MDIO_CTRL_OP, MDIO_C22_OP_WRITE)
		| FIELD_PREP(ASPEED_MDIO_CTRL_PHYAD, addr)
		| FIELD_PREP(ASPEED_MDIO_CTRL_REGAD, regnum)
		| FIELD_PREP(ASPEED_MDIO_CTRL_MIIWDATA, val);

	iowrite32(ctrl, ctx->base + ASPEED_MDIO_CTRL);

	for (i = 0; i < ASPEED_MDIO_RETRIES; i++) {
		ctrl = ioread32(ctx->base + ASPEED_MDIO_CTRL);
		if (!(ctrl & ASPEED_MDIO_CTRL_FIRE))
			return 0;

		udelay(100);
	}

	dev_err(&bus->dev, "MDIO write failed\n");
	return -EIO;
}

static int aspeed_mdio_probe(struct platform_device *pdev)
{
	struct aspeed_mdio *ctx;
	struct mii_bus *bus;
	int rc;

	bus = devm_mdiobus_alloc_size(&pdev->dev, sizeof(*ctx));
	if (!bus)
		return -ENOMEM;

	ctx = bus->priv;
	ctx->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ctx->base))
		return PTR_ERR(ctx->base);

	bus->name = DRV_NAME;
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s-%d", pdev->name, pdev->id);
	bus->parent = &pdev->dev;
	bus->read = aspeed_mdio_read;
	bus->write = aspeed_mdio_write;

	rc = mdiobus_register(bus);
	if (rc) {
		dev_err(&pdev->dev, "Cannot register MDIO bus!\n");
		return rc;
	}

	platform_set_drvdata(pdev, bus);

	return 0;
}

static int aspeed_mdio_remove(struct platform_device *pdev)
{
	mdiobus_unregister(platform_get_drvdata(pdev));

	return 0;
}

static const struct of_device_id aspeed_mdio_of_match[] = {
	{ .compatible = "aspeed,ast2600-mdio", },
	{ },
};

static struct platform_driver aspeed_mdio_driver = {
	.driver = {
		.name = DRV_NAME,
		.of_match_table = aspeed_mdio_of_match,
	},
	.probe = aspeed_mdio_probe,
	.remove = aspeed_mdio_remove,
};

module_platform_driver(aspeed_mdio_driver);

MODULE_AUTHOR("Andrew Jeffery <andrew@aj.id.au>");
MODULE_LICENSE("GPLv2");
