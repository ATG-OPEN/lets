/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * The node table and the console. vfs.c puts the tree on the table.
 */

#include "vfs.h"

#include "../kernel/task.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static vfs_node_t vfs_nodes[VFS_NODES_MAX];

vfs_node_t *vfs_nodes_get(void)
{
	return vfs_nodes;
}

/* empties the table; vfs_initialize() calls it first */
void vfs_nodes_clear(void)
{
	memset(vfs_nodes, 0, sizeof(vfs_nodes));
}

int vfs_node_new(const char *name, bool hidden, void *args, const vfs_handler_t *handlers)
{
	if (name == NULL || handlers == NULL) {
		return -1;
	}

	for (int i = 0; i < VFS_NODES_MAX; i++) {
		vfs_node_t *n = &vfs_nodes[i];

		if (n->handlers == NULL) {
			n->hidden = hidden;
			n->args = args;
			n->handlers = handlers;

			strncpy(n->name, name, VFS_NODE_NAME_MAX - 1);
			n->name[VFS_NODE_NAME_MAX - 1] = '\0';

			return i;
		}
	}

	return -1;
}

/* -------------------------------------------------------------------------- */
/* the console: the standard streams of every task that has none of its own */

static bool vfs_std_terminal; /* std -t: a person on a terminal */
static int8_t vfs_std_fd = -1;

int vfs_console(void)
{
	return vfs_std_fd;
}

bool vfs_terminal(void)
{
	int8_t terminal = task_streams()->terminal;

	return (terminal >= 0) ? (terminal != 0) : vfs_std_terminal;
}

int vfs_std_execute(void *args, int argc, char *argv[])
{
	bool terminal = (argc == 3 && strcmp(argv[1], "-t") == 0);

	if (argc != 2 && !terminal) {
		return -1;
	}

	int fd = open(argv[argc - 1], O_RDWR, 0);

	if (fd < 0) {
		return -1;
	}

	if (vfs_std_fd >= 0) {
		close(vfs_std_fd);
	}

	vfs_std_fd = (int8_t)fd;
	vfs_std_terminal = terminal;

	return 0;
}
