/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * Cooperative scheduler: one table of tasks and one endless loop that calls
 * each task in turn. See task.h for the rules a task follows.
 */

#include "task.h"
#include "alloc.h"

#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>

static uint64_t task_loop_time = 0; /* duration of the last loop, in us */
static task_t tasks[TASKS_MAX_COUNT];
static int current = -1; /* index of the running task, -1 outside a task */

/* the clock given to task_initialize() */
static uint64_t (*task_get_us_from_system)(void) = NULL;

/* -------------------------------------------------------------------------- */

static void task_set_name(task_t *t, const char *name)
{
	strncpy(t->info.name, name, TASK_NAME_SIZE - 1);
	t->info.name[TASK_NAME_SIZE - 1] = '\0';
}

/* frees the slot and every heap block the task owns */
static void task_release(int i)
{
	tasks[i].state = TASK_UNUSED;
	alloc_free_owner(i);
}

/* -------------------------------------------------------------------------- */

void task_initialize(uint64_t (*handle)(void))
{
	if (handle == NULL) {
		return;
	}

	task_get_us_from_system = handle;

	for (int i = 0; i < TASKS_MAX_COUNT; i++) {
		task_t *t = &tasks[i];

		t->state = TASK_UNUSED;
	}
}

uint64_t task_get_system_time(void)
{
	if (task_get_us_from_system == NULL) {
		return 0;
	}

	return task_get_us_from_system();
}

task_t *task_get_data(void)
{
	return tasks;
}

int task_get_current(void)
{
	return current;
}

int task_count(void)
{
	int count = 0;

	for (int i = 0; i < TASKS_MAX_COUNT; i++) {
		task_t *t = &tasks[i];

		if (t->state == TASK_UNUSED) {
			continue;
		}

		count++;
	}

	return count;
}

int task_current_rename(char *name)
{
	if (name == NULL || current < 0 || current >= TASKS_MAX_COUNT) {
		return -1;
	}

	task_set_name(&tasks[current], name);

	return 0;
}

int task_stop_byname(char *name)
{
	if (name == NULL) {
		return -1;
	}

	for (int i = 0; i < TASKS_MAX_COUNT; i++) {
		task_t *t = &tasks[i];

		if (t->state == TASK_UNUSED) {
			continue;
		}

		if (strcmp(t->info.name, name) != 0) {
			continue;
		}

		task_release(i);

		return 0;
	}

	return -1;
}

task_t *task_create(const char *name, int (*entry)(void *arg, uint64_t system_time), void *arg)
{
	if (name == NULL || entry == NULL) {
		return NULL;
	}

	for (int i = 0; i < TASKS_MAX_COUNT; i++) {
		task_t *t = &tasks[i];

		if (t->state != TASK_UNUSED) {
			continue;
		}

		t->entry = entry;
		t->arg = arg;
		t->state = TASK_RUNNING;
		t->wake_tick = 0;

		task_set_name(t, name);

		/* the task owns its argument: freed when the task stops */
		alloc_change_owner(arg, i);

		return t;
	}

	return NULL;
}

float task_get_loop_time(void)
{
	return (float)task_loop_time / 1000;
}

void tasks_start(void)
{
	uint64_t tasks_loop_start_time = 0;
	uint64_t tasks_loop_stop_time = 0;

	for (;;) {
		/* time of the previous loop, for task_get_loop_time() */
		tasks_loop_stop_time = task_get_system_time();
		task_loop_time = tasks_loop_stop_time - tasks_loop_start_time;
		tasks_loop_start_time = tasks_loop_stop_time;

		for (current = 0; current < TASKS_MAX_COUNT; current++) {
			task_t *t = &tasks[current];

			if (t->state == TASK_UNUSED) {
				continue;
			}

			uint64_t task_start_time = task_get_system_time();

			/* a sleeping task wakes once its time has come */
			if (t->state == TASK_SLEEPING) {
				if (task_start_time >= t->wake_tick) {
					t->state = TASK_RUNNING;
				}
			}

			if (t->state != TASK_RUNNING) {
				continue;
			}

			t->info.delay = (float)(task_start_time - t->info.last_call_time) / 1000;

			int r = t->entry(t->arg, task_start_time);

			/*
			 * Negative result: the task is done. A task that
			 * stopped itself (`kill` on its own name) is already
			 * unused: release it, never wake it again.
			 */
			if (r < 0 || t->state == TASK_UNUSED) {
				task_release(current);

				continue;
			}

			uint64_t task_exit_time = task_get_system_time();
			t->info.duration = (float)(task_exit_time - task_start_time) / 1000;
			t->info.last_call_time = task_start_time;

			/* positive result: sleep that many ms */
			if (r > 0) {
				t->state = TASK_SLEEPING;
				t->wake_tick = task_exit_time + (uint64_t)r * 1000;
			}
		}
	}
}
