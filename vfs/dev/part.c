/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "part.h"

#include <stddef.h>

static int part_read(vfs_blk_t *blk, uint32_t addr, void *buffer, unsigned int length)
{
	part_t *p = (part_t *)blk;

	if (addr + length > blk->size) {
		return -1;
	}

	return p->parent->read(p->parent, p->offset + addr, buffer, length);
}

static int part_write(vfs_blk_t *blk, uint32_t addr, const void *buffer, unsigned int length)
{
	part_t *p = (part_t *)blk;

	if (addr + length > blk->size) {
		return -1;
	}

	return p->parent->write(p->parent, p->offset + addr, buffer, length);
}

int part_init(part_t *part, vfs_blk_t *parent, uint32_t offset, uint32_t size)
{
	if (part == NULL || parent == NULL || size == 0 || offset + size > parent->size) {
		return -1;
	}

	part->blk.read = part_read;
	part->blk.write = part_write;
	part->blk.size = size;
	part->blk.pos = 0;
	part->parent = parent;
	part->offset = offset;

	return 0;
}
