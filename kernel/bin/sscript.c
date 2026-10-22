/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "sscript.h"

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int sscript(void *args, uint64_t system_time)
{
	if (args == NULL) {
		return -1;
	}

	char *script = args;

	char *p = script;
	char *line = script; /* start of the current line */

	while (*p != '\0') {
		if (*p == '\n') {
			*p = '\0';

			if (line[0] != '\0') {
				system(line);
			}

			line = p + 1;
		}

		p++;
	}

	/* the last line, when the script does not end with '\n' */
	if (line[0] != '\0') {
		system(line);
	}

	return -1;
}
