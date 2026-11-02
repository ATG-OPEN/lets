/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file blk.h
 * @brief Block device: byte-addressable storage that a file system can sit on.
 *
 * An SPI EEPROM, an SPI SRAM or a buffer in internal RAM all look the same to
 * the file system: read and write any number of bytes at any address. A driver
 * embeds vfs_blk_t as its first member and fills in the functions; see
 * dev/ee25.h, dev/sram23.h, dev/ramdisk.h and dev/sst25.h.
 *
 * A flash chip has to erase before it writes. Its driver also fills in
 * discard(): the file system calls it on a block it is about to fill, so the
 * driver can erase that block once instead of erasing at every small write.
 *
 * vfs_blk_register() makes a device a node in /dev (`/dev/ee0`), where it can
 * be read raw (`cat -x /dev/ee0`), formatted (`mkfs ee0`) and mounted
 * (`mount ee0 /mnt/ee`).
 */

#ifndef VFS_BLK_H
#define VFS_BLK_H

#include <stdint.h>

typedef struct vfs_blk vfs_blk_t;

/** A block device. Drivers put it first in their own struct. */
struct vfs_blk {
	/** Reads @p length bytes at @p addr; returns 0 or -1. */
	int (*read)(vfs_blk_t *blk, uint32_t addr, void *buffer, unsigned int length);
	/** Writes @p length bytes at @p addr (any alignment); returns 0 or -1. */
	int (*write)(vfs_blk_t *blk, uint32_t addr, const void *buffer, unsigned int length);
	/** Optional (NULL): @p length bytes at @p addr are no longer needed; returns 0 or -1. */
	int (*discard)(vfs_blk_t *blk, uint32_t addr, uint32_t length);
	uint32_t size; /**< Capacity in bytes. */
	uint32_t pos;  /**< Position of raw reads / writes through its /dev node. */
};

#endif /* VFS_BLK_H */
