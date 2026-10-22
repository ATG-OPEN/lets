/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file sram23.h
 * @brief 23xx SPI SRAM (Microchip 23K256, 23LC512, 23LC1024, and compatibles) as a block device.
 *
 * The chip is put in sequential mode, so a read or write may cross pages and
 * never waits. Its contents are lost at power off: mount it with format
 * (`mount -f ram1 /tmp`) to get an empty file system at every boot.
 *
 * @code
 * static sram23_t ram1;
 *
 * sram23_init(&ram1, &ram_spi, 131072);           // 23LC1024: 128 KiB
 * vfs_blk_register("ram1", &ram1.blk);
 * @endcode
 */

#ifndef VFS_SRAM23_H
#define VFS_SRAM23_H

#include "../blk.h"
#include "spi.h"

/** A 23xx SRAM. */
typedef struct {
	vfs_blk_t blk;	      /**< Must be first. */
	const vfs_spi_t *spi; /**< The chip's bus. */
} sram23_t;

/**
 * @brief Prepares the driver and sets the chip to sequential mode.
 * @param size Capacity in bytes (3 address bytes above 64 KiB).
 * @return 0, or -1 for bad arguments.
 */
int sram23_init(sram23_t *ram, const vfs_spi_t *spi, uint32_t size);

#endif /* VFS_SRAM23_H */
