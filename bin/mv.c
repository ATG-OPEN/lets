/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "mv.h"

#include "../fsi/fsi.h"

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* the node named @name, -1 if none; stdin, stdout and stderr are not renamed */
static int mv_find(const char *name)
{
	fsi_node_t *n = fsi_nodes_get();

	for (int i = 3; i < FSI_MAX_NODES_COUNT; i++) {
		if (n[i].used == FSI_NODE_USED && strcmp(n[i].name, name) == 0) {
			return i;
		}
	}

	return -1;
}

int mv(void *args, int argc, char *argv[])
{
	if (argc != 3 || strcmp(argv[1], "-help") == 0) {
		printf("usage: mv NODE NEWNAME\n");
		return -1;
	}

	int i = mv_find(argv[1]);

	if (i < 0) {
		printf("not found\n");
		return -1;
	}

	size_t length = strlen(argv[2]);

	if (length == 0 || length >= FSI_NODE_NAME_MAX_LENGTH || mv_find(argv[2]) >= 0) {
		printf("bad argument\n"); /* empty, too long, or taken */
		return -1;
	}

	strcpy(fsi_nodes_get()[i].name, argv[2]);

	return 0;
}
