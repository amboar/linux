/*
 * Copyright (c) 2014 ARM/Linaro
 *
 * Author: Daniel Lezcano <daniel.lezcano@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Maintainer: Daniel Lezcano <daniel.lezcano@linaro.org>
 */

#ifdef CONFIG_SCHED_IO_LATENCY
extern void io_latency_init(void);
extern int  io_latency_begin(struct rq *rq, struct task_struct *tsk);
extern void io_latency_end(struct rq *rq, struct task_struct *tsk);
extern int  io_latency_get_sleep_length(struct rq *rq);
#else
static inline void io_latency_init(void)
{
	;
}

static inline int io_latency_begin(struct rq *rq, struct task_struct *tsk)
{
	return 0;
}

static inline void io_latency_end(struct rq *rq, struct task_struct *tsk)
{
	;
}

static inline int io_latency_get_sleep_length(struct rq *rq)
{
	return 0;
}
#endif
