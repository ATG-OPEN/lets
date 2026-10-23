/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * RAM files: `touch name` creates a node with a small buffer; writes append
 * to it and reads return it from the start.
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

int tmp_file_create(void *args, int argc, char *argv[])
{
	if (argc != 2) {
		return -1;
	}

	tmp_args_t *a = malloc(sizeof(tmp_args_t));

	if (a == NULL) {
		return -1;
	}

	a->size = 0;

	return fsi_node_create(argv[1], false, a, &tmp_node);
}

int tmp_file_delete(void *args, int argc, char *argv[])
{
	if (argc != 2) {
		return -1;
	}

	args = NULL;

	/* the node's buffer, freed before the node goes */
	fsi_node_t *n = fsi_nodes_get();

	for (int i = 1; i < FSI_MAX_NODES_COUNT; i++) {
		if ((n + i)->used != FSI_NODE_USED) {
			continue;
		}

		if (strcmp((n + i)->name, argv[1]) != 0) {
			continue;
		}

		args = (n + i)->args;

		break;
	}

	if (args != NULL) {
		free(args);
		args = NULL;
	}

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
