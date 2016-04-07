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
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/string.h>
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
 * 3. An "other" function, typically GPIO
 *
 * The functions are enabled by logic expressions over a number of bits in a
 * number of registers in the SCU, and some ports in the SuperIO controller.
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
 * logical operator (mux_expr_eval_and, mux_expr_eval_or).  A pin's high and low
 * priority expressions are then captured in a mux_prio struct, and a
 * pointer to this is tucked into the pin's pinctrl subsystem registration.
 */

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
#define SIORD30 0x30

struct mux_reg {
	unsigned reg;
	u32 (*read)(void __iomem *, unsigned);
	void (*write)(void __iomem *, unsigned, u32);
};

u32 read_mmio_bits(void __iomem *base, unsigned offset)
{
	return ioread32(base + offset);
}

void write_mmio_bits(void __iomem *base, unsigned offset, u32 val)
{
	iowrite32(val, base + offset);
}

u32 read_sio_bits(void __iomem *base, unsigned offset)
{
	return 0;
}

void write_sio_bits(void __iomem *base, unsigned offset, u32 val)
{
	/* Something... */
}

struct mux_desc {
	const struct mux_reg *reg;
	u32 mask;
	u32 shift;
	u32 val;
	bool (*eval)(const struct mux_desc *, void __iomem *);
};

static bool mux_desc_eq(const struct mux_desc *desc, void __iomem *base)
{
	u32 raw = desc->reg->read(base, desc->reg->reg);
	return ((raw & desc->mask) >> desc->shift) == desc->val;
}

static bool mux_desc_neq(const struct mux_desc *desc, void __iomem *base)
{
	return !mux_desc_eq(desc, base);
}

struct mux_expr {
	const char *name;
	int ndescs;
	const struct mux_desc *descs;
	bool (*eval)(const struct mux_expr *, void __iomem *);
	bool (*enable)(const struct mux_expr *, void __iomem *);
	bool (*disable)(const struct mux_expr *, void __iomem *);
};

static bool mux_expr_eval_and(const struct mux_expr *expr, void __iomem *base)
{
	bool ret = true;
	int i;

	for (i = 0; i < expr->ndescs; i++) {
		const struct mux_desc *desc = &expr->descs[i];
		ret = ret && desc->eval(desc, base);
	}

	return ret;
}

static bool mux_expr_eval_or(const struct mux_expr *expr, void  __iomem *base)
{
	bool ret = false;
	int i;

	for (i = 0; i < expr->ndescs; i++) {
		const struct mux_desc *desc = &expr->descs[i];
		ret = ret || desc->eval(desc, base);
	}

	return ret;
}

static bool mux_expr_enable(const struct mux_expr *expr, void __iomem *base)
{
	int i;

	/* Strategy: Flip bits until we're enabled or we run out of bits.
	 * Except STRAP or SIO.
	 */
	for (i = 0; i < expr->ndescs && !expr->eval(expr, base); i++) {
		const struct mux_desc *desc = &expr->descs[i];
		const struct mux_reg *reg = desc->reg;
		u32 val;

		if (reg->reg == STRAP || reg->reg == SIORD30)
			continue;

		val = reg->read(base, reg->reg);
		val |= (desc->val << desc->shift) & desc->mask;
		reg->write(base, reg->reg, val);
	}

	return expr->eval(expr, base);
}

static bool mux_expr_disable(const struct mux_expr *expr, void __iomem *base)
{
	int i;

	/* Strategy: Flip bits until we're disabled or we run out of bits.
	 * Except STRAP or SIO.
	 */
	for (i = 0; i < expr->ndescs && expr->eval(expr, base); i++) {
		const struct mux_desc *desc = &expr->descs[i];
		const struct mux_reg *reg = desc->reg;
		u32 val;

		if (reg->reg == STRAP || reg->reg == SIORD30)
			continue;

		val = reg->read(base, reg->reg);
		/* Is zeroing a hack? */
		val &= ~desc->mask;
		reg->write(base, reg->reg, val);
	}

	return !expr->eval(expr, base);
}

/* Cater for mixed operation expressions with more specific functions. For
 * example, mux_expr_romd() implements the following expression:
 *
 * SCU90[6]=1 || Strap[4,1:0]=100
 *
 * Really this expression means:
 *
 * SCU90[6]=1 || (Strap[4]=1 && Strap[1:0]=0)
 */
static int mux_expr_romd(const struct mux_expr *expr, void __iomem *base)
{
	const struct mux_desc *desc;
	bool ra, rb;

	if (expr->ndescs != 3) {
		/* write error and return */
		return -1;
	}

	desc = &expr->descs[0];
	ra = desc->eval(desc, base);
	if (ra)
		return ra;

	desc = &expr->descs[1];
	ra = desc->eval(desc, base);
	desc = &expr->descs[2];
	rb = desc->eval(desc, base);

	return ra && rb;
}

struct mux_prio {
	const char *other;
	const struct mux_expr *high;
	const struct mux_expr *low;
};

/* Macro hell, better to see how they're used and work backwards */

/* "Internal" macros - consumed by other macros providing better abstractions */

#define EXPR_DESCS_SYM__(_pin, _prio) mux_descs_ ## _pin ## _ ## _prio
#define EXPR_DESCS_SYM(_pin, _prio) EXPR_DESCS_SYM__(_pin, _prio)

#define EXPR_DESCS_(_pin, _prio, ...) \
	static const struct mux_desc EXPR_DESCS_SYM(_pin, _prio)[] = \
		{ __VA_ARGS__ }

#define MUX_FUNC_SYM(_signal) mux_expr_ ## _signal

#define MUX_FUNC_EXPR_(_pin, _name, _prio, _op) \
	static const struct mux_expr MUX_FUNC_SYM(_name) = { \
		.name = #_name, \
		.ndescs = ARRAY_SIZE(EXPR_DESCS_SYM(_pin, _prio)), \
		.descs = &(EXPR_DESCS_SYM(_pin, _prio))[0], \
		.eval = _op, \
		.enable = mux_expr_enable, \
		.disable = mux_expr_disable, \
	}

#define PIN_SYM__(_pin) pin_ ## _pin
#define PIN_SYM(_pin) PIN_SYM__(_pin)

#define MF_PIN_(_pin, _other, _high, _low) \
	static const struct mux_prio PIN_SYM(_pin) = { \
		.other = #_other, \
		.high = _high, \
		.low = _low, \
	}

#define SF_PIN_EXPR_(_pin, _other, _name, _prio, _op, ...) \
	EXPR_DESCS_(_pin, _prio, __VA_ARGS__); \
	MUX_FUNC_EXPR_(_pin, _name, _prio, _op); \
	MF_PIN_(_pin, _other, &MUX_FUNC_SYM(_name), NULL)

/* The non-internal macros */

#define HIGH_PRIO high
#define LOW_PRIO low

/* Initialise a pin control descriptor. */
#define MUX_REG_SYM(_reg) mux_reg_  ##  _reg
#define MUX_REG_MMIO(_reg) \
	const struct mux_reg MUX_REG_SYM(_reg) = { \
		.reg = _reg, \
		.read = read_mmio_bits, \
		.write = write_mmio_bits, \
	}

#define MUX_REG_SIO(_reg) \
	const struct mux_reg MUX_REG_SYM(_reg) = { \
		.reg = _reg, \
		.read = read_sio_bits, \
		.write = write_sio_bits, \
	}

#define MUX_DESC(_op, _reg, _idx, _val) { \
	.eval = _op, \
	.reg = &MUX_REG_SYM(_reg), \
	.mask = BIT_MASK(_idx), \
	.shift = _idx, \
	.val = _val \
}

/* Initialise a pin control descriptor, checking for value equality */
#define MUX_DESC_EQ(_reg, _idx, _val) \
	MUX_DESC(mux_desc_eq, _reg, _idx, _val)

/* Initialise a pin control descriptor, checking for negated value equality */
#define MUX_DESC_NEQ(_reg, _idx, _val) \
	MUX_DESC(mux_desc_neq, _reg, _idx, _val)

#define MUX_FUNC_EXPR(_pin, _name, _prio, _op, ...) \
	EXPR_DESCS_(_pin, _prio, __VA_ARGS__); \
	MUX_FUNC_EXPR_(_pin, _name, _prio, _op)

#define MUX_FUNC(_pin, _name, _prio, ...) \
	EXPR_DESCS_(_pin, _prio, __VA_ARGS__); \
	MUX_FUNC_EXPR_(_pin, _name, _prio, NULL)

/* Multi-function pin, i.e. has both high and low priority pin functions. Need
 * to invoke MUX_FUNC() or MUX_FUNC_EXPR() for both HIGH_PRIO and
 * LOW_PRIO to define the expressions before invoking MF_PIN().
 * Failure to do so will give a compilation error. */
#define MF_PIN(_pin, _other, _high, _low) \
	MF_PIN_(_pin, _other, \
		       	&MUX_FUNC_SYM(_high), \
		       	&MUX_FUNC_SYM(_low))

/* Single function pin, enabled by a multi-descriptor pin expression */
#define SF_PIN_EXPR(_pin, _other, _name, _op, ...) \
	SF_PIN_EXPR_(_pin, _other, _name, HIGH_PRIO, _op, __VA_ARGS__)

/* Single function pin, enabled by a single pin descriptor */
#define SF_PIN(_pin, _other, _name, ...) \
	SF_PIN_EXPR(_pin, _other, _name, NULL, __VA_ARGS__)

#define PIN_GROUP_SYM(_name) pins_ ## _name 
#define PIN_GROUP_(_name, ...) \
	static const int PIN_GROUP_SYM(_name)[] = { __VA_ARGS__ }
#define PIN_GROUP(_name, ...) PIN_GROUP_(_name, __VA_ARGS__)

#define FUNC_SIGNALS_SYM(_name) func_signals_ ## _name
#define FUNC_SIGNALS(_name, ...) \
	const struct mux_expr *const FUNC_SIGNALS_SYM(_name)[] = { __VA_ARGS__ }
#define FUNC_SIGNAL(_name) &MUX_FUNC_SYM(_name)

#define FUNC_GROUP_SYM(_name) groups_ ## _name 
#define FUNC_GROUP(_name, ...) \
	FUNC_SIGNALS(_name, __VA_ARGS__); \
	static const char *const FUNC_GROUP_SYM(_name)[] = { #_name }

#define FUNC_GROUP_GPIO(_name) FUNC_GROUP(_name)
#define FUNC_GROUP_SINGLE(_name) FUNC_GROUP(_name, FUNC_SIGNAL(_name))

MUX_REG_MMIO(SCU3C);
MUX_REG_MMIO(SCU80);
MUX_REG_MMIO(SCU8C);
MUX_REG_MMIO(SCU90);
MUX_REG_MMIO(STRAP);
MUX_REG_SIO(SIORD30);

#define D6 0
#define GPIOA0 GPIOA0
#define MAC1LINK MAC1LINK
SF_PIN(D6, GPIOA0, MAC1LINK, MUX_DESC_EQ(SCU80, 0, 1));
PIN_GROUP(GPIOA0, D6);
FUNC_GROUP_GPIO(GPIOA0);
PIN_GROUP(MAC1LINK, D6);
FUNC_GROUP_SINGLE(MAC1LINK);

#define B5 1
#define GPIOA1 GPIOA1
#define MAC2LINK MAC2LINK
SF_PIN(B5, GPIOA1, MAC2LINK, MUX_DESC_EQ(SCU80, 1, 1));
PIN_GROUP(GPIOA1, B5);
FUNC_GROUP_GPIO(GPIOA1);
PIN_GROUP(MAC2LINK, B5);
FUNC_GROUP_SINGLE(MAC2LINK);

#define A4 2
#define GPIOA2 GPIOA2
#define TIMER3 TIMER3
SF_PIN(A4, GPIOA2, TIMER3, MUX_DESC_EQ(SCU80, 2, 1));
PIN_GROUP(GPIOA2, A4);
FUNC_GROUP_GPIO(GPIOA2);
PIN_GROUP(TIMER3, A4);
FUNC_GROUP_SINGLE(TIMER3);

#define E6 3
#define GPIOA3 GPIOA3
#define TIMER4 TIMER4
SF_PIN(E6, GPIOA3, TIMER4, MUX_DESC_EQ(SCU80, 3, 1));
PIN_GROUP(GPIOA3, E6);
FUNC_GROUP_GPIO(GPIOA3);
PIN_GROUP(TIMER4, E6);
FUNC_GROUP_SINGLE(TIMER4);

#define C5 4
#define GPIOA4 GPIOA4
#define SCL9 SCL9
#define TIMER5 TIMER5
MUX_FUNC(C5, SCL9, HIGH_PRIO, MUX_DESC_EQ(SCU90, 22, 1));
MUX_FUNC(C5, TIMER5, LOW_PRIO, MUX_DESC_EQ(SCU80, 4, 1));
MF_PIN(C5, GPIOA4, SCL9, TIMER5);
PIN_GROUP(GPIOA4, C5);
FUNC_GROUP_GPIO(GPIOA4);
PIN_GROUP(TIMER5, C5);
FUNC_GROUP_SINGLE(TIMER5);

#define B4 5
#define GPIOA5 GPIOA5
#define SDA9 SDA9
#define TIMER6 TIMER6
MUX_FUNC(B4, SDA9, HIGH_PRIO, MUX_DESC_EQ(SCU90, 22, 1));
MUX_FUNC(B4, TIMER6, LOW_PRIO, MUX_DESC_EQ(SCU80, 5, 1));
MF_PIN(B4, GPIOA5, SDA9, TIMER6);
PIN_GROUP(GPIOA5, B4);
FUNC_GROUP_GPIO(GPIOA5);
PIN_GROUP(TIMER6, B4);
FUNC_GROUP_SINGLE(TIMER6);
#define I2C9 I2C9
PIN_GROUP(I2C9, C5, B4);
FUNC_GROUP(I2C9, FUNC_SIGNAL(SCL9), FUNC_SIGNAL(SDA9));

#define A3 6
#define GPIOA6 GPIOA6
#define MDC2 MDC2
#define TIMER7 TIMER7
MUX_FUNC(A3, MDC2, HIGH_PRIO, MUX_DESC_EQ(SCU90, 2, 1));
MUX_FUNC(A3, TIMER7, LOW_PRIO, MUX_DESC_EQ(SCU80, 6, 1));
MF_PIN(A3, GPIOA6, MDC2, TIMER7);
PIN_GROUP(GPIOA6, A3);
FUNC_GROUP_GPIO(GPIOA6);
PIN_GROUP(TIMER7, A3);
FUNC_GROUP_SINGLE(TIMER7);

#define D5 7
#define GPIOA7 GPIOA7
#define MDIO2 MDIO2
#define TIMER8 TIMER8
MUX_FUNC(D5, MDIO2, HIGH_PRIO, MUX_DESC_EQ(SCU90, 2, 1));
MUX_FUNC(D5, TIMER8, LOW_PRIO, MUX_DESC_EQ(SCU80, 7, 1));
MF_PIN(D5, GPIOA7, MDIO2, TIMER8);
PIN_GROUP(GPIOA7, D5);
FUNC_GROUP_GPIO(GPIOA7);
PIN_GROUP(TIMER8, D5);
FUNC_GROUP_SINGLE(TIMER8);
#define MD2 MD2
PIN_GROUP(MD2, A3, D5);
FUNC_GROUP(MD2, FUNC_SIGNAL(MDC2), FUNC_SIGNAL(MDIO2));

#define J21 8
#define GPIOB0 GPIOB0
#define SALT1 SALT1
SF_PIN(J21, GPIOB0, SALT1, MUX_DESC_EQ(SCU80, 8, 1));
PIN_GROUP(GPIOB0, J21);
FUNC_GROUP_GPIO(GPIOB0);
PIN_GROUP(SALT1, J21);
FUNC_GROUP_SINGLE(SALT1);

#define J20 9
#define GPIOB1 GPIOB1
#define SALT2 SALT2
SF_PIN(J20, GPIOB1, SALT2, MUX_DESC_EQ(SCU80, 9, 1));
PIN_GROUP(GPIOB1, J20);
FUNC_GROUP_GPIO(GPIOB1);
PIN_GROUP(SALT2, J20);
FUNC_GROUP_SINGLE(SALT2);

#define H18 10
#define GPIOB2 GPIOB2
#define SALT3 SALT3
SF_PIN(H18, GPIOB2, SALT3, MUX_DESC_EQ(SCU80, 10, 1));
PIN_GROUP(GPIOB2, H18);
FUNC_GROUP_GPIO(GPIOB2);
PIN_GROUP(SALT3, H18);
FUNC_GROUP_SINGLE(SALT3);

#define F18 11
#define GPIOB3 GPIOB3
#define SALT4 SALT4
SF_PIN(F18, GPIOB3, SALT4, MUX_DESC_EQ(SCU80, 11, 1));
PIN_GROUP(GPIOB3, F18);
FUNC_GROUP_GPIO(GPIOB3);
PIN_GROUP(SALT4, F18);
FUNC_GROUP_SINGLE(SALT4);

#define E19 12
#define GPIOB4 GPIOB4
#define LPCRST LPCRST
SF_PIN_EXPR(E19, GPIOB4, LPCRST, mux_expr_eval_or,
	       	MUX_DESC_EQ(SCU80, 12, 1),
		MUX_DESC_EQ(STRAP, 14, 1));
PIN_GROUP(GPIOB4, E19);
FUNC_GROUP_GPIO(GPIOB4);
PIN_GROUP(LPCRST, E19);
FUNC_GROUP_SINGLE(LPCRST);

#define H19 13
#define GPIOB5 GPIOB5
#define LPCPD LPCPD
#define LPCSMI LPCSMI
MUX_FUNC_EXPR(H19, LPCPD, HIGH_PRIO,
		mux_expr_eval_and,
	       	MUX_DESC_EQ(SCU80, 13, 1),
		MUX_DESC_EQ(SIORD30, 1, 0));
MUX_FUNC_EXPR(H19, LPCSMI, LOW_PRIO,
		mux_expr_eval_and,
	       	MUX_DESC_EQ(SCU80, 13, 1),
		MUX_DESC_EQ(SIORD30, 1, 1));
MF_PIN(H19, GPIOB5, LPCPD, LPCSMI);
PIN_GROUP(GPIOB5, H19);
FUNC_GROUP_GPIO(GPIOB5);
PIN_GROUP(LPCPD, H19);
FUNC_GROUP_SINGLE(LPCPD);
PIN_GROUP(LPCSMI, H19);
FUNC_GROUP_SINGLE(LPCSMI);

#define H20 14
#define GPIOB6 GPIOB6
#define LPCPME LPCPME
SF_PIN(H20, GPIOB6, LPCPME, MUX_DESC_EQ(SCU80, 14, 1));
PIN_GROUP(GPIOB6, H20);
FUNC_GROUP_GPIO(GPIOB6);
PIN_GROUP(LPCPME, H20);
FUNC_GROUP_SINGLE(LPCPME);

#define E18 15
#define GPIOB7 GPIOB7
#define EXTRST EXTRST
#define SPICS1 SPICS1
MUX_FUNC_EXPR(E18, EXTRST, HIGH_PRIO,
	       	mux_expr_eval_and,
	       	MUX_DESC_EQ(SCU80, 15, 1),
		MUX_DESC_EQ(SCU90, 31, 0),
		MUX_DESC_EQ(SCU3C, 3, 1));
MUX_FUNC_EXPR(E18, SPICS1, LOW_PRIO,
	       	mux_expr_eval_and,
	       	MUX_DESC_EQ(SCU80, 15, 1),
		MUX_DESC_EQ(SCU90, 31, 1));
MF_PIN(E18, GPIOB7, EXTRST, SPICS1);
PIN_GROUP(GPIOB7, E18);
FUNC_GROUP_GPIO(GPIOB7);
PIN_GROUP(EXTRST, E18);
FUNC_GROUP_SINGLE(EXTRST);
PIN_GROUP(SPICS1, E18);
FUNC_GROUP_SINGLE(SPICS1);

/*
MUX_FUNC(A18, "SD2CLK", HIGH_PRIO, MUX_DESC_EQ(SCU90, 1, 1));
MUX_FUNC_EXPR(A18, "GPID0(In)", LOW_PRIO,
	       	mux_expr_eval_or,
	       	MUX_DESC_EQ(SCU8C, 1, 1),
		MUX_DESC_EQ(STRAP, 21, 1));
MF_PIN(A18, "GPIOD0");
*/

#define AST_PINCTRL_PIN(_name) \
	[_name] = { \
		.number = _name, \
		.name = #_name, \
		.drv_data = (void *)&(PIN_SYM(_name)) \
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
	AST_PINCTRL_GROUP(GPIOA0),
	AST_PINCTRL_GROUP(GPIOA1),
	AST_PINCTRL_GROUP(GPIOA2),
	AST_PINCTRL_GROUP(GPIOA3),
	AST_PINCTRL_GROUP(GPIOA4),
	AST_PINCTRL_GROUP(GPIOA5),
	AST_PINCTRL_GROUP(GPIOA6),
	AST_PINCTRL_GROUP(GPIOA7),
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

	AST_PINCTRL_GROUP(GPIOB0),
	AST_PINCTRL_GROUP(GPIOB1),
	AST_PINCTRL_GROUP(GPIOB2),
	AST_PINCTRL_GROUP(GPIOB3),
	AST_PINCTRL_GROUP(GPIOB4),
	AST_PINCTRL_GROUP(GPIOB5),
	AST_PINCTRL_GROUP(GPIOB6),
	AST_PINCTRL_GROUP(GPIOB7),
	AST_PINCTRL_GROUP(SALT1),
	AST_PINCTRL_GROUP(SALT2),
	AST_PINCTRL_GROUP(SALT3),
	AST_PINCTRL_GROUP(SALT4),
	AST_PINCTRL_GROUP(LPCRST),
	AST_PINCTRL_GROUP(LPCPD),
	AST_PINCTRL_GROUP(LPCSMI),
	AST_PINCTRL_GROUP(LPCPME),
	AST_PINCTRL_GROUP(EXTRST),
};

struct ast2400_pin_function {
	const char *name;
	const char *const *groups;
	unsigned ngroups;
	const struct mux_expr *const *signals;
	unsigned nsignals;
};

#define AST_PINCTRL_FUNC(_name, ...) { \
	.name = #_name, \
	.groups = &FUNC_GROUP_SYM(_name)[0], \
	.ngroups = ARRAY_SIZE(FUNC_GROUP_SYM(_name)), \
	.signals = &FUNC_SIGNALS_SYM(_name)[0], \
	.nsignals = ARRAY_SIZE(FUNC_SIGNALS_SYM(_name)), \
}

static const struct ast2400_pin_function ast2400_functions[] = {
	AST_PINCTRL_FUNC(GPIOA0),
	AST_PINCTRL_FUNC(GPIOA1),
	AST_PINCTRL_FUNC(GPIOA2),
	AST_PINCTRL_FUNC(GPIOA3),
	AST_PINCTRL_FUNC(GPIOA4),
	AST_PINCTRL_FUNC(GPIOA5),
	AST_PINCTRL_FUNC(GPIOA6),
	AST_PINCTRL_FUNC(GPIOA7),
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

	AST_PINCTRL_FUNC(GPIOB0),
	AST_PINCTRL_FUNC(GPIOB1),
	AST_PINCTRL_FUNC(GPIOB2),
	AST_PINCTRL_FUNC(GPIOB3),
	AST_PINCTRL_FUNC(GPIOB4),
	AST_PINCTRL_FUNC(GPIOB5),
	AST_PINCTRL_FUNC(GPIOB6),
	AST_PINCTRL_FUNC(GPIOB7),
	AST_PINCTRL_FUNC(SALT1),
	AST_PINCTRL_FUNC(SALT2),
	AST_PINCTRL_FUNC(SALT3),
	AST_PINCTRL_FUNC(SALT4),
	AST_PINCTRL_FUNC(LPCRST),
	AST_PINCTRL_FUNC(LPCPD),
	AST_PINCTRL_FUNC(LPCSMI),
	AST_PINCTRL_FUNC(LPCPME),
	AST_PINCTRL_FUNC(EXTRST),
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

static inline int maybe_disable(const struct mux_expr *expr, void __iomem *base)
{
	if (expr)
		return expr->disable(expr, base);
	return 1;
}

static int ast2400_pinmux_set_mux(struct pinctrl_dev *pctldev,
			      unsigned function,
			      unsigned group)
{
	int i;
	int ret;
	const struct ast2400_pinctrl_data *pdata =
	       	pinctrl_dev_get_drvdata(pctldev);
	const struct ast2400_pin_group *pgroup = &pdata->groups[group];
	const struct ast2400_pin_function *pfunc =
	       	&pdata->functions[function];
	const bool gpio = pfunc->nsignals == 0;

	if (pfunc->nsignals > 0 && pfunc->nsignals != pgroup->npins) {
		/* Print error */
		return -1;
	}

	for (i = 0; i < pgroup->npins; i++) {
		int pin = pgroup->pins[i];
		const struct pinctrl_pin_desc *ppin = &pdata->pins[pin];
		const struct mux_prio *pprio = ppin->drv_data;

		ret = maybe_disable(pprio->high, pdata->reg_base);
		if (!ret && gpio)
			return -1;

		ret = maybe_disable(pprio->low, pdata->reg_base);
		if (!ret && gpio)
			return -1;

		if (!gpio) {
			const struct mux_expr *signal = pfunc->signals[i];
			if (!signal->enable(signal, pdata->reg_base))
				return -1;
		}
	}

	return 0;
}

enum pin_prio { prio_other = 0, prio_low, prio_high };

static int eval_mux_expr(const struct mux_expr *expr, void __iomem *base)
{
	if (!(expr->ndescs && expr->descs)) {
		return -1;
	}
	if (expr->eval) {
		return expr->eval(expr, base);
	}
	return expr->descs->eval(expr->descs, base);
}

static enum pin_prio get_pin_prio(struct mux_prio *prios, void __iomem *base)
{
	if (eval_mux_expr(prios->high, base))
		return prio_high;

	if (eval_mux_expr(prios->low, base))
		return prio_low;

	return prio_other;
}

static int ast2400_pinmux_request(struct pinctrl_dev *pctldev, unsigned offset)
{
	struct ast2400_pinctrl_data *pdata = pinctrl_dev_get_drvdata(pctldev);
	struct mux_prio *prios = pdata->pins[offset].drv_data;

	return -((int) get_pin_prio(prios, pdata->reg_base));
}

static int ast2400_pinmux_free(struct pinctrl_dev *pctldev, unsigned offset)
{
	struct ast2400_pinctrl_data *pdata = pinctrl_dev_get_drvdata(pctldev);
	struct mux_prio *prios = pdata->pins[offset].drv_data;

	enum pin_prio prio = get_pin_prio(prios, pdata->reg_base);
	switch (prio) {
		case prio_high:
			prios->high->disable(prios->high, pdata->reg_base);
			break;
		case prio_low:
			prios->low->disable(prios->low, pdata->reg_base);
			break;
		case prio_other:
			/* No action required */
			break;
	}
	return prio;
}

static struct pinmux_ops ast2400_pinmux_ops = {
	.request = ast2400_pinmux_request,
	.free = ast2400_pinmux_free,
	.get_functions_count = ast2400_pinmux_get_fn_count,
	.get_function_name = ast2400_pinmux_get_fn_name,
	.get_function_groups = ast2400_pinmux_get_fn_groups,
	.set_mux = ast2400_pinmux_set_mux,
	.strict = true,
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
