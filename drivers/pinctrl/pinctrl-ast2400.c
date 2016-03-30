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
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/types.h>
#include "core.h"
#include "pinctrl-utils.h"

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
	bool (*eval)(void __iomem *, struct pin_ctrl_desc *);
	unsigned reg;
	u32 mask;
	u32 enable;
	u32 disable;
};

static bool pin_desc_eq(void __iomem *base, struct pin_ctrl_desc *desc)
{
	u32 val = ioread32(base + desc->reg) & desc->mask;
	return val == desc->enable;
}

static bool pin_desc_neq(void __iomem *base, struct pin_ctrl_desc *desc)
{
	return !pin_desc_eq(base, desc);
}

struct pin_func_expr {
	const char *name;
	int ndescs;
	struct pin_ctrl_desc *descs;
	bool (*eval)(void __iomem *, struct pin_func_expr *);
	int (*enable)(void __iomem *, struct pin_func_expr *);
	int (*disable)(void __iomem *, struct pin_func_expr *);
};

static int func_expr_and(void __iomem *base, struct pin_func_expr *expr)
{
	int ret = 1;
	int i;

	for (i = 0; i < expr->ndescs; i++) {
		struct pin_ctrl_desc *desc = &expr->descs[i];
		ret &= desc->eval(base, desc);
	}
	return ret;
}

static int func_expr_or(void  __iomem *base, struct pin_func_expr *expr)
{
	int ret = 0;
	int i;

	for (i = 0; i < expr->ndescs; i++) {
		struct pin_ctrl_desc *desc = &expr->descs[i];
		ret |= desc->eval(base, desc);
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
		.name = #_name, \
		.eval = _op, \
		.ndescs = ARRAY_SIZE(CTRL_DESC_SYM(_ball, _prio)), \
		.descs = &(CTRL_DESC_SYM(_ball, _prio))[0], \
	}

#define BALL_SYM__(_ball) ball_##_ball
#define BALL_SYM(_ball) BALL_SYM__(_ball)

#define MF_PIN_(_ball, _fallback, _high, _low) \
	static struct pin_func_prio BALL_SYM(_ball) = { \
		.fallback = #_fallback, \
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
	{ .eval = _op, .reg = _reg, .mask = _mask, .val = _val }

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

#define D6 0
#define B5 1
#define A4 2
#define E6 3
#define C5 4
#define B4 5
#define A3 6
#define D5 7
#define J21 8
#define J20 9
#define H18 10
#define F18 11
#define E19 12
#define H19 13
#define H20 14
#define E18 15

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

SF_PIN(D6, GPIOA0, MAC1LINK, CTRL_DESC_EQ(SCU80, BIT_MASK(0), 1));
SF_PIN(B5, GPIOA1, MAC2LINK, CTRL_DESC_EQ(SCU80, BIT_MASK(1), 1));
SF_PIN(A4, GPIOA2, TIMER3, CTRL_DESC_EQ(SCU80, BIT_MASK(2), 1));
SF_PIN(E6, GPIOA3, TIMER4, CTRL_DESC_EQ(SCU80, BIT_MASK(3), 1));

PRIO_FUNC(C5, SCL9, HIGH_PRIO, CTRL_DESC_EQ(SCU90, BIT_MASK(22), 1));
PRIO_FUNC(C5, TIMER5, LOW_PRIO, CTRL_DESC_EQ(SCU80, BIT_MASK(4), 1));
MF_PIN(C5, GPIOA4);

PRIO_FUNC(B4, SDA9, HIGH_PRIO, CTRL_DESC_EQ(SCU90, BIT_MASK(22), 1));
PRIO_FUNC(B4, TIMER6, LOW_PRIO, CTRL_DESC_EQ(SCU80, BIT_MASK(5), 1));
MF_PIN(B4, GPIOA5);

PRIO_FUNC(A3, MDC2, HIGH_PRIO, CTRL_DESC_EQ(SCU90, BIT_MASK(2), 1));
PRIO_FUNC(A3, TIMER7, LOW_PRIO, CTRL_DESC_EQ(SCU80, BIT_MASK(6), 1));
MF_PIN(A3, GPIOA6);

PRIO_FUNC(D5, MDIO2, HIGH_PRIO, CTRL_DESC_EQ(SCU90, BIT_MASK(2), 1));
PRIO_FUNC(D5, TIMER8, LOW_PRIO, CTRL_DESC_EQ(SCU80, BIT_MASK(7), 1));
MF_PIN(D5, GPIOA7);

SF_PIN(J21, GPIOB0, SALT1, CTRL_DESC_EQ(SCU80, BIT_MASK(8), 1));
SF_PIN(J20, GPIOB1, SALT2, CTRL_DESC_EQ(SCU80, BIT_MASK(9), 1));
SF_PIN(H18, GPIOB2, SALT3, CTRL_DESC_EQ(SCU80, BIT_MASK(10), 1));
SF_PIN(F18, GPIOB3, SALT4, CTRL_DESC_EQ(SCU80, BIT_MASK(11), 1));

SF_PIN_EXPR(E19, GPIOB4, LPCRST, func_expr_or,
	       	CTRL_DESC_EQ(SCU80, BIT_MASK(12), 1),
		CTRL_DESC_EQ(STRAP, BIT_MASK(14), 1));

/* H19: Need magic for SIORD30
PRIO_FUNC_EXPR(H19, LPCPD, HIGH_PRIO,
		func_expr_and,
	       	CTRL_DESC_EQ(SCU8C, BIT_MASK(1), 1),
		CTRL_DESC_EQ(STRAP, BIT_MASK(21), 1));
PRIO_FUNC_EXPR(H19, LPCSMI, LOW_PRIO,
		func_expr_and,
	       	CTRL_DESC_EQ(SCU8C, BIT_MASK(1), 1),
		CTRL_DESC_EQ(STRAP, BIT_MASK(21), 1));
MF_PIN(H19, GPIOB5);
*/
MF_PIN_(H19, GPIOB5, NULL, NULL);

SF_PIN(H20, GPIOB6, LPCPME, CTRL_DESC_EQ(SCU80, BIT_MASK(14), 1));

PRIO_FUNC_EXPR(E18, EXTRST, HIGH_PRIO,
	       	func_expr_and,
	       	CTRL_DESC_EQ(SCU80, BIT_MASK(15), 1),
		CTRL_DESC_EQ(SCU90, BIT_MASK(31), 0),
		CTRL_DESC_EQ(SCU3C, BIT_MASK(3), 1));
PRIO_FUNC_EXPR(E18, SPICS1, LOW_PRIO,
	       	func_expr_and,
	       	CTRL_DESC_EQ(SCU80, BIT_MASK(15), 1),
		CTRL_DESC_EQ(SCU90, BIT_MASK(31), 1));
MF_PIN(E18, GPIOB7);

/*
PRIO_FUNC(A18, "SD2CLK", HIGH_PRIO, CTRL_DESC_EQ(SCU90, BIT_MASK(1), 1));
PRIO_FUNC_EXPR(A18, "GPID0(In)", LOW_PRIO,
	       	func_expr_or,
	       	CTRL_DESC_EQ(SCU8C, BIT_MASK(1), 1),
		CTRL_DESC_EQ(STRAP, BIT_MASK(21), 1));
MF_PIN(A18, "GPIOD0");
*/

#define AST_PINCTRL_PIN(_name) \
	[_name] = { \
		.number = _name, \
		.name = #_name, \
		.drv_data = &(BALL_SYM(_name)) \
	}

static const struct pinctrl_pin_desc ast2400_pins[] = {
	AST_PINCTRL_PIN(D6),
	AST_PINCTRL_PIN(B5),
	AST_PINCTRL_PIN(A4),
	AST_PINCTRL_PIN(E6),
	AST_PINCTRL_PIN(C5),
	AST_PINCTRL_PIN(B4),
	AST_PINCTRL_PIN(A3),
	AST_PINCTRL_PIN(D5),
	AST_PINCTRL_PIN(J21),
	AST_PINCTRL_PIN(J20),
	AST_PINCTRL_PIN(H18),
	AST_PINCTRL_PIN(F18),
	AST_PINCTRL_PIN(E19),
	AST_PINCTRL_PIN(H19),
	AST_PINCTRL_PIN(H20),
	AST_PINCTRL_PIN(E18)
};

#define PIN_GROUP_SYM(_name) _name##_pins
#define PIN_GROUP_(_name, ...) \
	static const int PIN_GROUP_SYM(_name)[] = { __VA_ARGS__ }
#define PIN_GROUP(_name, ...) PIN_GROUP_(_name, __VA_ARGS__)

PIN_GROUP(GPIOA, D6, B5, A4, E6, C5, B4, A3, D5);
PIN_GROUP(MAC1LINK, D6);
PIN_GROUP(MAC2LINK, B5);
PIN_GROUP(TIMER3, A4);
PIN_GROUP(TIMER4, E6);
PIN_GROUP(TIMER5, C5);
PIN_GROUP(TIMER6, B4);
PIN_GROUP(I2C9, C5, B4);
PIN_GROUP(TIMER7, A3);
PIN_GROUP(TIMER8, D5);
PIN_GROUP(MD2, A3, D5);

PIN_GROUP(GPIOB, J21, J20, H18, F18, E19, H19, H20, E18);
PIN_GROUP(SALT1, J21);
PIN_GROUP(SALT2, J20);
PIN_GROUP(SALT3, H18);
PIN_GROUP(SALT4, F18);
PIN_GROUP(LPCRST, E19);
PIN_GROUP(LPCPD, H19);
PIN_GROUP(LPCSMI, H19);
PIN_GROUP(LPCPME, H20);
PIN_GROUP(EXTRST, E18);
PIN_GROUP(SPICS1, E18);

struct ast2400_pin_group {
	const char *name;
	const unsigned int *pins;
	const unsigned npins;
};

#define AST_PINCTRL_GROUP(_name) { \
	.name = #_name, \
	.pins = &(PIN_GROUP_SYM(_name))[0], \
	.npins = ARRAY_SIZE(PIN_GROUP_SYM(_name)), \
}

static const struct ast2400_pin_group ast2400_groups[] = {
	AST_PINCTRL_GROUP(GPIOA),
	AST_PINCTRL_GROUP(MAC1LINK),
	AST_PINCTRL_GROUP(MAC2LINK),
	AST_PINCTRL_GROUP(TIMER3),
	AST_PINCTRL_GROUP(TIMER4),
	AST_PINCTRL_GROUP(TIMER5),
	AST_PINCTRL_GROUP(TIMER6),
	AST_PINCTRL_GROUP(I2C9),
	AST_PINCTRL_GROUP(TIMER7),
	AST_PINCTRL_GROUP(TIMER8),
	AST_PINCTRL_GROUP(MD2),
	AST_PINCTRL_GROUP(GPIOB),
	AST_PINCTRL_GROUP(SALT1),
	AST_PINCTRL_GROUP(SALT2),
	AST_PINCTRL_GROUP(SALT3),
	AST_PINCTRL_GROUP(SALT4),
	AST_PINCTRL_GROUP(LPCRST),
	AST_PINCTRL_GROUP(LPCPD),
	AST_PINCTRL_GROUP(LPCSMI),
	AST_PINCTRL_GROUP(LPCPME),
	AST_PINCTRL_GROUP(EXTRST),
	AST_PINCTRL_GROUP(GPIOB),
};

#define FUNC_GROUP_SYM(_name) _name##_groups
#define FUNC_GROUP(_name) \
	static const char *const FUNC_GROUP_SYM(_name)[] = { #_name }

FUNC_GROUP(GPIOA);
FUNC_GROUP(MAC1LINK);
FUNC_GROUP(MAC2LINK);
FUNC_GROUP(TIMER3);
FUNC_GROUP(TIMER4);
FUNC_GROUP(TIMER5);
FUNC_GROUP(TIMER6);
FUNC_GROUP(I2C9);
FUNC_GROUP(TIMER7);
FUNC_GROUP(TIMER8);
FUNC_GROUP(MD2);
FUNC_GROUP(GPIOB);
FUNC_GROUP(SALT1);
FUNC_GROUP(SALT2);
FUNC_GROUP(SALT3);
FUNC_GROUP(SALT4);
FUNC_GROUP(LPCRST);
FUNC_GROUP(LPCPD);
FUNC_GROUP(LPCSMI);
FUNC_GROUP(LPCPME);
FUNC_GROUP(EXTRST);

struct ast2400_pin_function {
	const char *name;
	const char *const *groups;
	const unsigned ngroups;
};

#define AST_PINCTRL_FUNC(_name) { \
	.name = #_name, \
	.groups = &FUNC_GROUP_SYM(_name)[0], \
	.ngroups = ARRAY_SIZE(FUNC_GROUP_SYM(_name)), \
}

static const struct ast2400_pin_function ast2400_functions[] = {
	AST_PINCTRL_FUNC(GPIOA),
	AST_PINCTRL_FUNC(MAC1LINK),
	AST_PINCTRL_FUNC(MAC2LINK),
	AST_PINCTRL_FUNC(TIMER3),
	AST_PINCTRL_FUNC(TIMER4),
	AST_PINCTRL_FUNC(TIMER5),
	AST_PINCTRL_FUNC(TIMER6),
	AST_PINCTRL_FUNC(I2C9),
	AST_PINCTRL_FUNC(TIMER7),
	AST_PINCTRL_FUNC(TIMER8),
	AST_PINCTRL_FUNC(MD2),
	AST_PINCTRL_FUNC(GPIOB),
	AST_PINCTRL_FUNC(SALT1),
	AST_PINCTRL_FUNC(SALT2),
	AST_PINCTRL_FUNC(SALT3),
	AST_PINCTRL_FUNC(SALT4),
	AST_PINCTRL_FUNC(LPCRST),
	AST_PINCTRL_FUNC(LPCPD),
	AST_PINCTRL_FUNC(LPCSMI),
	AST_PINCTRL_FUNC(LPCPME),
	AST_PINCTRL_FUNC(EXTRST),
	AST_PINCTRL_FUNC(GPIOB),
};

struct ast2400_pinctrl_data {
	void __iomem *reg_base;

	const struct pinctrl_pin_desc *pins;
	const unsigned npins;

	const struct ast2400_pin_group *groups;
	const unsigned ngroups;

	const struct ast2400_pin_function *functions;
	const unsigned nfunctions;
};

static struct ast2400_pinctrl_data ast2400_pinctrl = {
	.pins = ast2400_pins,
	.npins = ARRAY_SIZE(ast2400_pins),
	.groups = ast2400_groups,
	.ngroups = ARRAY_SIZE(ast2400_groups),
	.functions = ast2400_functions,
	.nfunctions = ARRAY_SIZE(ast2400_functions),
};

static int ast2400_pinctrl_get_groups_count(struct pinctrl_dev *pctldev)
{
	struct ast2400_pinctrl_data *pdata = pinctrl_dev_get_drvdata(pctldev);

	return pdata->ngroups;
}

static const char *ast2400_pinctrl_get_group_name(struct pinctrl_dev *pctldev,
	       					  unsigned group)
{
	struct ast2400_pinctrl_data *pdata = pinctrl_dev_get_drvdata(pctldev);

	return pdata->groups[group].name;
}

static int ast2400_pinctrl_get_group_pins(struct pinctrl_dev *pctldev,
					        unsigned group,
					        const unsigned **pins,
					        unsigned *npins)
{
	struct ast2400_pinctrl_data *pdata = pinctrl_dev_get_drvdata(pctldev);

	*pins = &pdata->groups[group].pins[0];
	*npins = pdata->groups[group].npins;

	return 0;
}

static void ast2400_pinctrl_pin_dbg_show(struct pinctrl_dev *pctldev,
					 struct seq_file *s,
					 unsigned offset)
{
	seq_printf(s, " %s", dev_name(pctldev->dev));
}

static struct pinctrl_ops ast2400_pinctrl_ops = {
	.get_groups_count = ast2400_pinctrl_get_groups_count,
	.get_group_name = ast2400_pinctrl_get_group_name,
	.get_group_pins = ast2400_pinctrl_get_group_pins,
	.pin_dbg_show = ast2400_pinctrl_pin_dbg_show,
	.dt_node_to_map = pinconf_generic_dt_node_to_map_pin,
	.dt_free_map = pinctrl_utils_dt_free_map,
};

static int ast2400_pinmux_get_fn_count(struct pinctrl_dev *pctldev)
{
	struct ast2400_pinctrl_data *pdata = pinctrl_dev_get_drvdata(pctldev);

	return pdata->nfunctions;
}

static const char *ast2400_pinmux_get_fn_name(struct pinctrl_dev *pctldev,
						unsigned function)
{
	struct ast2400_pinctrl_data *pdata = pinctrl_dev_get_drvdata(pctldev);

	return pdata->functions[function].name;
}

static int ast2400_pinmux_get_fn_groups(struct pinctrl_dev *pctldev,
					  unsigned function,
					  const char * const **groups,
					  unsigned * const num_groups)
{
	struct ast2400_pinctrl_data *pdata = pinctrl_dev_get_drvdata(pctldev);

	*groups = pdata->functions[function].groups;
	*num_groups = pdata->functions[function].ngroups;

	return 0;
}

static int ast2400_pinmux_set_mux(struct pinctrl_dev *pctldev,
			      unsigned function,
			      unsigned group)
{
	return -ENOTSUPP;
}

enum pin_prio { prio_fallback = 0, prio_low, prio_high }

static bool eval_func_expr(void __iomem *base, struct pin_func_expr *expr)
{
	if (!(expr->ndescs && expr->descs)) {
		return false;
	}
	if (expr->op) {
		return expr->eval(base, expr);
	}
	return expr->descs->eval(base, expr->descs);
}

static enum pin_prio get_pin_prio(struct pinctrl_dev *pctldev,
	       			  	  unsigned offset)
{
	struct ast2400_pinctrl_data *pdata = pinctrl_dev_get_drvdata(pctldev);
	struct pin_func_prio *prios = &pdata->pins[offset];

	if (eval_func_expr(pdata->iomem, prios->high))
		return prio_high;

	if (eval_func_expr(pdata->iomem, prios->low))
		return prio_low;

	return prio_fallback;
}

static int ast2400_pinmux_request(struct pinctrl_dev *pctldev, unsigned offset)
{
	return -((int) get_pin_prio(pctldev, offset));
}

static int ast2400_pinmux_free(struct pinctrl_dev *pctldev, unsigned offset)
{
	enum pin_prio prio = get_pin_prio(pctldev, offset);
	return 0;
}

static struct pinmux_ops ast2400_pinmux_ops = {
	.request = ast2400_pinmux_request,
	.free = ast2400_pinmux_free,
	.get_functions_count = ast2400_pinmux_get_fn_count,
	.get_function_name = ast2400_pinmux_get_fn_name,
	.get_function_groups = ast2400_pinmux_get_fn_groups,
	.set_mux = ast2400_pinmux_set_mux,
	.strict = true;
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
