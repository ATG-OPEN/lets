/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * echo WORDS [> NODE]: the words, one space apart, and a newline, to stdout
 * or to a node (`>>` is the same: a node takes what is written to it).
 */

#include "echo.h"

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define ECHO_LINE_SIZE 128 /* longest line, with the newline and the 0 */

int echo(void *args, int argc, char *argv[])
{
	int end = argc; /* the first word that is not printed */

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], ">") == 0 || strcmp(argv[i], ">>") == 0) {
			if (i != argc - 2) {
				printf("usage: echo WORDS [> NODE]\n");
				return -1;
			}

			end = i;
			break;
		}
	}

	char *line = malloc(ECHO_LINE_SIZE);

	if (line == NULL) {
		printf("out of memory\n");
		return -1;
	}

	unsigned int length = 0;

	for (int i = 1; i < end; i++) {
		unsigned int w = (unsigned int)strlen(argv[i]);

		if (length + w + 2u >= ECHO_LINE_SIZE) {
			break; /* the rest does not fit */
		}

		memcpy(line + length, argv[i], w);
		length += w;

		if (i < end - 1) {
			line[length++] = ' ';
		}
	}

	line[length++] = '\n';
	line[length] = '\0';

	int r = 0;

	if (end == argc) {
		printf("%s", line);
	} else {
		int fd = open(argv[argc - 1], 0, 0);

		if (fd < 0 || write(fd, line, length) < 0) {
			printf("open failed\n");
			r = -1;
		}

		if (fd >= 0) {
			close(fd);
		}
	}

	free(line);

	return r;
}
