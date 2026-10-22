/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file ee25.h
 * @brief 25xx SPI EEPROM (Microchip 25LC / 25AA, ST M95, and compatibles) as a block device.
 *
 * Commands READ 0x03, WRITE 0x02, WREN 0x06, RDSR 0x05. Writes are split at
 * page boundaries; after each page the driver polls the status register until
 * the chip is done (typically 5 ms per page). That wait is the only place the
 * file system blocks: write files in chunks of a page or less from a task.
 *
 * 2 address bytes up to 64 KiB, 3 above (25LC1024, 25LC512 uses 2). Chips of
 * 512 bytes and less (9th address bit in the command) are not supported.
 *
 * @code
 * static ee25_t ee0;
 *
 * ee25_init(&ee0, &ee_spi, 131072, 256);          // 25LC1024: 128 KiB, 256-byte pages
 * vfs_blk_register("ee0", &ee0.blk);
 * @endcode
 */

#ifndef VFS_EE25_H
#define VFS_EE25_H

#include "../blk.h"
#include "spi.h"

/** A 25xx EEPROM. */
typedef struct {
	vfs_blk_t blk;	      /**< Must be first. */
	const vfs_spi_t *spi; /**< The chip's bus. */
	uint16_t page;	      /**< Page size in bytes (power of two). */
} ee25_t;

/**
 * @brief Prepares the driver (no SPI traffic).
 * @param size Capacity in bytes.
 * @param page Page size in bytes, from the datasheet (16 .. 512).
 * @return 0, or -1 for bad arguments.
 */
int ee25_init(ee25_t *ee, const vfs_spi_t *spi, uint32_t size, uint16_t page);

#endif /* VFS_EE25_H */
