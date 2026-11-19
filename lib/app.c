/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/* lets_spawn() (include/lets-app.h): a program's step as a task. */
#include "../include/lets-app.h"
#include "../kernel/task.h"

#include <stdlib.h>

typedef struct {
	lets_step_t step;
	void *ctx;
} spawn_t;

static int run(void *arg, uint64_t system_time)
{
	spawn_t *s = arg;
	int r = s->step(s->ctx);

	(void)system_time;
	if (r < 0) {
		free(s);
	}
	return r;
}

int lets_spawn(const char *name, lets_step_t step, void *ctx)
{
	spawn_t *s = malloc(sizeof(*s));

	if (s == NULL) {
		return -1;
	}
	s->step = step;
	s->ctx = ctx;
	if (task_create(name, run, s) == NULL) {
		free(s);
		return -1;
	}
	return 0;
}
