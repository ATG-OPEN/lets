/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * The node table. Entries 0..2 are stdin, stdout and stderr; nodes created
 * by drivers and applications start at entry 3.
 */

#include "fsi.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <ctype.h>

/* -------------------------------------------------------------------------- */

static fsi_node_t FSI_NODES[FSI_MAX_NODES_COUNT];

fsi_node_t *fsi_nodes_get(void)
{
	return FSI_NODES;
}

/* -------------------------------------------------------------------------- */

int fsi_initialize(void)
{
	for (int i = 0; i < FSI_MAX_NODES_COUNT; i++) {
		FSI_NODES[i].used = FSI_NODE_UNUSED;
		FSI_NODES[i].hidden = true;
		FSI_NODES[i].args = NULL;
		FSI_NODES[i].handlers = NULL;
	}

	fsi_register_app("std", fsi_node_std_execute);

	return 0;
}

int fsi_register_app(char *name, int (*execute_handler)(void *args, int argc, char *argv[]))
{
	if (name == NULL || execute_handler == NULL) {
		return -1;
	}

	fsi_handler_t *node = malloc(sizeof(fsi_handler_t));

	if (node == NULL) {
		return -1;
	}

	node->open_handler = NULL;
	node->read_handler = NULL;
	node->write_handler = NULL;
	node->lseek_handler = NULL;
	node->execute_handler = execute_handler;
	node->close_handler = NULL;

	if (fsi_node_create(name, true, NULL, node) < 0) {
		free(node);
		return -1;
	}

	return 0;
}

int fsi_node_std_execute(void *args, int argc, char *argv[])
{
	if (argc != 2) {
		return -1;
	}

	int fd = open(argv[1], 0, 0);

	if (fd < 0) {
		return -1;
	}

	fsi_node_standard(fd);

	return close(fd);
}

/* handlers of the standard streams: copies of the target node's read / write */
static fsi_handler_t fsi_stdin_handler;
static fsi_handler_t fsi_stdout_handler;

static void fsi_node_set(int i, const char *name, void *args, fsi_handler_t *handlers)
{
	FSI_NODES[i].used = FSI_NODE_USED;
	FSI_NODES[i].hidden = true;
	FSI_NODES[i].args = args;
	FSI_NODES[i].handlers = handlers;

	strncpy(FSI_NODES[i].name, name, FSI_NODE_NAME_MAX_LENGTH - 1);
	FSI_NODES[i].name[FSI_NODE_NAME_MAX_LENGTH - 1] = '\0';
}

int fsi_node_standard(int fd)
{
	if (fd < 3 || fd >= FSI_MAX_NODES_COUNT) {
		return -1;
	}

	if (FSI_NODES[fd].used != FSI_NODE_USED || FSI_NODES[fd].handlers == NULL) {
		return -1;
	}

	memset(&fsi_stdin_handler, 0, sizeof(fsi_handler_t));
	memset(&fsi_stdout_handler, 0, sizeof(fsi_handler_t));

	fsi_stdin_handler.read_handler = FSI_NODES[fd].handlers->read_handler;
	fsi_stdout_handler.write_handler = FSI_NODES[fd].handlers->write_handler;

	fsi_node_set(0, "stdin", FSI_NODES[fd].args, &fsi_stdin_handler);
	fsi_node_set(1, "stdout", FSI_NODES[fd].args, &fsi_stdout_handler);
	fsi_node_set(2, "stderr", FSI_NODES[fd].args, &fsi_stdout_handler);

	return 0;
}

int fsi_node_create(const char *name, bool hidden, void *args, fsi_handler_t *handlers)
{
	if (name == NULL || handlers == NULL) {
		return -1;
	}

	/* the first free entry after the standard streams */
	for (int i = 3; i < FSI_MAX_NODES_COUNT; i++) {
		if (FSI_NODES[i].used != FSI_NODE_UNUSED) {
			continue;
		}

		FSI_NODES[i].used = FSI_NODE_USED;
		FSI_NODES[i].hidden = hidden;

		strncpy(FSI_NODES[i].name, name, FSI_NODE_NAME_MAX_LENGTH - 1);
		FSI_NODES[i].name[FSI_NODE_NAME_MAX_LENGTH - 1] = '\0';

		FSI_NODES[i].args = args;

		FSI_NODES[i].handlers = handlers;

		return 0;
	}
	return -1;
}

int fsi_node_delete(const char *name)
{
	if (name == NULL) {
		return -1;
	}

	for (int i = 3; i < FSI_MAX_NODES_COUNT; i++) {
		if (FSI_NODES[i].used != FSI_NODE_USED) {
			continue;
		}

		if (strcmp(FSI_NODES[i].name, name) != 0) {
			continue;
		}

		FSI_NODES[i].used = FSI_NODE_UNUSED;
		FSI_NODES[i].args = NULL;

		return 0;
	}

	return -1;
}
