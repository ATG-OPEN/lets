/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file ramdisk.h
 * @brief A buffer in internal RAM as a block device.
 *
 * For a /tmp with real files on a board without SPI RAM, and for tests.
 *
 * @code
 * static uint8_t ram0_mem[2048];
 * static ramdisk_t ram0;
 *
 * ramdisk_init(&ram0, ram0_mem, sizeof(ram0_mem));
 * vfs_blk_register("ram0", &ram0.blk);
 * @endcode
 */

#ifndef VFS_RAMDISK_H
#define VFS_RAMDISK_H

#include "../blk.h"

/** A RAM disk. */
typedef struct {
	vfs_blk_t blk;	/**< Must be first. */
	uint8_t *mem;	/**< The buffer, owned by the caller. */
} ramdisk_t;

/** @brief Prepares the RAM disk over @p mem. @return 0, or -1 for bad arguments. */
int ramdisk_init(ramdisk_t *disk, uint8_t *mem, uint32_t size);

#endif /* VFS_RAMDISK_H */
