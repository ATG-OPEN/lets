/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * RAM files: a node with a small buffer (TMP_FILE_SIZE bytes). A write appends
 * to it, a read goes on from where this handle was.
 */

#include "tmp.h"

#include "../vfs/vfs.h"

#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#define TMP_FILE_SIZE 126 /* bytes per file (the struct is 128) */

typedef struct {
	uint16_t size; /* bytes stored */
	uint8_t memory[TMP_FILE_SIZE];
} tmp_args_t;

static int tmp_write(void *args, void *buffer, unsigned int length)
{
	tmp_args_t *a = args;

	if (length > TMP_FILE_SIZE - a->size) {
		return -1;
	}

	memcpy(a->memory + a->size, buffer, length);
	a->size += length;

	return (int)length;
}

static int tmp_read(void *args, void *buffer, unsigned int length)
{
	tmp_args_t *a = args;
	unsigned int *position = vfs_position();
	unsigned int left = (*position < a->size) ? a->size - *position : 0u;

	length = (length > left) ? left : length;

	memcpy(buffer, a->memory + *position, length);
	*position += length;

	return (int)length;
}

static int tmp_lseek(void *args, off_t offset, int whence)
{
	return vfs_position_seek(((tmp_args_t *)args)->size, offset, whence);
}

const vfs_handler_t tmp_node = {
	.read_handler = tmp_read,
	.write_handler = tmp_write,
	.lseek_handler = tmp_lseek,
};

int tmp_file_create(const char *name)
{
	tmp_args_t *a = malloc(sizeof(tmp_args_t));

	if (a == NULL) {
		return -1;
	}

	a->size = 0;

	int node = vfs_node_new(name, false, a, &tmp_node);

	if (node < 0) {
		free(a);
	}

	return node;
}

void tmp_file_truncate(const vfs_node_t *node)
{
	if (node->handlers == &tmp_node && node->args != NULL) {
		((tmp_args_t *)node->args)->size = 0;
	}
}

int tmp_file_size(const vfs_node_t *node)
{
	if (node == NULL || node->handlers != &tmp_node || node->args == NULL) {
		return -1;
	}

	return ((const tmp_args_t *)node->args)->size;
}
