/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "kill.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <string.h>
#include <stdio.h>

#define KILL_IDS 100u /* /proc/0 .. /proc/99: the task slots there can be */

/* the task called @p name: its ID, found in its row of /proc (as `top` shows it), or -1 */
static int kill_find(const char *name)
{
	size_t length = strlen(name);
	char row[24]; /* "ID " and the name: " 12 " + 15 + ' ' */

	for (unsigned int id = 0; id < KILL_IDS && length <= 15u; id++) {
		snprintf(row, sizeof(row), "/proc/%u", id);

		int fd = open(row, O_RDONLY, 0);
		int n = (fd < 0) ? -1 : read(fd, row, sizeof(row) - 1u);

		close(fd);

		/* the name: from the 5th character, then a blank */
		if (n > 4 && (row[n] = '\0', strncmp(row + 4, name, length) == 0) &&
		    row[4 + length] == ' ' && kill((pid_t)id, 0) == 0) {
			return (int)id;
		}
	}

	return -1;
}

int kill_execute(void *args, int argc, char *argv[])
{
	if (argc != 2 || strcmp(argv[1], "-help") == 0) {
		printf("usage: kill TASK\n");
		return -1;
	}

	int id = kill_find(argv[1]);

	if (id < 0 || kill((pid_t)id, SIGTERM) < 0) {
		printf("not found\n");
		return -1;
	}

	return 0;
}
