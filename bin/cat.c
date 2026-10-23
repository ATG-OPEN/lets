/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "cat.h"

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define CAT_BUFFER_SIZE 128 /* most bytes printed */

int cat(void *args, int argc, char *argv[])
{
	if (argc < 2 || argc > 3) {
		printf("usage: cat [-x] file\n");
		return -1;
	}

	int hex = 0;
	int file = 1; /* index of the node name in argv */

	if (argc == 3) {
		if (strcmp(argv[1], "-x") != 0) {
			printf("bad argument\n");
			return -1;
		}

		hex = 1;
		file = 2;
	}

	int fd = open(argv[file], 0, 0);

	if (fd < 0) {
		printf("open error\n");
		return -1;
	}

	char *buffer = malloc(CAT_BUFFER_SIZE);

	if (buffer == NULL) {
		printf("out of memory\n");
		return -1;
	}

	int size = read(fd, buffer, CAT_BUFFER_SIZE - 1);

	if (size > 0 && hex == 0) {
		buffer[size] = '\0';
		printf("%s\n", buffer);
	} else if (size > 0) {
		for (int i = 0; i < size; i++) {
			printf("%02X ", (unsigned char)buffer[i]);
		}

		printf("\n");
	}

	free(buffer);

	return size;
}
