/*
 * Copyright (c) 2014 ARM/Linaro
 *
 * Author: Daniel Lezcano <daniel.lezcano@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/percpu.h>
#include <linux/ktime.h>
#include <linux/rbtree.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "sched.h"

struct io_latency_tree {
	spinlock_t lock;
	struct rb_root tree;
	struct io_latency_node *left_most;
};

static DEFINE_PER_CPU(struct io_latency_tree, latency_trees);

/**
 * io_latency_init : initialization routine to be called for each possible cpu.
 *
 * @rq: the runqueue associated with the cpu
 *
 */
void io_latency_init(struct rq *rq)
{
	int cpu = rq->cpu;
	struct io_latency_tree *latency_tree = &per_cpu(latency_trees, cpu);
	struct rb_root *root = &latency_tree->tree;

	spin_lock_init(&latency_tree->lock);
	latency_tree->left_most = NULL;
	root->rb_node = NULL;
}

/**
 * io_latency_get_sleep_length: compute the expected sleep time
 *
 * @rq: the runqueue associated with the cpu
 *
 * Returns the minimal estimated remaining sleep time for the pending IOs
 */
s64 io_latency_get_sleep_length(struct rq *rq)
{
	int cpu = rq->cpu;
	struct io_latency_tree *latency_tree = &per_cpu(latency_trees, cpu);
	struct io_latency_node *node;
	ktime_t now = ktime_get();
	s64 diff;

	node = latency_tree->left_most;

	if (!node)
		return 0;

	diff = ktime_to_us(ktime_sub(now, node->start_time));
	diff = node->avg_latency - diff;

	/* Estimation was wrong, return 0 */
	if (diff < 0)
		return 0;

	return diff;
}

/**
 * io_latency_avg: compute the io latency sliding average value
 *
 * @node: a rb tree node belonging to a task
 *
 */
static void io_latency_avg(struct io_latency_node *node)
{
	/* MA*[i]= MA*[i-1] + X[i] - MA*[i-1]/N */
	s64 latency = ktime_to_us(ktime_sub(node->end_time, node->start_time));
	s64 diff = latency - node->avg_latency;

	node->avg_latency = node->avg_latency + (diff >> 6);
}

/**
 * io_latency_begin - insert the node in the rb tree
 *
 * @rq: the runqueue the task is running on
 * @task: the task being blocked on an IO
 *
 * Inserts the node in the rbtree in an ordered manner. If this task
 * has the minimal io latency of all the tasks blocked on IO, it falls
 * at the left most node and a shortcut is used.  Stores the start
 * time of the io schedule.
 *
 */
int io_latency_begin(struct rq *rq, struct task_struct *tsk)
{
	int cpu = rq->cpu;
	struct io_latency_tree *latency_tree = &per_cpu(latency_trees, cpu);
	struct rb_root *root = &latency_tree->tree;
	struct io_latency_node *node = &tsk->io_latency;
	struct rb_node **new = &root->rb_node, *parent = NULL;
	struct io_latency_node *lat;
	int leftmost = 1;

	node->start_time = ktime_get();

	spin_lock(&latency_tree->lock);

	while (*new) {
		lat = rb_entry(*new, struct io_latency_node, node);

		parent = *new;

		if (lat->avg_latency > node->avg_latency)
			new = &parent->rb_left;
		else {
			new = &parent->rb_right;
			leftmost = 0;
		}
	}

	if (leftmost)
		latency_tree->left_most = node;

	rb_link_node(&node->node, parent, new);
	rb_insert_color(&node->node, root);

	spin_unlock(&latency_tree->lock);

	return 0;
}

/**
 * io_latency_end - Removes the node from the rb tree
 *
 * @rq: the runqueue the task belongs to
 * @tsk: the task woken up after an IO completion
 *
 * Removes the node for the rb tree for this cpu. Update the left most
 * node with the next node if itself it is the left most
 * node. Retrieves the end time after the io has complete and update
 * the io latency average time
 */
void io_latency_end(struct rq *rq, struct task_struct *tsk)
{
	int cpu = rq->cpu;
	struct io_latency_tree *latency_tree = &per_cpu(latency_trees, cpu);
	struct rb_root *root = &latency_tree->tree;
	struct io_latency_node *old = &tsk->io_latency;

	old->end_time = ktime_get();

	spin_lock(&latency_tree->lock);

	if (latency_tree->left_most == old) {
		struct rb_node *next_node =
			rb_next(&latency_tree->left_most->node);
		latency_tree->left_most =
			rb_entry(next_node, struct io_latency_node, node);
	}

	rb_erase(&old->node, root);

	spin_unlock(&latency_tree->lock);

	io_latency_avg(old);
}
