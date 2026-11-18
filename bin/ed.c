/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * ed: a line editor in one command per edit, after Unix ed's a / i / c / d.
 * The file is copied to a new version of itself (O_TRUNC) a piece at a time,
 * with the edit made on the way, so an edit is all or nothing on a vfs file
 * system and a file larger than the free heap can be edited. Nothing stays in
 * memory between commands.
 */

#include "lines.h"

#include "../vfs/vfs.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ED_PIECE 16 /* bytes copied at a time, on the stack */

static int ed_usage(void)
{
	printf("usage: ed FILE [Na|Ni|Nc TEXT | Nd]\n");
	return -1;
}

/*
 * The listing: one numbered line (or a part of a long one) at a time, read
 * from the file each time, so a stream that takes only a few lines at once
 * (lines.h) gets them all and only one line is in memory.
 */
typedef struct {
	lines_t lines;
	uint32_t at;	/* where the next piece starts in the file */
	uint32_t size;	/* of the file */
	uint16_t line;	/* its number */
	uint8_t tries;	/* reads that failed in a row (a full heap: open() needs memory) */
	bool mid;	/* inside a line longer than a piece */
	char path[];
} ed_list_t;

static unsigned int ed_list_next(lines_t *l, char *piece, unsigned int max)
{
	ed_list_t *e = (ed_list_t *)l;
	unsigned int n = 0;

	if (!e->mid) {
		/* "%3u  " by hand: snprintf() is slow on a small MCU */
		char d[5];
		unsigned int k = 0, v = e->line;

		do {
			d[k++] = (char)('0' + v % 10u);
			v /= 10u;
		} while (v > 0 && k < sizeof(d));

		for (; n + k < 3u; n++) {
			piece[n] = ' ';
		}

		while (k > 0) {
			piece[n++] = d[--k];
		}

		piece[n++] = ' ';
		piece[n++] = ' ';
	}

	int fd = (e->at < e->size) ? open(e->path, O_RDONLY, 0) : -1;
	int got = 0;

	if (e->at < e->size) {
		got = -1;

		if (fd >= 0 && lseek(fd, (off_t)e->at, SEEK_SET) == (off_t)e->at) {
			got = read(fd, piece + n, max - n - 1u); /* and room for its '\n' */
		}

		close(fd);

		/* not now (the heap is full): again later, for a while */
		if (got <= 0 && ++e->tries < 200u) {
			return LINES_LATER;
		}
	}

	e->tries = 0;

	if (got <= 0) {
		/* the end: a long last line without '\n' still gets one */
		if (e->mid) {
			e->mid = false;
			piece[0] = '\n';
			return 1;
		}

		return 0;
	}

	const char *nl = memchr(piece + n, '\n', (size_t)got);

	if (nl != NULL) {
		got = (int)(nl - (piece + n)) + 1; /* up to its '\n' */
		e->line++;
		e->mid = false;
	} else if ((unsigned int)got < max - n - 1u) {
		piece[n + (unsigned int)got] = '\n'; /* the last line, without '\n' in the file */
		e->at += (uint32_t)got;
		e->mid = false;
		e->line++;
		return n + (unsigned int)got + 1u;
	} else {
		e->mid = true; /* the rest of it next time */
	}

	e->at += (uint32_t)got;

	return n + (unsigned int)got;
}

static int ed_list(const char *path)
{
	int fd = open(path, O_RDONLY, 0);

	if (fd < 0) {
		printf("open failed\n");
		return -1;
	}

	long end = (long)lseek(fd, 0, SEEK_END);

	close(fd);

	if (end < 0) {
		printf("not a file\n");
		return -1;
	}

	size_t length = strlen(path) + 1u;
	ed_list_t *e = lines_alloc((unsigned int)(sizeof(ed_list_t) + length), ed_list_next);

	if (e == NULL) {
		return -1;
	}

	memcpy(e->path, path, length);
	e->line = 1;
	e->size = (uint32_t)end;

	return lines_run("ed", &e->lines);
}

/* 0 when all of it was written, else -1 */
static int ed_put(int fd, const char *data, unsigned int n)
{
	return (write(fd, (void *)data, n) == (int)n) ? 0 : -1;
}

/* the new line: the words after the command, one space apart, and its '\n' */
static int ed_insert(int fd, int argc, char *argv[])
{
	int r = 0;

	for (int i = 3; i < argc; i++) {
		r |= ed_put(fd, argv[i], strlen(argv[i]));
		r |= ed_put(fd, " ", (i < argc - 1) ? 1u : 0u);
	}

	return r | ed_put(fd, "\n", 1);
}

int vfs_ed(void *args, int argc, char *argv[])
{
	if (argc < 2 || strcmp(argv[1], "-help") == 0) {
		return ed_usage();
	}

	if (argc == 2) {
		return ed_list(argv[1]);
	}

	char *op = NULL;
	long n = strtol(argv[2], &op, 10);
	char c = *op;

	/* a: after 0 .. lines; i: before 1 .. lines + 1; c, d: 1 .. lines (lines: checked at the end) */
	if (op == argv[2] || op[0] == '\0' || op[1] != '\0' || strchr("aicd", c) == NULL ||
	    n < (c == 'a' ? 0 : 1) || n > 32000 || (c == 'd' && argc != 3)) {
		return ed_usage();
	}

	char piece[ED_PIECE];
	int in = open(argv[1], O_RDONLY, 0);
	long end = (in < 0) ? 0 : (long)lseek(in, 0, SEEK_END);

	/*
	 * A file of a file system: its O_TRUNC copy below is new, so the old one can still be read.
	 * A RAM file would be emptied by that open, and a device has no lines to keep.
	 */
	if (end < 0 || (in >= 0 && (!vfs_file_is(in) || lseek(in, 0, SEEK_SET) != 0))) {
		close(in);
		printf("not a file\n");
		return -1;
	}

	/* the new line goes in before line `add` (0: none), line `drop` is left out (0: none) */
	unsigned int add = (c == 'd') ? 0u : (unsigned int)n + (c == 'a');
	unsigned int drop = (c == 'c' || c == 'd') ? (unsigned int)n : 0u;
	unsigned int line = 1;
	char last = '\n';
	int k;
	int out = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0);
	int r = (out < 0) ? -1 : 0;

	/* the file as it was, a piece at a time: the parts to keep in runs, the new line where it goes */
	while (r >= 0 && in >= 0 && (k = read(in, piece, sizeof(piece))) > 0) {
		int run = 0;

		for (int i = 0; i < k; i++) {
			bool start = (i == 0) ? (last == '\n') : (piece[i - 1] == '\n');

			if (start && (line == add || line == drop)) {
				r |= ed_put(out, piece + run, (unsigned int)(i - run));
				run = i;
			}

			if (start && line == add) {
				r |= ed_insert(out, argc, argv);
				add = 0;
			}

			if (line == drop) {
				run = i + 1;
			}

			line += (piece[i] == '\n');
		}

		r |= ed_put(out, piece + run, (unsigned int)(k - run));
		last = piece[k - 1];
	}

	close(in);

	/* the file's lines (a last one without '\n' counts too): n must have been one of them */
	unsigned int lines = line - 1u + (last != '\n');
	bool ok = (unsigned int)n <= lines + (c == 'a' || c == 'i');

	/* at the end: after a last line without '\n', give it one first */
	if (ok && add > 0 && last != '\n') {
		r |= ed_put(out, "\n", 1);
	}

	if (ok && add > 0) {
		r |= ed_insert(out, argc, argv);
	}

	/* an edit that failed must not replace what was there */
	if (r < 0 || !ok) {
		vfs_file_abort(out);
	} else if (close(out) == 0) {
		return 0;
	}

	if (!ok) {
		return ed_usage();
	}

	printf("write failed\n");

	return -1;
}
