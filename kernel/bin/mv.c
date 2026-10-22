/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "mv.h"

#include "../../fsi/fsi.h"

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int mv(void *args, int argc, char *argv[])
{
	if (argc != 3) {
		printf("bad argument\n");
		return -1;
	}

	fsi_node_t *n = fsi_nodes_get();

	for (int i = 2; i < FSI_MAX_NODES_COUNT; i++) {
		if ((n + i)->used != FSI_NODE_USED) {
			continue;
		}

		if (strcmp((n + i)->name, argv[1]) != 0) {
			continue;
		}

		strncpy((n + i)->name, argv[2], FSI_NODE_NAME_MAX_LENGTH - 1);
		(n + i)->name[FSI_NODE_NAME_MAX_LENGTH - 1] = '\0';

		return 0;
	}

	printf("not found\n");
	return -1;
}
