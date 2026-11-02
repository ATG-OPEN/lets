/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * Scheduler host test: every task sleeps. A pass that runs no task must still
 * read the clock, or no task ever wakes again (the board's link and shell all
 * sleep between calls). The alarm fails the test instead of hanging it.
 */
#include "../kernel/task.h"
#include "../kernel/alloc.h"
#include "../vfs/vfs.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

ALLOC_INIT_HEAP(4096);

static uint64_t now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static int wakes;

static int sleeper(void *arg, uint64_t now)
{
	(void)arg;
	(void)now;

	if (++wakes == 20) {
		fprintf(stderr, "sleep: ok\n");
		exit(0);
	}

	return 5;
}

static int other(void *arg, uint64_t now)
{
	(void)arg;
	(void)now;
	return 7;
}

static void stuck(int sig)
{
	(void)sig;
	fprintf(stderr, "sleep: FAILED, the tasks stopped waking after %d wakes\n", wakes);
	_exit(1);
}

int main(void)
{
	signal(SIGALRM, stuck);
	alarm(2);

	task_initialize(now_us);
	vfs_initialize();
	task_create("sleeper", sleeper, NULL);
	task_create("other", other, NULL);
	tasks_start();
	return 1;
}
