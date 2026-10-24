/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * ed: a line editor in one command per edit, after Unix ed's a / i / c / d.
 * The file is read, changed in one buffer and written back once with
 * O_TRUNC, so an edit is all or nothing on a vfs file system. Nothing stays
 * in memory between commands.
 */

#include "vfs.h"

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

int vfs_ed(void *args, int argc, char *argv[])
{
	if (argc < 2 || strcmp(argv[1], "-help") == 0) {
		return ed_usage();
	}

	/* the new line: the words after the command, one space apart */
	int length = (argc > 3) ? argc - 4 : 0;

	for (int i = 3; i < argc; i++) {
		length += (int)strlen(argv[i]);
	}

	int add = length + 2; /* and its '\n', and one before it if the file has none at the end */

	int fd = open(argv[1], 0, 0);
	long end = (fd < 0) ? 0 : (long)lseek(fd, 0, SEEK_END);

	/* a file has a size; a device or a RAM file has none */
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

	if (argc == 2) {
		/* list */
		if (fd < 0) {
			printf("open failed\n");
			r = -1;
		}

		for (long n = 1; n <= lines; n++) {
			int a = ed_line(text, size, n);
			int b = ed_line(text, size, n + 1);

			int w = b - a - (text[b - 1] == '\n');

			printf("%3ld  %.*s\n", n, w, text + a);
		}

		free(text);
		return r;
	}

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

	if (fd >= 0) {
		close(fd);
	}

	free(text);

	return r;
}
