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

#include <linux/cpuidle.h>
#include <linux/debugfs.h>
#include <linux/atomic.h>
#include <linux/init.h>

static atomic_t idle_predictions_under_estimate;
static atomic_t idle_predictions_over_estimate;
static atomic_t idle_predictions_success;

void idle_debug_prediction_update(struct cpuidle_driver *drv,
				  struct cpuidle_device *dev,
				  struct cpuidle_times *times, int index)
{
	int residency, target_residency;
	int i;

	/*
	 * The cpuidle driver was not able to enter an idle state, the
	 * last_residency is then zero and it does not make sense to
	 * update the predictions accuracy.
	 */
	residency = dev->last_residency;
	if (!residency)
		return;

	target_residency = drv->states[index].target_residency;

	/*
	 * The last residency is smaller than the target residency, we
	 * overestimated the sleep time.
	 */
	if (residency < target_residency) {
		atomic_inc(&idle_predictions_over_estimate);
		return;
	}

	/*
	 * This state is not the deepest one, get the next time residency to
	 * check if we could have been deeper in idle.
	 */
	for (i = index + 1; i < drv->state_count; i++) {

		/* 
		 * Ignore the disabled states
		 */
		if (drv->states[i].disabled || dev->states_usage[i].disable)
			continue;

		/*
		 * Ignore the states which did not fit the latency
		 * constraint. As the idle states array is ordered, we
		 * know the deeper idle state will have a greater exit
		 * latency, so no need to continue the loop because
		 * none of next idle states will fit the latency
		 * requirement.
		 */
		if (drv->states[i].exit_latency > times->latency_req)
			break;

		/*
		 * The residency is greater than the next state's
		 * target residency. We underestimate the sleep time
		 * and we could have been sleeping deeper.
		 */
		if (residency > drv->states[i].target_residency) {
			atomic_inc(&idle_predictions_under_estimate);
			return;
		}

		/*
		 * No need to continue looking at the deeper idle
		 * state as their target residency will be greater
		 * than the last one we compare to.
		 */
		break;
	}

	atomic_inc(&idle_predictions_success);
}

static int __init idle_debug(void)
{
	struct dentry *dsched, *didle;
	int ret = -1;
	
	dsched = debugfs_create_dir("sched", NULL);
	if (!dsched)
		return -1;

	didle = debugfs_create_dir("idle", dsched);
	if (!didle)
		goto out;

	if (!debugfs_create_atomic_t("predictions_under_estimate", 0600, didle,
				     &idle_predictions_under_estimate))
		goto out;

	if (!debugfs_create_atomic_t("predictions_over_estimate", 0600, didle,
				     &idle_predictions_over_estimate))
		goto out;

	if (!debugfs_create_atomic_t("predictions_success", 0600, didle,
				     &idle_predictions_success))
		goto out;

	ret = 0;
out:
	if (ret)
		debugfs_remove_recursive(dsched);

	return ret;
}

core_initcall(idle_debug)
