/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "ramdisk.h"

#include <stddef.h>
#include <string.h>

static int ramdisk_read(vfs_blk_t *blk, uint32_t addr, void *buffer, unsigned int length)
{
	if (addr + length > blk->size) {
		return -1;
	}

	memcpy(buffer, ((ramdisk_t *)blk)->mem + addr, length);

	return 0;
}

static int ramdisk_write(vfs_blk_t *blk, uint32_t addr, const void *buffer, unsigned int length)
{
	if (addr + length > blk->size) {
		return -1;
	}

	memcpy(((ramdisk_t *)blk)->mem + addr, buffer, length);

	return 0;
}

int ramdisk_init(ramdisk_t *disk, uint8_t *mem, uint32_t size)
{
	if (disk == NULL || mem == NULL || size == 0) {
		return -1;
	}

	disk->blk.read = ramdisk_read;
	disk->blk.write = ramdisk_write;
	disk->blk.size = size;
	disk->blk.pos = 0;
	disk->mem = mem;

	return 0;
}
