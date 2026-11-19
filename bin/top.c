/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * See top.h. The lines are the kernel's (/proc: stat, meminfo, then one file per task), read
 * one at a time and written as lines.h does: a packet port takes only so many packets (one per
 * line) before its queue is full and a write returns 0; then the line is written again a little
 * later, so no line is lost however many tasks there are. It all happens in a task of its own,
 * which shows among the others; the shell waits for it, so the prompt comes after the last line.
 */

#include "top.h"
#include "lines.h"

#include "../lib/posix.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TOP_HEADER " ID NAME            STATE   RAM    LAST     MAX  PERIOD   CPU\n"
#define TOP_IDS	   100u /* /proc/0 .. /proc/99: the task slots there can be */

#if !VFS_PROC
/* a board without the /proc mount (VFS_PROC 0, vfs.h) asks the kernel for the same text */
int vfs_proc_line(unsigned int line, char *buf, unsigned int max);
void vfs_proc_mem(void);
#endif

typedef struct {
	lines_t lines;
	uint8_t step; /* 0 stat, 1 meminfo, 2 the header, 3 + k task slot k */
} top_t;

/* line @p i of `top` but the header (0 stat, 1 meminfo, 2 + k slot k): its length, 0 or -1 */
static int top_line(unsigned int i, char *piece, unsigned int max)
{
#if VFS_PROC
	char path[16];

	if (i < 2u) {
		strcpy(path, (i == 0u) ? "/proc/stat" : "/proc/meminfo");
	} else {
		snprintf(path, sizeof(path), "/proc/%u", i - 2u);
	}

	int fd = open(path, O_RDONLY, 0);
	int n = (fd < 0) ? 0 : read(fd, piece, max);

	close(fd);

	return (i - 2u >= TOP_IDS && i >= 2u) ? -1 : n;
#else
	return vfs_proc_line(i, piece, max + 1u);
#endif
}

static unsigned int top_next(lines_t *l, char *piece, unsigned int max)
{
	top_t *s = (top_t *)l;

	for (int n = 0; n >= 0; s->step++) {
		if (s->step == 2u) {
			s->step++;
			strcpy(piece, TOP_HEADER);
			return sizeof(TOP_HEADER) - 1u;
		}

		n = top_line(s->step - ((s->step > 2u) ? 1u : 0u), piece, max);

		if (n > 0) {
			s->step++;
			piece[n - 1] = '\n'; /* a line cut short ends too */
			return (unsigned int)n;
		}
	}

	return 0;
}

int top(void *args, int argc, char *argv[])
{
	(void)args;

	if (argc == 2 && strcmp(argv[1], "mem") == 0) {
#if VFS_PROC
		char buf[32];
		int fd = open("/proc/mem", O_RDONLY, 0);
		int n;

		while (fd >= 0 && (n = read(fd, buf, sizeof(buf))) > 0) {
			write(1, buf, (unsigned int)n);
		}

		return close(fd);
#else
		vfs_proc_mem();

		return 0;
#endif
	}

	if (argc != 1) {
		printf("usage: top [mem]\n");
		return -1;
	}

	top_t *s = lines_alloc(sizeof(top_t), top_next);

	return (s == NULL) ? -1 : lines_spawn("top", &s->lines);
}
