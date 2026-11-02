/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * See top.h. The lines are written by a task of their own, one at a time and
 * straight to the console handle: a DRN port takes only so many packets (one
 * per line) before its queue is full and a write returns 0. Then the line is
 * kept and written again on the next call, so no line is lost however many
 * tasks there are. The shell waits for the task (shell_wait()), so the prompt
 * comes after the last line.
 */

#include "top.h"
#include "date.h"
#include "shell.h"

#include "../kernel/task.h"
#include "../kernel/alloc.h"
#include "../vfs/vfs.h"

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#define TOP_LINE_SIZE 96u /* a line and its end, well inside a DRN payload */

enum { TOP_UPTIME, TOP_MEMORY, TOP_HEADER, TOP_TASKS };

typedef struct {
	uint8_t step;	/* TOP_*: the line being made */
	uint8_t slot;	/* TOP_TASKS: the next task table slot to look at */
	uint8_t length; /* of line, 0: make the next one */
	char line[TOP_LINE_SIZE];
} top_t;

/*
 * The text is put together by hand, not with printf(): on a PIC24 a line by
 * snprintf() took 11 ms, long enough for a UART to fill its receive queue
 * while every other task waited.
 */
typedef struct {
	char *buf;
	uint8_t n;
	uint8_t max;
} top_text_t;

static void top_put(top_text_t *t, const char *text)
{
	while (*text != '\0' && t->n < t->max) {
		t->buf[t->n++] = *text++;
	}
}

/* @p v in decimal, at least @p width digits wide, padded with @p fill */
static void top_uint(top_text_t *t, uint32_t v, uint8_t width, char fill)
{
	char d[10];
	uint8_t k = 0;

	do {
		d[k++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v > 0);

	while (width > k && t->n < t->max) {
		t->buf[t->n++] = fill;
		width--;
	}

	while (k > 0 && t->n < t->max) {
		t->buf[t->n++] = d[--k];
	}
}

/* a time in us as "850us", "1.25ms", "125ms" or "12.5s" */
static void top_time(top_text_t *t, uint32_t us)
{
	if (us < 1000u) {
		top_uint(t, us, 0, ' ');
		top_put(t, "us");
	} else if (us < 10000u) {
		top_uint(t, us / 1000u, 0, ' ');
		top_put(t, ".");
		top_uint(t, us / 10u % 100u, 2, '0');
		top_put(t, "ms");
	} else if (us < 1000000u) {
		top_uint(t, us / 1000u, 0, ' ');
		top_put(t, "ms");
	} else {
		top_uint(t, us / 1000000u, 0, ' ');
		top_put(t, ".");
		top_uint(t, us / 100000u % 10u, 0, ' ');
		top_put(t, "s");
	}
}

/* part of whole in %, as "40%" */
static void top_percent(top_text_t *t, uint32_t part, uint32_t whole)
{
	top_uint(t, whole ? part * 100u / whole : 0u, 0, ' ');
	top_put(t, "%");
}

/* @p text in a column of @p width, to the right or the left; a space after it */
static void top_column(top_text_t *line, const char *text, uint8_t length, uint8_t width, bool right)
{
	uint8_t pad = (length < width) ? (uint8_t)(width - length) : 0u;

	for (; right && pad > 0 && line->n < line->max; pad--) {
		line->buf[line->n++] = ' ';
	}

	for (uint8_t i = 0; i < length && i < width && line->n < line->max; i++) {
		line->buf[line->n++] = text[i];
	}

	for (; pad > 0 && line->n < line->max; pad--) {
		line->buf[line->n++] = ' ';
	}

	top_put(line, " ");
}

/* why the chip last reset, from RCON (PIC24 / dsPIC33); "-" on a PC */
static const char *top_reset(uint16_t rcon)
{
	static const struct {
		uint16_t bit;
		const char *name;
	} causes[] = {
		{0x8000u, "trap"},     {0x4000u, "illegal opcode"}, {0x0200u, "config mismatch"},
		{0x0080u, "MCLR pin"}, {0x0040u, "software"},	   {0x0010u, "watchdog"},
		{0x0002u, "brown-out"}, {0x0001u, "power-on"},
	};

	for (unsigned int i = 0; i < sizeof(causes) / sizeof(causes[0]); i++) {
		if (rcon & causes[i].bit) {
			return causes[i].name;
		}
	}

	return "-";
}

static const char *top_state(uint8_t state)
{
	return (state == TASK_RUNNING) ? "run" : (state == TASK_SLEEPING) ? "sleep" : "stop";
}

/* one task's row */
static void top_row(top_text_t *line, uint8_t slot, const task_t *task)
{
	char c[12];
	top_text_t cell = {c, 0, sizeof(c)};

	top_uint(&cell, slot, 0, ' ');
	top_column(line, c, cell.n, 3, true);
	top_column(line, task->info.name, (uint8_t)strlen(task->info.name), 15, false);

	const char *state = top_state(task->state);
	top_column(line, state, (uint8_t)strlen(state), 5, false);

	cell.n = 0;
	top_uint(&cell, (uint32_t)alloc_get_owner_size(slot), 0, ' ');
	top_put(&cell, "B");
	top_column(line, c, cell.n, 5, true);

	const uint32_t times[3] = {task->info.duration, task->info.worst, task->info.delay};

	for (int i = 0; i < 3; i++) {
		cell.n = 0;
		top_time(&cell, times[i]);
		top_column(line, c, cell.n, 7, true);
	}

	/* its share of the time: its last call over the time between its last two calls, in 0.1% */
	uint32_t d = task->info.duration, p = task->info.delay;
	cell.n = 0;

	if (p > 0) {
		uint32_t pm = (p >= 1000u) ? d / (p / 1000u) : d * 1000u / p;

		pm = (pm > 1000u) ? 1000u : pm;
		top_uint(&cell, pm / 10u, 0, ' ');
		top_put(&cell, ".");
		top_uint(&cell, pm % 10u, 0, ' ');
		top_put(&cell, "%");
	} else {
		top_put(&cell, "-");
	}

	top_column(line, c, cell.n, 5, true);
}

/* makes the next line in s->line; false when there is none */
static bool top_make(top_t *s)
{
	top_text_t l = {s->line, 0, TOP_LINE_SIZE - 3u}; /* room for the end */

	switch (s->step) {
	case TOP_UPTIME: {
		uint32_t up = date_uptime();
		uint32_t loop = (uint32_t)(task_get_loop_time() * 1000.0f);

		top_put(&l, "up ");
		top_uint(&l, up / 86400u, 0, ' ');
		top_put(&l, "d ");
		top_uint(&l, up / 3600u % 24u, 2, '0');
		top_put(&l, ":");
		top_uint(&l, up / 60u % 60u, 2, '0');
		top_put(&l, ":");
		top_uint(&l, up % 60u, 2, '0');
		top_put(&l, "  tasks ");
		top_uint(&l, (uint32_t)task_count(), 0, ' ');
		top_put(&l, "  loop ");
		top_time(&l, loop);

		if (loop > 0) {
			top_put(&l, " (");
			top_uint(&l, 1000000u / loop, 0, ' ');
			top_put(&l, "/s)");
		}

		top_put(&l, "  reset ");
		top_put(&l, top_reset(task_reset_cause()));
		s->step = TOP_MEMORY;
		break;
	}

	case TOP_MEMORY: {
		uint32_t heap = (uint32_t)alloc_heap_size();
		uint32_t used = heap - (uint32_t)alloc_free_size();
		size_t peak, size;

		top_put(&l, "heap ");
		top_uint(&l, used, 0, ' ');
		top_put(&l, " of ");
		top_uint(&l, heap, 0, ' ');
		top_put(&l, " B used (");
		top_percent(&l, used, heap);
		top_put(&l, ")");

		if (task_stack(&peak, &size) == 0) {
			top_put(&l, "  stack ");
			top_uint(&l, (uint32_t)peak, 0, ' ');
			top_put(&l, " of ");
			top_uint(&l, (uint32_t)size, 0, ' ');
			top_put(&l, " B (");
			top_percent(&l, (uint32_t)peak, (uint32_t)size);
			top_put(&l, ")");
		}

		s->step = TOP_HEADER;
		break;
	}

	case TOP_HEADER:
		top_put(&l, " ID NAME            STATE   RAM    LAST     MAX  PERIOD   CPU");
		s->step = TOP_TASKS;
		break;

	default: {
		task_t *t = task_get_data();

		while (s->slot < TASKS_MAX_COUNT && t[s->slot].state == TASK_UNUSED) {
			s->slot++;
		}

		if (s->slot >= TASKS_MAX_COUNT) {
			return false;
		}

		top_row(&l, s->slot, &t[s->slot]);

		/* no trailing space */
		while (l.n > 0 && l.buf[l.n - 1] == ' ') {
			l.n--;
		}

		s->slot++;
		break;
	}
	}

	/* the line end: a terminal needs "\r\n" */
	if (vfs_terminal()) {
		s->line[l.n++] = '\r';
	}

	s->line[l.n++] = '\n';
	s->length = l.n;

	return true;
}

static int top_task(void *args, uint64_t system_time)
{
	top_t *s = args;
	int out = vfs_stream(1);

	(void)system_time;

	/* one line a call: the other tasks run in between */
	if (s->length == 0 && !top_make(s)) {
		return -1; /* all written */
	}

	int n = vfs_fd_write(out, s->line, s->length);

	if (n < 0) {
		return -1; /* the console is gone */
	}

	if (n == 0) {
		return 5; /* its queue is full: the same line again soon */
	}

	s->length = 0;

	return 1;
}

int top(void *args, int argc, char *argv[])
{
	(void)args;

	if (argc == 2 && strcmp(argv[1], "mem") == 0) {
		size_t peak, size;

		alloc_info();

		if (task_stack(&peak, &size) == 0) {
			printf("stack %u of %u B\n", (unsigned)peak, (unsigned)size);
		}

		printf("reset %04X\n", task_reset_cause());

		return 0;
	}

	if (argc != 1) {
		printf("usage: top [mem]\n");
		return -1;
	}

	top_t *s = malloc(sizeof(top_t));

	if (s == NULL) {
		printf("out of memory\n");
		return -1;
	}

	memset(s, 0, sizeof(top_t));

	/* what the shell has in stdout goes first */
	vfs_stdout_flush();

	task_t *t = task_create("top", top_task, s);

	if (t == NULL) {
		free(s);
		printf("no free task slot\n");
		return -1;
	}

	shell_wait(t);

	return 0;
}
