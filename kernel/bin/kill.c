/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "kill.h"

#include "../task.h"

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int kill(void *args, int argc, char *argv[])
{
	if (argc != 2) {
		printf("bad argument\n");
		return -1;
	}

	if (task_stop_byname(argv[1]) < 0) {
		printf("-task not found\n");
		return -1;
	}

	return 0;
}
