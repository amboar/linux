// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) Aspeed
 * Copyright 2019 IBM Corp. Joel Stanley <joel@jms.id.au>
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_mdio.h>
#include <linux/phy.h>
#include <linux/platform_device.h>

/*
 * PHY control register
 */
#define FTGMAC100_PHYCR_MDC_CYCTHR_MASK 0x3f
#define FTGMAC100_PHYCR_MDC_CYCTHR(x)   ((x) & 0x3f)
#define FTGMAC100_PHYCR_PHYAD(x)        (((x) & 0x1f) << 16)
#define FTGMAC100_PHYCR_REGAD(x)        (((x) & 0x1f) << 21)
#define FTGMAC100_PHYCR_MIIRD           (1 << 26)
#define FTGMAC100_PHYCR_MIIWR           (1 << 27)

/*
 * PHY data register
 */
#define FTGMAC100_PHYDATA_MIIWDATA(x)           ((x) & 0xffff)
#define FTGMAC100_PHYDATA_MIIRDATA(phydata)     (((phydata) >> 16) & 0xffff)

#define PHYCR		0x0
#define PHYDATA		0x4

struct aspeed_mdio {
	void __iomem *base;
};

struct aspeed_mdio_config {
	int (*read)(struct mii_bus *bus, int phy_addr, int regnum);
	int (*write)(struct mii_bus *bus, int phy_addr, int regnum, u16 value);
};

/* G6 MDC/MDIO  */
#define ASPEED_G6_PHYCR_FIRE		BIT(31)
#define ASPEED_G6_PHYCR_ST_22		BIT(28)
#define ASPEED_G6_PHYCR_WRITE		BIT(26)
#define ASPEED_G6_PHYCR_READ		BIT(27)
#define ASPEED_G6_PHYCR_WDATA(x)	(x & 0xffff)
#define ASPEED_G6_PHYCR_PHYAD(x)	(((x) & 0x1f) << 21)
#define ASPEED_G6_PHYCR_REGAD(x)	(((x) & 0x1f) << 16)


/* G4/G5 "New" MDC/MDIO */
#define ASPEED_G5_PHYCR_FIRE		BIT(15)
#define ASPEED_G5_PHYCR_BUSY		BIT(15)
#define ASPEED_G5_PHYCR_ST_22		BIT(12)
#define ASPEED_G5_PHYCR_WRITE		BIT(10)
#define ASPEED_G5_PHYCR_READ		BIT(11)
#define ASPEED_G5_PHYCR_WDATA(x)	((x & 0xffff) << 16)
#define ASPEED_G5_PHYCR_PHYAD(x)	(((x) & 0x1f) << 5)
#define ASPEED_G5_PHYCR_REGAD(x)	((x) & 0x1f)

#define ASPEED_PHYDATA_MIIWDATA(x)		((x) & 0xffff)

static int aspeed_g6_mdiobus_read(struct mii_bus *bus, int phy_addr, int regnum)
{
	struct aspeed_mdio *priv = bus->priv;
	u32 phycr;
	int i;

	//Use New MDC and MDIO interface
	phycr = ASPEED_G6_PHYCR_FIRE | ASPEED_G6_PHYCR_ST_22 | ASPEED_G6_PHYCR_READ |
			ASPEED_G6_PHYCR_PHYAD(phy_addr) | // 20141114
			ASPEED_G6_PHYCR_REGAD(regnum); // 20141114

	iowrite32(phycr, priv->base + PHYCR);

	for (i = 0; i < 10; i++) {
		phycr = ioread32(priv->base + PHYCR);

		if ((phycr & ASPEED_G6_PHYCR_FIRE) == 0) {
			u32 reg = ioread32(priv->base + PHYDATA);
			return ASPEED_PHYDATA_MIIWDATA(reg);
		}

		mdelay(10);
	}

	dev_err(&bus->dev, "mdio g6 read timed out\n");
	return -EIO;

}

static int aspeed_g5_mdiobus_read(struct mii_bus *bus, int phy_addr, int regnum)
{
	struct aspeed_mdio *priv = bus->priv;
	u32 phycr;
	int i;

	//Use New MDC and MDIO interface
	phycr = ASPEED_G5_PHYCR_FIRE | ASPEED_G5_PHYCR_ST_22 | ASPEED_G5_PHYCR_READ |
			ASPEED_G5_PHYCR_PHYAD(phy_addr) | // 20141114
			ASPEED_G5_PHYCR_REGAD(regnum); // 20141114

	iowrite32(phycr, priv->base + PHYCR);

	for (i = 0; i < 10; i++) {
		phycr = ioread32(priv->base + PHYCR);

		if ((phycr & ASPEED_G5_PHYCR_FIRE) == 0) {
			u32 reg = ioread32(priv->base + PHYDATA);
			return ASPEED_PHYDATA_MIIWDATA(reg);
		}

		mdelay(10);
	}

	dev_err(&bus->dev, "mdio g5 read timed out\n");
	return -EIO;

}

static int ftgmac100_mdiobus_read(struct mii_bus *bus, int phy_addr, int regnum)
{
	struct aspeed_mdio *priv = bus->priv;
	u32 phycr;
	int i;

	phycr = ioread32(priv->base + PHYCR);

	/* preserve MDC cycle threshold */
	phycr &= FTGMAC100_PHYCR_MDC_CYCTHR_MASK;

	phycr |= FTGMAC100_PHYCR_PHYAD(phy_addr) |
		 FTGMAC100_PHYCR_REGAD(regnum) |
		 FTGMAC100_PHYCR_MIIRD;

	iowrite32(phycr, priv->base + PHYCR);

	for (i = 0; i < 10; i++) {
		phycr = ioread32(priv->base + PHYCR);

		if ((phycr & FTGMAC100_PHYCR_MIIRD) == 0) {
			u32 reg = ioread32(priv->base + PHYDATA);
			return ASPEED_PHYDATA_MIIWDATA(reg);
		}

		udelay(100);
	}

	dev_err(&bus->dev, "mdio read timed out\n");
	return -EIO;
}

static int aspeed_g6_mdiobus_write(struct mii_bus *bus, int phy_addr,
				   int regnum, u16 value)
{
	struct aspeed_mdio *priv = bus->priv;
	u32 phycr;
	int i;

	phycr = ASPEED_PHYDATA_MIIWDATA(value) |
			ASPEED_G6_PHYCR_FIRE | ASPEED_G6_PHYCR_ST_22 |
			ASPEED_G6_PHYCR_WRITE |
			ASPEED_G6_PHYCR_PHYAD(phy_addr) |
			ASPEED_G6_PHYCR_REGAD(regnum);

	iowrite32(phycr, priv->base + PHYCR);

	for (i = 0; i < 10; i++) {
		phycr = ioread32(priv->base + PHYCR);

		if ((phycr & ASPEED_G6_PHYCR_FIRE) == 0)
			return 0;

		mdelay(100);
	}

	dev_err(&bus->dev, "mdio g6 write timed out\n");
	return -EIO;

}

static int aspeed_g5_mdiobus_write(struct mii_bus *bus, int phy_addr,
				   int regnum, u16 value)
{
	struct aspeed_mdio *priv = bus->priv;
	u32 phycr;
	int i;

	phycr = ASPEED_PHYDATA_MIIWDATA(value) |
			ASPEED_G5_PHYCR_FIRE | ASPEED_G5_PHYCR_ST_22 |
			ASPEED_G5_PHYCR_WRITE |
			ASPEED_G5_PHYCR_PHYAD(phy_addr) |
			ASPEED_G5_PHYCR_REGAD(regnum);

	iowrite32(phycr, priv->base + PHYCR);

	for (i = 0; i < 10; i++) {
		phycr = ioread32(priv->base + PHYCR);

		if ((phycr & ASPEED_G5_PHYCR_FIRE) == 0)
			return 0;

		mdelay(100);
	}

	dev_err(&bus->dev, "mdio g5 write timed out\n");
	return -EIO;

}

static int ftgmac100_mdiobus_write(struct mii_bus *bus, int phy_addr,
				   int regnum, u16 value)
{
	struct aspeed_mdio *priv = bus->priv;
	u32 phycr;
	u32 data;
	int i;

	phycr = ioread32(priv->base + PHYCR);

	/* Preserve MDC cycle threshold */
	phycr &= FTGMAC100_PHYCR_MDC_CYCTHR_MASK;

	phycr |= FTGMAC100_PHYCR_PHYAD(phy_addr) |
		 FTGMAC100_PHYCR_REGAD(regnum) |
		 FTGMAC100_PHYCR_MIIWR;

	data = FTGMAC100_PHYDATA_MIIWDATA(value);

	iowrite32(data, priv->base + PHYDATA);
	iowrite32(phycr, priv->base + PHYCR);

	for (i = 0; i < 10; i++) {
		phycr = ioread32(priv->base + PHYCR);

		if ((phycr & FTGMAC100_PHYCR_MIIWR) == 0)
			return 0;

		udelay(100);
	}

	dev_err(&bus->dev, "mdio write timed out\n");
	return -EIO;
}

static const struct aspeed_mdio_config ast2600_config = {
	.read = aspeed_g6_mdiobus_read,
	.write = aspeed_g6_mdiobus_write,
};
static const struct aspeed_mdio_config ast2500_config = {
	.read = aspeed_g5_mdiobus_read,
	.write = aspeed_g5_mdiobus_write,
};
static const struct aspeed_mdio_config ftgmac100_config = {
	.read = ftgmac100_mdiobus_read,
	.write = ftgmac100_mdiobus_write,
};

static const struct of_device_id aspeed_mdio_ids[] = {
	{ .compatible = "aspeed,ast2400-mdio", .data = &ftgmac100_config },
	{ .compatible = "aspeed,ast2500-mdio", .data = &ast2500_config },
	{ .compatible = "aspeed,ast2600-mdio", .data = &ast2600_config },
	{ },
};
MODULE_DEVICE_TABLE(of, aspeed_mdio_ids);

static int aspeed_mdio_probe(struct platform_device *pdev)
{
	const struct aspeed_mdio_config *config;
	struct device_node *np = pdev->dev.of_node;
	struct aspeed_mdio *priv;
	struct resource *res;
	struct mii_bus *bus;
	int ret, i;

	bus = mdiobus_alloc_size(sizeof(*priv));
	if (!bus)
		return -ENOMEM;

	priv = bus->priv;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	bus->name = "ftgmac100_mdio";
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s-%d", pdev->name, pdev->id);
	bus->parent = &pdev->dev;

        config = of_device_get_match_data(&pdev->dev);
	bus->read = config->read;
	bus->write = config->write;

	for (i = 0; i < PHY_MAX_ADDR; i++)
		bus->irq[i] = PHY_POLL;

	ret = of_mdiobus_register(bus, np);
	if (ret) {
		dev_err(&bus->dev, "registration failed\n");
		goto free_mdio;
	}

	return 0;

free_mdio:
	mdiobus_free(bus);
	return ret;
}

static int aspeed_mdio_remove(struct platform_device *pdev)
{
	struct mii_bus *bus = platform_get_drvdata(pdev);

	mdiobus_unregister(bus);
	mdiobus_free(bus);

	return 0;
}

static struct platform_driver aspeed_mdio_driver = {
	.probe	= aspeed_mdio_probe,
	.remove	= aspeed_mdio_remove,
	.driver = {
		.name = "aspeed-mdio",
		.of_match_table = aspeed_mdio_ids,
	},
};
module_platform_driver(aspeed_mdio_driver);
