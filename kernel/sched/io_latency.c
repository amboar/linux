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

/*
 * That represents the resolution of the statistics in usec, the latency
 * for a bucket is BUCKET_INTERVAL * index.
 * The higher the resolution is the lesser good prediction you will have.
 * Some measurements:
 *
 * For 1ms:
 *  SSD 6Gb/s       : 99.7%
 *  SD card class 10: 97.7%
 *  SD card class 4 : 54.3%
 *  HDD on USB      : 93.6%
 *
 * For 500us:
 *  SSD 6Gb/s               : 99.9%
 *  SD card class 10        : 96.8%
 *  SD card class 4         : 55.8%
 *  HDD on USB              : 86.3%
 *
 * For 200us:
 *  SSD 6Gb/s               : 99.7%
 *  SD card class 10        : 95.5%
 *  SD card class 4         : 29.5%
 *  HDD on USB              : 66.3%
 *
 * For 100us:
 *  SSD 6Gb/s               : 85.7%
 *  SD card class 10        : 67.63%
 *  SD card class 4         : 31.4%
 *  HDD on USB              : 44.97%
 *
 * Aiming a 100% is not necessary good because we want to hit the correct
 * idle state. Setting a low resolution will group the different latencies
 * into a big interval which may overlap with the cpuidle state target
 * residency.
 *
 */
#define BUCKET_INTERVAL 200

/*
 * Number of successive hits for the same bucket. That is the thresold
 * triggering the move of the element at the beginning of the list, so
 * becoming more weighted for the statistics when guessing for the next
 * latency.
 */
#define BUCKET_SUCCESSIVE 5

/*
 * What is a bucket ?
 *
 * A bucket is an interval of latency. This interval is defined with the
 * BUCKET_INTERVAL. The bucket index gives what latency interval we have.
 * For example, if you have an index 2 and a bucket interval of 1000usec,
 * then the bucket contains the latencies 2000 and 2999 usec.
 *
 */
struct bucket {
	int hits;
	int successive_hits;
	int index;
	int average;
	struct list_head list;
};

static struct kmem_cache *bucket_cachep;

static DEFINE_PER_CPU(struct io_latency_tree, latency_trees);

/**
 * io_latency_bucket_find - Find a bucket associated with the specified index
 *
 * @index: the index of the bucket to find
 * @tsk: the task to retrieve the task list
 *
 * Returns the bucket associated with the index, NULL if no bucket is found
 */
static struct bucket *io_latency_bucket_find(struct task_struct *tsk, int index)
{
	struct list_head *list;
	struct bucket *bucket = NULL;
	struct list_head *bucket_list = &tsk->io_latency.bucket_list;

	list_for_each(list, bucket_list) {

		bucket = list_entry(list, struct bucket, list);

		if (bucket->index == index)
			return bucket;
	}

	return NULL;
}

/**
 * io_latency_bucket_alloc - Allocate a bucket
 * 
 * @index: index of the bucket to allow
 *
 * Allocate and initialize a bucket structure
 *
 * Returns a pointer to a bucket or NULL is the allocation failed
 */
static struct bucket *io_latency_bucket_alloc(int index)
{
	struct bucket *bucket;

	bucket = kmem_cache_alloc(bucket_cachep, GFP_KERNEL);
	if (bucket) {
		bucket->hits  = 0;
		bucket->successive_hits = 0;
		bucket->index = index;
		bucket->average = 0;
		INIT_LIST_HEAD(&bucket->list);
	}

	return bucket;
}

/**
 * io_latency_guessed_bucket - try to predict the next bucket
 *
 * @tsk: the task to get the bucket list
 *
 * The list is ordered by history. The first element is the one with
 * the more *successive* hits. This function is called each time a new
 * latency is inserted. The algorithm is pretty simple here: As the
 * first element is the one which more chance to occur next, its
 * weight is the bigger, the second one has less weight, etc ...
 *
 * The bucket which has the maximum score (number of hits weighted by
 * its position in the list) is the next bucket which has more chances
 * to occur.
 *
 * Returns a pointer to the bucket structure, NULL if there are no
 * buckets in the list
 */
static struct bucket *io_latency_guessed_bucket(struct task_struct *tsk)
{
	int weight = 0;
	int score, score_max = 0;
	struct bucket *bucket, *winner = NULL;
	struct list_head *list = NULL;
	struct list_head *bucket_list = &tsk->io_latency.bucket_list;

	if (list_empty(bucket_list))
		return NULL;

	list_for_each(list, bucket_list) {

		bucket = list_entry(list, struct bucket, list);

		/*
		 * The list is ordered by history, the first element has
		 * more weight the next one
		 */
		score = bucket->hits / ((2 * weight) + 1);

		weight++;

		if (score < score_max)
			continue;

		score_max = score;
		winner = bucket;
	}

	return winner;
}

/*
 * io_latency_bucket_index - Returns the bucket index for the specified latency
 *
 * @latency: the latency fitting a bucket with the specified index
 *
 * Returns an integer for the bucket's index
 */
static int io_latency_bucket_index(int latency)
{
	return latency / BUCKET_INTERVAL;
}

/*
 * io_latency_bucket_fill - Compute and fill the bucket list
 *
 * @tsk: the task completing an IO
 * @latency: the latency of the IO
 *
 * The dynamic of the list is the following.
 * - Each new element is inserted at the end of the list
 * - Each element passing <BUCKET_SUCCESSIVE> times in this function
 *   is elected to be moved at the beginning at the list
 *
 * Returns 0 on success, -1 if a bucket allocation failed
 */
static int io_latency_bucket_fill(struct task_struct *tsk, int latency)
{
	int diff, index = io_latency_bucket_index(latency);
	struct bucket *bucket;

	/*
	 * Find the bucket associated with the index
	 */
	bucket = io_latency_bucket_find(tsk, index);
	if (!bucket) {
		bucket = io_latency_bucket_alloc(index);
		if (!bucket)
			return -1;

		list_add_tail(&bucket->list, &tsk->io_latency.bucket_list);
	}

	/*
	 * Increase the number of times this bucket has been hit
	 */
	bucket->hits++;
	bucket->successive_hits++;

	/*
	 * Compute a sliding average for latency in this bucket
	 */
	diff = latency - bucket->average;
	bucket->average += (diff >> 6);

	/*
	 * We hit a successive number of times the same bucket, move
	 * it at the beginning of the list
	 */
	if (bucket->successive_hits == BUCKET_SUCCESSIVE) {
		list_move(&bucket->list, &tsk->io_latency.bucket_list);
		bucket->successive_hits = 1;
	}

	return 0;
}

/*
 * exit_io_latency - free ressources when the task exits
 *
 * @tsk : the exiting task
 *
 */
void exit_io_latency(struct task_struct *tsk)
{
	struct list_head *bucket_list = &tsk->io_latency.bucket_list;
	struct list_head *tmp, *list;
	struct bucket *bucket;

	list_for_each_safe(list, tmp, bucket_list) {

		list_del(list);
		bucket = list_entry(list, struct bucket, list);
		kmem_cache_free(bucket_cachep, bucket);
	}
}

/**
 * io_latency_init : initialization routine
 *
 * Initializes the cache pool and the io latency rb trees.
 */
void io_latency_init(void)
{
	int cpu;
	struct io_latency_tree *latency_tree;
	struct rb_root *root;

	bucket_cachep = KMEM_CACHE(bucket, SLAB_PANIC);

	for_each_possible_cpu(cpu) {
		latency_tree = &per_cpu(latency_trees, cpu);
		latency_tree->left_most = NULL;
		spin_lock_init(&latency_tree->lock);
		root = &latency_tree->tree;
		root->rb_node = NULL;
	}
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
static void io_latency_avg(struct task_struct *tsk)
{
	struct io_latency_node *node = &tsk->io_latency;
	s64 latency = ktime_to_us(ktime_sub(node->end_time, node->start_time));
	struct bucket *bucket;

	io_latency_bucket_fill(tsk, latency);

	bucket = io_latency_guessed_bucket(tsk);
	if (bucket)
		node->avg_latency = bucket->average;
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

	io_latency_avg(tsk);
}
