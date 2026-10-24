/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * ls [-a] [-l]: the node table. Plain: the names, two spaces apart. -l: one
 * node per line, its handle, what it can do and its name. -a: the hidden
 * nodes too (the commands, stdin / stdout / stderr).
 */

#include "ls.h"

#include "../fsi/fsi.h"

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* one line of -l: handle, then o c r w s x for each handler it has, h when hidden */
static void ls_long(int i, const fsi_node_t *n)
{
	const fsi_handler_t *h = n->handlers;
	char can[8];

	can[0] = h->open_handler ? 'o' : '-';
	can[1] = h->close_handler ? 'c' : '-';
	can[2] = h->read_handler ? 'r' : '-';
	can[3] = h->write_handler ? 'w' : '-';
	can[4] = h->lseek_handler ? 's' : '-';
	can[5] = h->execute_handler ? 'x' : '-';
	can[6] = n->hidden ? 'h' : '-';
	can[7] = '\0';

	printf("%2d  %s  %s\n", i, can, n->name);
}

int ls(void *args, int argc, char *argv[])
{
	bool hidden = false; /* -a */
	bool list = false;   /* -l */

	for (int i = 1; i < argc; i++) {
		const char *c = argv[i];

		if (*c++ != '-' || *c == '\0') {
			printf("usage: ls [-a] [-l]\n");
			return -1;
		}

		for (; *c != '\0'; c++) {
			if (*c == 'a') {
				hidden = true;
			} else if (*c == 'l') {
				list = true;
			} else {
				printf("usage: ls [-a] [-l]\n");
				return -1;
			}
		}
	}

	fsi_node_t *n = fsi_nodes_get();
	int count = 0;

	for (int i = 0; i < FSI_MAX_NODES_COUNT; i++) {
		if (n[i].used != FSI_NODE_USED || n[i].handlers == NULL || (n[i].hidden && !hidden)) {
			continue;
		}

		if (list) {
			ls_long(i, &n[i]);
		} else {
			printf("%s%s", count > 0 ? "  " : "", n[i].name);
		}

		count++;
	}

	if (!list && count > 0) {
		printf("\n");
	}

	return count;
}
