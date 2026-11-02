/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file sst25.h
 * @brief SST25VF SPI flash (SST25VF016B and compatibles: 4 KiB sectors, AAI word program) as a block device.
 *
 * Commands READ 0x03, sector erase 0x20, byte program 0x02, AAI word program
 * 0xAD, WREN 0x06, WRDI 0x04, RDSR 0x05, EWSR 0x50, WRSR 0x01, JEDEC ID 0x9F.
 * The chip starts write protected; sst25_init() clears the protection.
 *
 * A flash bit goes from 1 to 0 by programming and back to 1 only by erasing
 * its 4 KiB sector. A write:
 *
 * - leaves bytes that do not change alone and programs the others when that
 *   only clears bits (into an erased block, an append, a FAT entry taken);
 * - otherwise rewrites the sector: copies it, changed, to @p scratch (an SPI
 *   SRAM, say), erases it and programs it back. That takes about 50 ms, and a
 *   power loss in between loses the sector.
 *
 * The file system calls discard() on each block it allocates (blk.h); with
 * 4 KiB blocks (mkfs's default on a flash) each block of a file is erased
 * once, when it is taken, and then written without rewrites. Only the sector
 * with the directory and the FAT is rewritten, about once per file closed.
 *
 * @code
 * static sram23_t ram;
 * static part_t scratch;
 * static sst25_t sf0;
 *
 * sram23_init(&ram, &ram_spi, 32768);                  // 23K256
 * part_init(&scratch, &ram.blk, 0, SST25_SECTOR);
 * sst25_init(&sf0, &sf_spi, 2097152, &scratch.blk);    // SST25VF016B: 2 MiB
 * vfs_blk_register("sf0", &sf0.blk);
 * @endcode
 */

#ifndef VFS_SST25_H
#define VFS_SST25_H

#include "../blk.h"
#include "spi.h"

#define SST25_SECTOR 4096u /**< Erase sector in bytes. */

/** An SST25VF flash. */
typedef struct {
	vfs_blk_t blk;	      /**< Must be first. */
	const vfs_spi_t *spi; /**< The chip's bus. */
	vfs_blk_t *scratch;   /**< SST25_SECTOR bytes for sector rewrites. */
	uint32_t id;	      /**< JEDEC id: 0xBF2541 for an SST25VF016B. */
	uint16_t erases;      /**< Sector erases since start (wraps). */
} sst25_t;

/**
 * @brief Reads the JEDEC id and clears the write protection.
 * @param size    Capacity in bytes, a multiple of SST25_SECTOR.
 * @param scratch At least SST25_SECTOR bytes of another device.
 * @return 0, or -1 for bad arguments or when no chip answers.
 */
int sst25_init(sst25_t *sf, const vfs_spi_t *spi, uint32_t size, vfs_blk_t *scratch);

#endif /* VFS_SST25_H */
