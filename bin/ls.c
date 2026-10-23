/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "ls.h"

#include "../fsi/fsi.h"

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int ls(void *args, int argc, char *argv[])
{
	if (argc > 2) {
		printf("bad argument\n");

		return -1;
	}

	bool hidden = false; /* -a: hidden nodes too */
	bool list = false;   /* -l: one node per line */

	if (argc == 2) {
		if (strcmp(argv[1], "-a") == 0) {
			hidden = true;
		} else if (strcmp(argv[1], "-l") == 0) {
			list = true;
		} else if (strcmp(argv[1], "-la") == 0) {
			hidden = true;
			list = true;
		} else {
			printf("bad argument\n");
			return -1;
		}
	}

	fsi_node_t *n = fsi_nodes_get();

	char *lstring = malloc(128);
	if (!lstring) {
		return -1;
	}

	int ls_size = 0;
	lstring[0] = '\0';

	if (list == false) {
		/* the names on one line, separated by spaces */
		for (int i = 0; i < FSI_MAX_NODES_COUNT; i++) {
			if ((n + i)->used != FSI_NODE_USED) {
				continue;
			}

			if ((n + i)->hidden == true && hidden == false) {
				continue;
			}

			const char *name = (n + i)->name;
			int name_len = strnlen(name, FSI_NODE_NAME_MAX_LENGTH);

			/* room for the name, a space and the 0 */
			if (ls_size + name_len + 2 >= 128) {
				break;
			}

			memcpy(lstring + ls_size, name, name_len);
			ls_size += name_len;

			lstring[ls_size++] = ' ';
			lstring[ls_size] = '\0';
		}

		printf("%s\n", lstring);
	} else {
		/* a table: which handlers each node has */
		printf("fd\tname\thide\t\topen\tclose\tread\twrite\tlseek\texec\n");

		for (int i = 0; i < FSI_MAX_NODES_COUNT; i++) {
			if ((n + i)->used != FSI_NODE_USED) {
				continue;
			}

			if ((n + i)->hidden == true && hidden == false) {
				continue;
			}

			printf("%i\t%s\t%s\t\t%s\t%s\t%s\t%s\t%s\t%s\n", i, (n + i)->name,
			       (n + i)->hidden ? "true" : "false",
			       ((n + i)->handlers->open_handler == NULL) ? "false" : "true",
			       ((n + i)->handlers->close_handler == NULL) ? "false" : "true",
			       ((n + i)->handlers->read_handler == NULL) ? "false" : "true",
			       ((n + i)->handlers->write_handler == NULL) ? "false" : "true",
			       ((n + i)->handlers->lseek_handler == NULL) ? "false" : "true",
			       ((n + i)->handlers->execute_handler == NULL) ? "false" : "true");

			ls_size++;
		}
	}

	free(lstring);

	return ls_size;
}
