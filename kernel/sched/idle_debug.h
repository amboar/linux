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

struct cpuidle_device;
struct cpuidle_driver;

#ifdef CONFIG_SCHED_IDLE_DEBUG
extern void idle_debug_prediction_update(struct cpuidle_driver *drv,
					 struct cpuidle_device *dev,
					 struct cpuidle_times  *times,
					 int index);
#else
static inline void idle_debug_prediction_update(struct cpuidle_driver *drv,
						struct cpuidle_device *dev,
						struct cpuidle_times  *times,
						int index)
{
	;
}
#endif
