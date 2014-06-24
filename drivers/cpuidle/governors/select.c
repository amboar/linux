/*
 * select.c - the select governor
 *
 * Copyright (C) 2014 Daniel Lezcano <daniel.lezcano@linaro.org>
 *
*/

#include <linux/cpuidle.h>

static int select(struct cpuidle_driver *drv, struct cpuidle_device *dev,
		  struct cpuidle_times *times)
{
	int i, index = 0, latency_req = times->latency_req;
	unsigned int next_event;

	/*
	 * If the guessed IO next event is zero, that means there is no IO
	 * pending, so we ignore it in the equation
	 */
	next_event = times->next_io_event ? 
		min(times->next_io_event, times->next_timer_event) :
		times->next_timer_event;

	for (i = 0; i < drv->state_count; i++) {

		struct cpuidle_state *s = &drv->states[i];
		struct cpuidle_state_usage *su = &dev->states_usage[i];

		if (s->disabled || su->disable)
			continue;
		if (s->target_residency > next_event)
			continue;
		if (s->exit_latency > latency_req)
			continue;

		index = i;
	}

	return index;
}

static struct cpuidle_governor select_governor = {
	.name   = "select",
	.rating = 10,
	.select = select,
	.owner  = THIS_MODULE,
};

static int __init select_init(void)
{
	return cpuidle_register_governor(&select_governor);
}

postcore_initcall(select_init);

