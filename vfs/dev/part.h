/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file part.h
 * @brief A partition: a part of a block device as a block device of its own.
 *
 * Two file systems on one memory, /boot and /home on one flash or EEPROM:
 * @code
 * static part_t boot, home;
 *
 * part_init(&boot, &nv.blk, 0, 512);                    // bytes 0 .. 511
 * part_init(&home, &nv.blk, 512, nv.blk.size - 512);    // the rest
 * vfs_blk_register("nv0", &boot.blk);
 * vfs_blk_register("nv1", &home.blk);
 * @endcode
 */

#ifndef VFS_PART_H
#define VFS_PART_H

#include "../blk.h"

/** A partition. */
typedef struct {
	vfs_blk_t blk;	   /**< Must be first. */
	vfs_blk_t *parent; /**< The device it is part of. */
	uint32_t offset;   /**< Where it starts on the parent. */
} part_t;

/** @brief Makes @p size bytes of @p parent at @p offset a device. @return 0, or -1. */
int part_init(part_t *part, vfs_blk_t *parent, uint32_t offset, uint32_t size);

#endif /* VFS_PART_H */
