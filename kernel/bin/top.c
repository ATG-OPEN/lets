/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "top.h"

#include "../task.h"
#include "../alloc.h"

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int top(void *args, int argc, char *argv[])
{
	if (argc > 2) {
		return -1;
	}

	if (argc == 2) {
		if (strcmp(argv[1], "mem") != 0) {
			return -1;
		}

		alloc_info();

		return 0;
	}

	task_t *t = task_get_data();
	float loop = task_get_loop_time();

	/* summary: tasks, free heap, heap, loop time (ms), loops per second */
	printf("%u\t\t%uB\t\t%uB\t\t%.2f\t\t%u\n", (unsigned)task_count(),
	       (unsigned)alloc_free_size(), (unsigned)alloc_heap_size(), (double)loop,
	       (loop > 0) ? (unsigned)(1000 / loop) : 0u);

	printf("ID\t\tNAME\t\tRAM\t\tDUR\t\tPER\n");

	for (int i = 0; i < TASKS_MAX_COUNT; i++) {
		if (t[i].state == TASK_UNUSED) {
			continue;
		}

		printf("%i\t\t%s\t\t%uB\t\t%.2fms\t\t%.2fms\n", i, t[i].info.name,
		       (unsigned)alloc_get_owner_size(i), (double)t[i].info.duration,
		       (double)t[i].info.delay);
	}

	return 0;
}
