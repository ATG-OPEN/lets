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

#if defined(__XC16__)
#include <xc.h>

#define TASK_STACK_PATTERN 0x5AA5u
#define TASK_RESET_FLAGS   0xC2DFu /* in RCON: the flags, not the settings */

extern char _SP_init; /* from the linker: the bottom of the stack */
#endif

static uint32_t task_loop_time = 0; /* duration of the last loop, in us */
static task_t tasks[TASKS_MAX_COUNT];
static int current = -1; /* index of the running task, -1 outside a task */
static task_streams_t task_main = {-1, -1, -1, 0}; /* the streams outside a task */
static uint16_t task_reset; /* RCON at start-up */

/* vfs: a new task holds its streams open, one that stopped lets them go */
extern void vfs_streams_hold(int in, int out) __attribute__((weak));
extern void vfs_streams_close(int in, int out) __attribute__((weak));

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
	task_streams_t s = tasks[i].streams;

	tasks[i].state = TASK_UNUSED;

	alloc_free_owner(i);

	if (vfs_streams_close != NULL) {
		vfs_streams_close(s.in, s.out);
	}
}

/* -------------------------------------------------------------------------- */

void task_initialize(uint64_t (*handle)(void))
{
	if (handle == NULL) {
		return;
	}

	task_get_us_from_system = handle;

#if defined(__XC16__)
	task_reset = RCON;
	RCON &= (uint16_t)~TASK_RESET_FLAGS;

	/* what is above the stack pointer now: task_stack() reads how far it was used */
	for (uint16_t *p = (uint16_t *)WREG15 + 1; p < (uint16_t *)SPLIM; p++) {
		*p = TASK_STACK_PATTERN;
	}
#endif

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

task_streams_t *task_streams(void)
{
	return (current >= 0 && current < (int)TASKS_MAX_COUNT) ? &tasks[current].streams : &task_main;
}

bool task_stopping(void)
{
	return current >= 0 && current < (int)TASKS_MAX_COUNT && tasks[current].state == TASK_STOPPING;
}

void *task_alloc(size_t size)
{
	void *p = malloc(size);

	if (p != NULL && current >= 0 && current < (int)TASKS_MAX_COUNT) {
		alloc_change_owner(p, current);
	}

	return p;
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

int task_stop_byname(char *name)
{
	if (name == NULL) {
		return -1;
	}

	for (int i = 0; i < TASKS_MAX_COUNT; i++) {
		task_t *t = &tasks[i];

		if (t->state == TASK_UNUSED || t->state == TASK_STOPPING) {
			continue;
		}

		if (strcmp(t->info.name, name) != 0) {
			continue;
		}

		/* it is running: its memory and streams are in use until it returns */
		if (i == current) {
			t->state = TASK_STOPPING;
		} else {
			task_release(i);
		}

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
		t->streams = *task_streams();
		t->info.worst = 0;

		task_set_name(t, name);

		if (vfs_streams_hold != NULL) {
			vfs_streams_hold(t->streams.in, t->streams.out);
		}

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

int task_stack(size_t *peak, size_t *size)
{
#if defined(__XC16__)
	uint16_t *start = (uint16_t *)&_SP_init;
	uint16_t *end = (uint16_t *)SPLIM;
	uint16_t *p = end;

	while (p > start && p[-1] == TASK_STACK_PATTERN) {
		p--;
	}

	*peak = (size_t)((char *)p - (char *)start);
	*size = (size_t)((char *)end - (char *)start);

	return 0;
#else
	(void)peak;
	(void)size;

	return -1;
#endif
}

uint16_t task_reset_cause(void)
{
	return task_reset;
}

void tasks_start(void)
{
	uint64_t now = task_get_system_time();
	uint32_t loop_start = (uint32_t)now;

	for (;;) {
		/*
		 * Read the clock on every pass: a pass where every task sleeps runs
		 * none of them, and a stale time would never wake one again.
		 */
		now = task_get_system_time();

		/* time of the previous loop, for task_get_loop_time() */
		task_loop_time = (uint32_t)now - loop_start;
		loop_start = (uint32_t)now;

#if defined(__XC16__)
		__asm__ volatile("clrwdt"); /* a task that never returns resets the board */
#endif

		for (current = 0; current < TASKS_MAX_COUNT; current++) {
			task_t *t = &tasks[current];

			if (t->state == TASK_UNUSED) {
				continue;
			}

			uint32_t start = (uint32_t)now;

			/* a sleeping task wakes once its time has come; the time wraps */
			if (t->state == TASK_SLEEPING) {
				if ((int32_t)(start - t->wake_tick) < 0) {
					continue;
				}

				t->state = TASK_RUNNING;
			}

			t->info.delay = start - t->info.last_call_time;

			int r = t->entry(t->arg, now);

			now = task_get_system_time();

			/* negative result: the task is done; it may have stopped itself */
			if (r < 0 || t->state == TASK_STOPPING) {
				task_release(current);

				continue;
			}

			uint32_t end = (uint32_t)now;

			t->info.duration = end - start;
			t->info.last_call_time = start;

			if (t->info.duration > t->info.worst) {
				t->info.worst = t->info.duration;
			}

			/*
			 * Positive result: sleep that many ms, counted from the last
			 * wake, so a periodic task keeps its period whatever it took.
			 * One that fell behind starts over from now.
			 */
			if (r > 0) {
				uint32_t wait = (uint32_t)r * 1000u;
				uint32_t wake = t->wake_tick + wait;

				if ((int32_t)(end - wake) >= 0 || (int32_t)(wake - end) > (int32_t)wait) {
					wake = end + wait;
				}

				t->state = TASK_SLEEPING;
				t->wake_tick = wake;
			}
		}
	}
}
