/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * cat [-x] NODE: one read of a node, printed as text or, with -x, as hex
 * bytes. A node gives its whole content in one read (a RAM file, a sensor
 * value), so one read is all cat does; vfs has its own cat for files.
 */

#include "cat.h"

#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define CAT_BUFFER_SIZE 128 /* most bytes printed, with the 0 */

int cat(void *args, int argc, char *argv[])
{
	bool hex = (argc == 3 && strcmp(argv[1], "-x") == 0);

	if (argc != (hex ? 3 : 2) || strcmp(argv[1], "-help") == 0) {
		printf("usage: cat [-x] NODE\n");
		return -1;
	}

	int fd = open(argv[argc - 1], 0, 0);

	if (fd < 0) {
		printf("open failed\n");
		return -1;
	}

	char *buffer = malloc(CAT_BUFFER_SIZE);

	if (buffer == NULL) {
		close(fd);
		printf("out of memory\n");
		return -1;
	}

	int size = read(fd, buffer, CAT_BUFFER_SIZE - 1);

	close(fd);

	if (size > 0 && !hex) {
		buffer[size] = '\0';
		printf("%s%s", buffer, (buffer[size - 1] == '\n') ? "" : "\n");
	}

	for (int i = 0; hex && i < size; i++) {
		printf("%02X%c", (unsigned char)buffer[i], (i == size - 1) ? '\n' : ' ');
	}

	free(buffer);

	return size;
}
