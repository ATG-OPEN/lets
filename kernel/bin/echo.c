/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "echo.h"

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int echo(void *args, int argc, char *argv[])
{
	int fd = -1;
	int redirect = 0;

	/* `> node` sends the line to that node */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], ">") == 0 && i + 1 < argc) {
			redirect = 1;
			fd = open(argv[i + 1], 0, 0);
			if (fd < 0) {
				printf("echo: cannot open file\n");
				return -1;
			}
			break;
		}
	}

	/* the words before `>`, separated by spaces, and a newline */
	char *buffer = malloc(128);

	if (buffer == NULL) {
		return -1;
	}

	buffer[0] = '\0';

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], ">") == 0) {
			break;
		}

		strcat(buffer, argv[i]);
		if (i < argc - 1) {
			strcat(buffer, " ");
		}
	}

	strcat(buffer, "\n");

	if (redirect) {
		write(fd, buffer, strlen(buffer));
		close(fd);
	} else {
		printf("%s", buffer);
	}

	free(buffer);
	buffer = NULL;

	return 0;
}
