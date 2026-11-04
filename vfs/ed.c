/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * ed: a line editor in one command per edit, after Unix ed's a / i / c / d.
 * The file is read, changed in one buffer and written back once with
 * O_TRUNC, so an edit is all or nothing on a vfs file system. Nothing stays
 * in memory between commands.
 */

#include "vfs.h"
#include "../bin/lines.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ED_FILE_MAX
#define ED_FILE_MAX 1024 /* largest file ed takes, in bytes */
#endif

static int ed_usage(void)
{
	printf("usage: ed FILE [Na|Ni|Nc TEXT | Nd]\n");
	return -1;
}

/* where line @n (from 1) starts; past the last line: @size */
static int ed_line(const char *text, int size, long n)
{
	int at = 0;

	while (--n > 0 && at < size) {
		const char *nl = memchr(text + at, '\n', (size_t)(size - at));

		at = (nl == NULL) ? size : (int)(nl - text) + 1;
	}

	return at;
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

int vfs_ed(void *args, int argc, char *argv[])
{
	if (argc < 2 || strcmp(argv[1], "-help") == 0) {
		return ed_usage();
	}

	if (argc == 2) {
		return ed_list(argv[1]);
	}

	/* the new line: the words after the command, one space apart */
	int length = (argc > 3) ? argc - 4 : 0;

	for (int i = 3; i < argc; i++) {
		length += (int)strlen(argv[i]);
	}

	int add = length + 2; /* and its '\n', and one before it if the file has none at the end */

	int fd = open(argv[1], O_RDONLY, 0);
	long end = (fd < 0) ? 0 : (long)lseek(fd, 0, SEEK_END);

	/* a file has a size; a device has none */
	if (end < 0 || end > ED_FILE_MAX || (fd >= 0 && lseek(fd, 0, SEEK_SET) != 0)) {
		close(fd);
		printf(end < 0 ? "not a file\n" : "too long\n");
		return -1;
	}

	char *text = malloc((size_t)end + (size_t)add);

	if (text == NULL) {
		close(fd);
		printf("out of memory\n");
		return -1;
	}

	int size = (fd < 0) ? 0 : read(fd, text, (unsigned int)end);

	close(fd);
	size = (size < 0) ? 0 : size;

	int lines = 0;

	for (int i = 0; i < size; i++) {
		lines += (text[i] == '\n' || i == size - 1);
	}

	int r = 0;

	char *op = NULL;
	long n = strtol(argv[2], &op, 10);
	char c = *op;

	/* a: after 0 .. lines; i: before 1 .. lines + 1; c, d: 1 .. lines */
	bool ok = op != argv[2] && op[0] != '\0' && op[1] == '\0' && strchr("aicd", c) != NULL &&
		  n >= (c == 'a' ? 0 : 1) && n <= lines + (c == 'a' || c == 'i') &&
		  (c != 'd' || argc == 3);

	if (!ok) {
		free(text);
		return ed_usage();
	}

	int at = ed_line(text, size, (c == 'a') ? n + 1 : n);

	if (c == 'c' || c == 'd') {
		int next = ed_line(text, size, n + 1);

		memmove(text + at, text + next, (size_t)(size - next));
		size -= next - at;
	}

	if (c != 'd') {
		/* after a last line without '\n': give it one first */
		if (at == size && size > 0 && text[size - 1] != '\n') {
			text[size++] = '\n';
			at = size;
		}

		memmove(text + at + length + 1, text + at, (size_t)(size - at));

		for (int i = 3; i < argc; i++) {
			int w = (int)strlen(argv[i]);

			memcpy(text + at, argv[i], (size_t)w);
			at += w;

			if (i < argc - 1) {
				text[at++] = ' ';
			}
		}

		text[at] = '\n';
		size += length + 1;
	}

	fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0);

	if (fd < 0 || write(fd, text, (unsigned int)size) != size) {
		printf("write failed\n");
		r = -1;
	}

	/* an edit that failed must not replace what was there */
	if (r < 0) {
		vfs_file_abort(fd);
	} else {
		close(fd);
	}

	free(text);

	return r;
}
