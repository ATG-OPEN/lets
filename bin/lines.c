/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/* See lines.h. */

#include "lines.h"
#include "shell.h"

#include "../kernel/task.h"
#include "../vfs/vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LINES_DONE, LINES_WROTE, LINES_FULL, LINES_GONE };

void *lines_alloc(unsigned int size, lines_next_t next)
{
	lines_t *l = malloc(size);

	if (l == NULL) {
		printf("out of memory\n");
		return NULL;
	}

	memset(l, 0, size);
	l->next = next;

	return l;
}

/* the next piece, or the rest of this one, to stdout */
static int lines_step(lines_t *l)
{
	if (l->length == 0) {
		unsigned int n = l->next(l, l->piece, LINES_SIZE);

		if (n == 0) {
			return LINES_DONE;
		}

		if (n == LINES_LATER) {
			return LINES_FULL; /* nothing to write yet: the same question again later */
		}

		n = (n > LINES_SIZE) ? LINES_SIZE : n;

		/* a terminal needs "\r\n" to start a line */
		if (l->piece[n - 1] == '\n' && vfs_terminal()) {
			l->piece[n - 1] = '\r';
			l->piece[n++] = '\n';
		}

		l->length = (uint8_t)n;
		l->at = 0;
	}

	int n;

#if !defined(__XC16__)
	/* a PC's printf() is not ours: without a stream of the task's own it goes to the PC's stdout */
	if (task_streams()->out < 0) {
		n = printf("%.*s", (int)(l->length - l->at), l->piece + l->at);
		n = (n < 0) ? -1 : l->length - l->at;
	} else
#endif
	{
		n = vfs_fd_write(vfs_stream(1), l->piece + l->at, (unsigned int)(l->length - l->at));
	}

	if (n < 0) {
		return LINES_GONE;
	}

	if (n == 0) {
		return LINES_FULL; /* its queue is full: the same piece again later */
	}

	l->at = (uint8_t)(l->at + n);

	if (l->at >= l->length) {
		l->length = 0;
	}

	return LINES_WROTE;
}

static int lines_task(void *arg, uint64_t system_time)
{
	(void)system_time;

	/* a few pieces a call: the other tasks run in between */
	for (int i = 0; i < 4; i++) {
		int r = lines_step(arg);

		if (r == LINES_DONE || r == LINES_GONE) {
			return -1; /* the task's end frees arg */
		}

		if (r == LINES_FULL) {
			return 5;
		}
	}

	return 0;
}

int lines_run(const char *name, lines_t *l)
{
	if (l == NULL) {
		return -1;
	}

	/* what stdout already holds goes first */
	vfs_stdout_flush();

	/* inside the command while the stream takes everything */
	for (;;) {
		int r = lines_step(l);

		if (r == LINES_DONE || r == LINES_GONE) {
			free(l);
			return (r == LINES_DONE) ? 0 : -1;
		}

		if (r == LINES_FULL) {
			break;
		}
	}

	/* the stream is full: a task goes on with it, the shell waits for that */
	task_t *t = task_create(name, lines_task, l);

	if (t == NULL) {
		free(l);
		printf("no free task slot\n");
		return -1;
	}

	shell_wait(t);

	return 0;
}
