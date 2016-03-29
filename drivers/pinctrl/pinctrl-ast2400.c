/*
 * Copyright (C) 2016 IBM
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <asm/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/types.h>
#include "core.h"

/* Overview
 * --------
 *
 * A pin on the AST2400 can have up to three functions:
 *
 * 1. A "high" priority function
 * 2. A "low" priority function
 * 3. A default function
 *
 * The functions are enabled by logic expressions over one or more bits in one
 * or more registers in the SCU, and some ports in the SuperIO controller.
 *
 * This is all rather complex and tedious, so a number of structs, functions
 * and macros are defined. Together these help reduce the tedium and line noise
 * so the definitions in the source can be more easily matched up with the
 * definitions in data sheet.
 *
 * The approach is to divide the problem roughly in two, into pin control
 * descriptors and pin function expressions, which are tied together in a
 * struct capturing the function priorities.  A pin control descriptor tells
 * the driver where and how to extract a value and what the expected value
 * should be (or not be). Multiple pin control descriptors can be combined
 * into a pin function expression. The expressions are limited in power to
 * what's required to implement the AST2400 pinctrl: They cannot be arbitrarily
 * compounded.  Instead, multiple descriptions can be chained with one type of
 * logical operator (func_expr_and, func_expr_or).  A pin's high and low
 * priority expressions are then captured in a pin_func_prio struct, and a
 * pointer to this is tucked into the pin's pinctrl subsystem registration.
 */

struct pin_ctrl_desc {
	bool (*op)(void __iomem *, struct pin_ctrl_desc *);
	unsigned reg;
	uint32_t mask;
	uint32_t val;
};

static bool pin_desc_eq(void __iomem *base, struct pin_ctrl_desc *desc)
{
	uint32_t val = ioread32(base + desc->reg) & desc->mask;
	return val == desc->val;
}

static bool pin_desc_neq(void __iomem *base, struct pin_ctrl_desc *desc)
{
	return !pin_desc_eq(base, desc);
}

struct pin_func_expr {
	const char *name;
	int ndescs;
	struct pin_ctrl_desc *descs;
	bool (*op)(void __iomem *, struct pin_func_expr *);
};

static bool func_expr_and(void __iomem *base, struct pin_func_expr *expr)
{
	bool ret = true;
	int i;

	for (i = 0; i < expr->ndescs; i++) {
		struct pin_ctrl_desc *desc = &expr->descs[i];
		ret &= desc->op(base, desc);
	}
	return ret;
}

static bool func_expr_or(void  __iomem *base, struct pin_func_expr *expr)
{
	bool ret = false;
	int i;

	for (i = 0; i < expr->ndescs; i++) {
		struct pin_ctrl_desc *desc = &expr->descs[i];
		ret |= desc->op(base, desc);
	}
	return ret;
}

struct pin_func_prio {
	const char *fallback;
	struct pin_func_expr *high;
	struct pin_func_expr *low;
};

/* Macro hell, better to see how they're used and work backwards */

/* "Internal" macros - consumed by other macros providing better abstractions */

#define CTRL_DESC_SYM(_ball, _prio) ctrl_desc_##_ball##_##_prio

#define CTRL_DESC_(_ball, _prio, ...) \
	static struct pin_ctrl_desc CTRL_DESC_SYM(_ball, _prio)[] = \
		{ __VA_ARGS__ }

#define PRIO_FUNC_SYM__(_ball, _prio) func_expr_##_ball##_##_prio
#define PRIO_FUNC_SYM(_ball, _prio) PRIO_FUNC_SYM__(_ball, _prio)

#define PRIO_FUNC_EXPR_(_ball, _name, _prio, _op) \
	static struct pin_func_expr PRIO_FUNC_SYM(_ball, _prio) = { \
		.name = _name, \
		.op = _op, \
		.ndescs = ARRAY_SIZE(CTRL_DESC_SYM(_ball, _prio)), \
		.descs = &(CTRL_DESC_SYM(_ball, _prio))[0], \
	}

#define BALL_SYM__(_ball) ball_##_ball
#define BALL_SYM(_ball) BALL_SYM__(_ball)

#define MF_PIN_(_ball, _fallback, _high, _low) \
	static struct pin_func_prio BALL_SYM(_ball) = { \
		.fallback = _fallback, \
		.high = _high, \
		.low = _low, \
	}

#define SF_PIN_EXPR_(_ball, _fallback, _name, _prio, _op, ...) \
	CTRL_DESC_(_ball, _prio, __VA_ARGS__); \
	PRIO_FUNC_EXPR_(_ball, _name, _prio, _op); \
	MF_PIN_(_ball, _fallback, &PRIO_FUNC_SYM(_ball, HIGH_PRIO), NULL)

#define SF_PIN_EXPR__(_ball, _fallback, _name, _prio, _op, ...) \
	SF_PIN_EXPR_(_ball, _fallback, _name, _prio, _op, __VA_ARGS__)

/* The non-internal macros */

#define HIGH_PRIO high
#define LOW_PRIO low

/* Initialise a pin control descriptor. */
#define CTRL_DESC(_op, _reg, _mask, _val) \
	{ .op = _op, .reg = _reg, .mask = _mask, .val = _val }

/* Initialise a pin control descriptor, checking for value equality */
#define CTRL_DESC_EQ(_reg, _mask, _val) \
	CTRL_DESC(pin_desc_eq, _reg, _mask, _val)

/* Initialise a pin control descriptor, checking for negated value equality */
#define CTRL_DESC_NEQ(_reg, _mask, _val) \
	CTRL_DESC(pin_desc_neq, _reg, _mask, _val)

#define PRIO_FUNC_EXPR(_ball, _name, _prio, _op, ...) \
	CTRL_DESC_(_ball, _prio, __VA_ARGS__); \
	PRIO_FUNC_EXPR_(_ball, _name, _prio, _op)

#define PRIO_FUNC(_ball, _name, _prio, ...) \
	CTRL_DESC_(_ball, _prio, __VA_ARGS__); \
	PRIO_FUNC_EXPR_(_ball, _name, _prio, NULL)

/* Multi-function pin, i.e. has both high and low priority pin functions. Need
 * to invoke PRIO_FUNC() or PRIO_FUNC_EXPR() for both HIGH_PRIO and
 * LOW_PRIO to define the expressions before invoking MF_PIN().
 * Failure to do so will give a compilation error. */
#define MF_PIN(_ball, _fallback) \
	MF_PIN_(_ball, _fallback, \
		       	&PRIO_FUNC_SYM(_ball, HIGH_PRIO), \
		       	&PRIO_FUNC_SYM(_ball, LOW_PRIO))

/* Single function pin, enabled by a multi-descriptor pin expression */
#define SF_PIN_EXPR(_ball, _fallback, _name, _op, ...) \
	SF_PIN_EXPR__(_ball, _fallback, _name, HIGH_PRIO, _op, __VA_ARGS__)

/* Single function pin, enabled by a single pin descriptor */
#define SF_PIN(_ball, _fallback, _name, ...) \
	SF_PIN_EXPR(_ball, _fallback, _name, NULL, __VA_ARGS__)

#define AST_PINCTRL_PIN(_number, _name) \
	{ .number = _number, .name = #_name, .drv_data = &(BALL_SYM(_name)) }

#define SCU3C 0x3C
#define SCU3C 0x3C
#define SCU70 0x70
#define STRAP 0x70
#define SCU80 0x80
#define SCU84 0x84
#define SCU88 0x88
#define SCU8C 0x8C
#define SCU90 0x90
#define SCU94 0x94

SF_PIN(D6, "GPIOA0", "MAC1LINK", CTRL_DESC_EQ(SCU80, BIT_MASK(0), 1));
SF_PIN(B5, "GPIOA1", "MAC2LINK", CTRL_DESC_EQ(SCU80, BIT_MASK(1), 1));
SF_PIN(A4, "GPIOA2", "TIMER3", CTRL_DESC_EQ(SCU80, BIT_MASK(2), 1));
SF_PIN(E6, "GPIOA3", "TIMER4", CTRL_DESC_EQ(SCU80, BIT_MASK(3), 1));

PRIO_FUNC(C5, "SCL9", HIGH_PRIO, CTRL_DESC_EQ(SCU90, BIT_MASK(22), 1));
PRIO_FUNC(C5, "TIMER5", LOW_PRIO, CTRL_DESC_EQ(SCU80, BIT_MASK(4), 1));
MF_PIN(C5, "GPIOA4");

PRIO_FUNC(B4, "SDA9", HIGH_PRIO, CTRL_DESC_EQ(SCU90, BIT_MASK(22), 1));
PRIO_FUNC(B4, "TIMER6", LOW_PRIO, CTRL_DESC_EQ(SCU80, BIT_MASK(5), 1));
MF_PIN(B4, "GPIOA5");

PRIO_FUNC(A3, "MDC2", HIGH_PRIO, CTRL_DESC_EQ(SCU90, BIT_MASK(2), 1));
PRIO_FUNC(A3, "TIMER7", LOW_PRIO, CTRL_DESC_EQ(SCU80, BIT_MASK(6), 1));
MF_PIN(A3, "GPIOA6");

PRIO_FUNC(D5, "MDIO2", HIGH_PRIO, CTRL_DESC_EQ(SCU90, BIT_MASK(2), 1));
PRIO_FUNC(D5, "TIMER8", LOW_PRIO, CTRL_DESC_EQ(SCU80, BIT_MASK(7), 1));
MF_PIN(D5, "GPIOA7");

SF_PIN(J21, "GPIOB0", "SALT1", CTRL_DESC_EQ(SCU80, BIT_MASK(8), 1));
SF_PIN(J20, "GPIOB1", "SALT2", CTRL_DESC_EQ(SCU80, BIT_MASK(9), 1));
SF_PIN(H18, "GPIOB2", "SALT3", CTRL_DESC_EQ(SCU80, BIT_MASK(10), 1));
SF_PIN(F18, "GPIOB3", "SALT4", CTRL_DESC_EQ(SCU80, BIT_MASK(11), 1));

SF_PIN_EXPR(E19, "GPIOB4", "LPCRST#", func_expr_or,
	       	CTRL_DESC_EQ(SCU80, BIT_MASK(12), 1),
		CTRL_DESC_EQ(STRAP, BIT_MASK(14), 1));

/* H19: Need magic for SIORD30
PRIO_FUNC_EXPR(H19, "LPCPD#", HIGH_PRIO,
		func_expr_and,
	       	CTRL_DESC_EQ(SCU8C, BIT_MASK(1), 1),
		CTRL_DESC_EQ(STRAP, BIT_MASK(21), 1));
PRIO_FUNC_EXPR(H19, "LPCSMI#", LOW_PRIO,
		func_expr_and,
	       	CTRL_DESC_EQ(SCU8C, BIT_MASK(1), 1),
		CTRL_DESC_EQ(STRAP, BIT_MASK(21), 1));
MF_PIN(H19, "GPIOB5");
*/
MF_PIN_(H19, "GPIOB5", NULL, NULL);

SF_PIN(H20, "GPIOB6", "LPCPME#", CTRL_DESC_EQ(SCU80, BIT_MASK(14), 1));

PRIO_FUNC_EXPR(E18, "EXTRST#", HIGH_PRIO,
	       	func_expr_and,
	       	CTRL_DESC_EQ(SCU80, BIT_MASK(15), 1),
		CTRL_DESC_EQ(SCU90, BIT_MASK(31), 0),
		CTRL_DESC_EQ(SCU3C, BIT_MASK(3), 1));
PRIO_FUNC_EXPR(E18, "SPICS1#", LOW_PRIO,
	       	func_expr_and,
	       	CTRL_DESC_EQ(SCU80, BIT_MASK(15), 1),
		CTRL_DESC_EQ(SCU90, BIT_MASK(31), 1));
MF_PIN(E18, "GPIOB7");

/*
PRIO_FUNC(A18, "SD2CLK", HIGH_PRIO, CTRL_DESC_EQ(SCU90, BIT_MASK(1), 1));
PRIO_FUNC_EXPR(A18, "GPID0(In)", LOW_PRIO,
	       	func_expr_or,
	       	CTRL_DESC_EQ(SCU8C, BIT_MASK(1), 1),
		CTRL_DESC_EQ(STRAP, BIT_MASK(21), 1));
MF_PIN(A18, "GPIOD0");
*/

struct ast2400_pin_function {
	const char *name;
	const char * const *groups;
	const unsigned ngroups;
};

static const struct pinctrl_pin_desc ast2400_pinctrl_pins[] = {
	AST_PINCTRL_PIN(0, D6),
	AST_PINCTRL_PIN(1, B5),
	AST_PINCTRL_PIN(2, A4),
	AST_PINCTRL_PIN(3, E6),
	AST_PINCTRL_PIN(4, C5),
	AST_PINCTRL_PIN(5, B4),
	AST_PINCTRL_PIN(6, A3),
	AST_PINCTRL_PIN(7, D5),
	AST_PINCTRL_PIN(8, J21),
	AST_PINCTRL_PIN(9, J20),
	AST_PINCTRL_PIN(10, H18),
	AST_PINCTRL_PIN(11, F18),
	AST_PINCTRL_PIN(12, E19),
	AST_PINCTRL_PIN(13, H19),
	AST_PINCTRL_PIN(14, H20),
	AST_PINCTRL_PIN(15, E18)
};

struct ast2400_pinctrl_data {
	void __iomem *reg_base;

	const struct pinctrl_pin_desc *pins;
	const unsigned npins;

	const struct ast2400_pin_function *functions;
	const unsigned nfunctions;
};

static struct ast2400_pinctrl_data ast2400_pinctrl = {
	.pins = ast2400_pinctrl_pins,
	.npins = ARRAY_SIZE(ast2400_pinctrl_pins),
	/*
	.functions = NULL,
	.nfunctions = ARRAY_SIZE(NULL),
	*/
};

static int ast2400_pinctrl_get_groups_count(struct pinctrl_dev *pctldev)
{
	struct ast2400_pinctrl_data *pdata = pinctrl_dev_get_drvdata(pctldev);

	return pdata->npins;
}

static const char *ast2400_pinctrl_get_group_name(struct pinctrl_dev *pctldev,
	       					  unsigned group)
{
	struct ast2400_pinctrl_data *pdata = pinctrl_dev_get_drvdata(pctldev);

	return pdata->pins[group].name;
}

static const int ast2400_pinctrl_get_group_pins(struct pinctrl_dev *pctldev,
					        unsigned group,
					        const unsigned **pins,
					        unsigned *num_pins)
{
	struct ast2400_pinctrl_data *pdata = pinctrl_dev_get_drvdata(pctldev);

	*pins = &pdata->pins[group].number;
	*num_pins = 1;

	return 0;
}

static struct pinctrl_ops ast2400_pinctrl_ops = {
	.get_groups_count = ast2400_pinctrl_get_groups_count,
	.get_group_name = ast2400_pinctrl_get_group_name,
	.get_group_pins = NULL,
	.pin_dbg_show = NULL,
	.dt_node_to_map = NULL,
	.dt_free_map = NULL,
};

static struct pinmux_ops ast2400_pinmux_ops = {
	.get_functions_count = NULL,
	.get_function_name = NULL,
	.get_function_groups = NULL,
	.set_mux = NULL,
};

static struct pinconf_ops ast2400_pinconf_ops = {
	.pin_config_get = NULL,
	.pin_config_set = NULL,
};

static struct pinctrl_desc ast2400_pinctrl_desc = {
	.pctlops = &ast2400_pinctrl_ops,
	.pmxops = &ast2400_pinmux_ops,
	.confops = &ast2400_pinconf_ops,
	.owner = THIS_MODULE,
};

static int __init ast2400_pinctrl_probe(struct platform_device *pdev)
{
	struct ast2400_pinctrl_data *pdata = &ast2400_pinctrl;
	struct resource *res;
	struct pinctrl_dev *pctl;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pdata->reg_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pdata->reg_base)) {
		dev_err(&pdev->dev, "Failed to ioremap MEM resource\n");
		return -ENODEV;
	}

	/* FIXME: do some dynamic stuff? See pinctrl-bcm281xx.c */

	ast2400_pinctrl_desc.name = dev_name(&pdev->dev);
	ast2400_pinctrl_desc.pins = ast2400_pinctrl.pins;
	ast2400_pinctrl_desc.npins = ast2400_pinctrl.npins;

	pctl = pinctrl_register(&ast2400_pinctrl_desc, &pdev->dev, pdata);

	if (IS_ERR(pctl)) {
		dev_err(&pdev->dev, "Failed to register pinctrl\n");
		return PTR_ERR(pctl);
	}
	
	platform_set_drvdata(pdev, pdata);

	return 0;
}

static const struct of_device_id ast2400_pinctrl_of_match[] = {
	{ .compatible = "aspeed,ast2400-pinctrl", },
	{ },
};

static struct platform_driver ast2400_pinctrl_driver = {
	.driver = {
		.name = "ast2400-pinctrl",
		.of_match_table = ast2400_pinctrl_of_match,
	},
};

module_platform_driver_probe(ast2400_pinctrl_driver, ast2400_pinctrl_probe);

MODULE_AUTHOR("Andrew Jeffery <andrew@aj.id.au>");
MODULE_DESCRIPTION("ASPEED AST2400 pinctrl driver");
MODULE_LICENSE("GPL v2");
