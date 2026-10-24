/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * RAM files: `touch NAME` creates a node with a small buffer (TMP_FILE_SIZE
 * bytes); a write appends to it, a read returns it from the start, `rm NAME`
 * deletes it.
 */

#include "tmp.h"

#include "../fsi/fsi.h"

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TMP_FILE_SIZE 126 /* bytes per file (the struct is 128) */

typedef struct {
	uint16_t size; /* bytes stored */
	uint8_t memory[TMP_FILE_SIZE];
} tmp_args_t;

fsi_handler_t tmp_node = {
	.open_handler = NULL,
	.read_handler = tmp_read,
	.write_handler = tmp_write,
	.lseek_handler = NULL,
	.execute_handler = NULL,
	.close_handler = NULL,
};

/* the RAM file named @name, -1 if none */
static int tmp_find(const char *name)
{
	fsi_node_t *n = fsi_nodes_get();

	for (int i = 3; i < FSI_MAX_NODES_COUNT; i++) {
		if (n[i].used == FSI_NODE_USED && strcmp(n[i].name, name) == 0) {
			return i;
		}
	}

	return -1;
}

int tmp_file_create(void *args, int argc, char *argv[])
{
	if (argc != 2 || strcmp(argv[1], "-help") == 0) {
		printf("usage: touch NAME\n");
		return -1;
	}

	/* a name that is taken: nothing to do, as touch on an existing file */
	if (tmp_find(argv[1]) >= 0) {
		return 0;
	}

	tmp_args_t *a = malloc(sizeof(tmp_args_t));

	if (a == NULL) {
		printf("out of memory\n");
		return -1;
	}

	a->size = 0;

	if (fsi_node_create(argv[1], false, a, &tmp_node) < 0) {
		free(a);
		printf("out of memory\n"); /* the node table is full */
		return -1;
	}

	return 0;
}

int tmp_file_delete(void *args, int argc, char *argv[])
{
	if (argc != 2 || strcmp(argv[1], "-help") == 0) {
		printf("usage: rm NAME\n");
		return -1;
	}

	int i = tmp_find(argv[1]);

	if (i < 0) {
		printf("not found\n");
		return -1;
	}

	/* only RAM files: a device's args belong to its driver */
	fsi_node_t *n = fsi_nodes_get() + i;

	if (n->handlers != &tmp_node) {
		printf("bad argument\n");
		return -1;
	}

	free(n->args);

	return fsi_node_delete(argv[1]);
}

int tmp_write(void *args, void *buffer, unsigned int length)
{
	if (args == NULL) {
		return -1;
	}

	tmp_args_t *a = args;

	if (length <= TMP_FILE_SIZE - a->size) {
		memcpy(a->memory + a->size, buffer, length);
		a->size += length;

		return length;
	}
	return -1;
}

int tmp_read(void *args, void *buffer, unsigned int length)
{
	if (args == NULL) {
		return -1;
	}

	tmp_args_t *a = args;

	length = (length > a->size) ? a->size : length;

	memcpy(buffer, a->memory, length);

	return length;
}

int tmp_file_size(const fsi_node_t *node)
{
	if (node == NULL || node->handlers != &tmp_node || node->args == NULL) {
		return -1;
	}

	return ((const tmp_args_t *)node->args)->size;
}
