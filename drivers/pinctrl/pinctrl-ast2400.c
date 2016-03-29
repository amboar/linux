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
 * descriptors and pin function expressions which are tied together in a
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
	const char *ball;
	struct pin_func_expr *high;
	struct pin_func_expr *low;
};

/* Macro hell, better to see how they're used and work backwards */

/* "Internal" macros - consumed by other macros providing better abstractions */

#define AST_CTRL_DESC_SYM(_ball, _prio) ctrl_desc_##_ball##_##_prio

#define AST_CTRL_DESC_(_ball, _prio, ...) \
	static struct pin_ctrl_desc AST_CTRL_DESC_SYM(_ball, _prio)[] = \
		{ __VA_ARGS__ }

#define AST_FUNC_EXPR_SYM__(_ball, _prio) pin_expr_##_ball##_##_prio
#define AST_FUNC_EXPR_SYM(_ball, _prio) AST_FUNC_EXPR_SYM__(_ball, _prio)

#define AST_FUNC_EXPR_OP_(_ball, _name, _prio, _op) \
	static struct pin_func_expr AST_FUNC_EXPR_SYM(_ball, _prio) = { \
		.name = _name, \
		.op = _op, \
		.ndescs = ARRAY_SIZE(AST_CTRL_DESC_SYM(_ball, _prio)), \
		.descs = &(AST_CTRL_DESC_SYM(_ball, _prio))[0], \
	}

#define AST_BALL_SYM__(_ball) ball_##_ball
#define AST_BALL_SYM(_ball) AST_BALL_SYM__(_ball)

#define AST_PIN_MF_(_ball, _high, _low) \
	static struct pin_func_prio AST_BALL_SYM(_ball) = \
		{ .ball = #_ball, .high = _high, .low = _low, }

#define AST_PIN_SF_OP_(_ball, _name, _prio, _op, ...) \
	AST_CTRL_DESC_(_ball, _prio, __VA_ARGS__); \
	AST_FUNC_EXPR_OP_(_ball, _name, _prio, _op); \
	AST_PIN_MF_(_ball, &AST_FUNC_EXPR_SYM(_ball, CTRL_HIGH_PRIO), NULL)

#define AST_PIN_SF_OP__(_ball, _name, _prio, _op, ...) \
	AST_PIN_SF_OP_(_ball, _name, _prio, _op, __VA_ARGS__)

/* The non-internal macros */

#define CTRL_HIGH_PRIO high
#define CTRL_LOW_PRIO low

/* Initialise a pin control descriptor. */
#define AST_CTRL_DESC(_op, _reg, _mask, _val) \
	{ .op = _op, .reg = _reg, .mask = _mask, .val = _val }

/* Initialise a pin control descriptor, checking for value equality */
#define AST_CTRL_DESC_EQ(_reg, _mask, _val) \
	AST_CTRL_DESC(pin_desc_eq, _reg, _mask, _val)

/* Initialise a pin control descriptor, checking for negated value equality */
#define AST_CTRL_DESC_NEQ(_reg, _mask, _val) \
	AST_CTRL_DESC(pin_desc_neq, _reg, _mask, _val)


#define AST_FUNC_EXPR_OP(_ball, _name, _prio, _op, ...) \
	AST_CTRL_DESC_(_ball, _prio, __VA_ARGS__); \
	AST_FUNC_EXPR_OP_(_ball, _name, _prio, _op)

#define AST_FUNC_EXPR(_ball, _name, _prio, ...) \
	AST_CTRL_DESC_(_ball, _prio, __VA_ARGS__); \
	AST_FUNC_EXPR_OP_(_ball, _name, _prio, NULL)

/* Multi-function pin, i.e. has both high and low priority pin functions. Need
 * to invoke AST_FUNC_EXPR() or AST_FUNC_EXPR_OP() for both CTRL_HIGH_PRIO and
 * CTRL_LOW_PRIO to define the expressions before invoking AST_PIN_MF().
 * Failure to do so will give a compilation error. */
#define AST_PIN_MF(_ball) \
	AST_PIN_MF_(_ball, &AST_FUNC_EXPR_SYM(_ball, CTRL_HIGH_PRIO), \
		       	&AST_FUNC_EXPR_SYM(_ball, CTRL_LOW_PRIO))

/* Single function pin, enabled by a multi-element pin expression */
#define AST_PIN_SF_OP(_ball, _name, _op, ...) \
	AST_PIN_SF_OP__(_ball, _name, CTRL_HIGH_PRIO, _op, __VA_ARGS__)

/* Single function pin, enabled by a simple pin description */
#define AST_PIN_SF(_ball, _name, ...) \
	AST_PIN_SF_OP(_ball, _name, NULL, __VA_ARGS__)

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

AST_PIN_SF(D6, "MAC1LINK", AST_CTRL_DESC_EQ(SCU80, BIT_MASK(0), 1));
AST_PIN_SF(B5, "MAC2LINK", AST_CTRL_DESC_EQ(SCU80, BIT_MASK(1), 1));
AST_PIN_SF(A4, "TIMER3", AST_CTRL_DESC_EQ(SCU80, BIT_MASK(2), 1));
AST_PIN_SF(E6, "TIMER4", AST_CTRL_DESC_EQ(SCU80, BIT_MASK(3), 1));

AST_FUNC_EXPR(C5, "SCL9", CTRL_HIGH_PRIO,
	       	AST_CTRL_DESC_EQ(SCU90, BIT_MASK(22), 1));
AST_FUNC_EXPR(C5, "TIMER5", CTRL_LOW_PRIO,
		AST_CTRL_DESC_EQ(SCU80, BIT_MASK(4), 1));
AST_PIN_MF(C5);

AST_FUNC_EXPR(B4, "SDA9", CTRL_HIGH_PRIO,
	       	AST_CTRL_DESC_EQ(SCU90, BIT_MASK(22), 1));
AST_FUNC_EXPR(B4, "TIMER6", CTRL_LOW_PRIO,
	       	AST_CTRL_DESC_EQ(SCU80, BIT_MASK(5), 1));
AST_PIN_MF(B4);

AST_FUNC_EXPR(A3, "MDC2", CTRL_HIGH_PRIO,
	       	AST_CTRL_DESC_EQ(SCU90, BIT_MASK(2), 1));
AST_FUNC_EXPR(A3, "TIMER7", CTRL_LOW_PRIO,
	       	AST_CTRL_DESC_EQ(SCU80, BIT_MASK(6), 1));
AST_PIN_MF(A3);

AST_FUNC_EXPR(D5, "MDIO2", CTRL_HIGH_PRIO,
	       	AST_CTRL_DESC_EQ(SCU90, BIT_MASK(2), 1));
AST_FUNC_EXPR(D5, "TIMER8", CTRL_LOW_PRIO,
	       	AST_CTRL_DESC_EQ(SCU80, BIT_MASK(7), 1));
AST_PIN_MF(D5);

AST_PIN_SF(J21, "SALT1", AST_CTRL_DESC_EQ(SCU80, BIT_MASK(8), 1));
AST_PIN_SF(J20, "SALT2", AST_CTRL_DESC_EQ(SCU80, BIT_MASK(9), 1));
AST_PIN_SF(H18, "SALT3", AST_CTRL_DESC_EQ(SCU80, BIT_MASK(10), 1));
AST_PIN_SF(F18, "SALT4", AST_CTRL_DESC_EQ(SCU80, BIT_MASK(11), 1));

AST_PIN_SF_OP(E19, "LPCRST#", func_expr_or,
	       	AST_CTRL_DESC_EQ(SCU80, BIT_MASK(12), 1),
		AST_CTRL_DESC_EQ(STRAP, BIT_MASK(14), 1));

/* H19: Need magic for SIORD30
AST_FUNC_EXPR_OP(H19, "LPCPD#", CTRL_HIGH_PRIO,
		func_expr_and,
	       	AST_CTRL_DESC_EQ(SCU8C, BIT_MASK(1), 1),
		AST_CTRL_DESC_EQ(STRAP, BIT_MASK(21), 1));
AST_FUNC_EXPR_OP(H19, "LPCSMI#", CTRL_LOW_PRIO,
		func_expr_and,
	       	AST_CTRL_DESC_EQ(SCU8C, BIT_MASK(1), 1),
		AST_CTRL_DESC_EQ(STRAP, BIT_MASK(21), 1));
AST_PIN_MF(H19);
*/

AST_PIN_SF(H20, "LPCPME#", AST_CTRL_DESC_EQ(SCU80, BIT_MASK(14), 1));

AST_FUNC_EXPR_OP(E18, "EXTRST#", CTRL_HIGH_PRIO,
	       	func_expr_and,
	       	AST_CTRL_DESC_EQ(SCU80, BIT_MASK(15), 1),
		AST_CTRL_DESC_EQ(SCU90, BIT_MASK(31), 0),
		AST_CTRL_DESC_EQ(SCU3C, BIT_MASK(3), 1));
AST_FUNC_EXPR_OP(E18, "SPICS1#", CTRL_LOW_PRIO,
	       	func_expr_and,
	       	AST_CTRL_DESC_EQ(SCU80, BIT_MASK(15), 1),
		AST_CTRL_DESC_EQ(SCU90, BIT_MASK(31), 1));
AST_PIN_MF(E18);

AST_FUNC_EXPR(A18, "SD2CLK", CTRL_HIGH_PRIO, AST_CTRL_DESC_EQ(SCU90, BIT_MASK(1), 1));
AST_FUNC_EXPR_OP(A18, "GPID0(In)", CTRL_LOW_PRIO,
	       	func_expr_or,
	       	AST_CTRL_DESC_EQ(SCU8C, BIT_MASK(1), 1),
		AST_CTRL_DESC_EQ(STRAP, BIT_MASK(21), 1));
AST_PIN_MF(A18);

struct ast2400_pin_function {
	const char *name;
	const char * const *groups;
	const unsigned ngroups;
};

static const struct pinctrl_pin_desc ast2400_pinctrl_pins[] = {

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



static struct pinctrl_ops ast2400_pinctrl_ops = {
	.get_groups_count = NULL,
	.get_group_name = NULL,
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
