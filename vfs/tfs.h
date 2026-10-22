/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file tfs.h
 * @brief Tiny file system: flat, FAT-linked, for small byte-addressable memories.
 *
 * Made for SPI EEPROM and SRAM of a few KiB to 16 MiB on MCUs with a few KiB
 * of RAM. Nothing is cached: a mounted file system is 18 bytes of RAM, an open
 * file 22 (on a PIC24). tfs.c depends only on blk.h and the C library, so a bootloader can
 * link it to find and check a firmware image.
 *
 * Layout on the device (little endian):
 *
 *   0                 superblock, 16 bytes: "TFS1", block size, blocks,
 *                     directory entries, data address, check
 *   16                directory: entries x 32 bytes
 *   16 + entries*32   FAT: one u16 per block (0xFFFF free, 0xFFFE last, else next)
 *   data              blocks, data aligned to the block size
 *
 * A directory entry: name[16] (15 characters and a 0), state (1 writing,
 * 2 valid, anything else free), generation, first block (u16), size (u32),
 * CRC-32 (u32, the zlib / IEEE one), attributes, 3 spare bytes.
 *
 * Power loss:
 * - a file opened with TFS_TRUNC is written to a new entry; the old one stays
 *   readable until tfs_close() commits the new one. At mount, an entry still
 *   being written is dropped, and of two valid entries with the same name the
 *   newer generation wins. Configs and firmware images are replaced whole or
 *   not at all.
 * - other writes go in place, and the new size is committed at close.
 * - blocks no file reaches any more are freed at mount.
 *
 * Build options: TFS_NO_CHECK leaves out the repair at mount (and its
 * temporary malloc); TFS_READONLY also leaves out format and the repair, for
 * a bootloader that only reads (tfs_write() etc. are still there but unused
 * functions are dropped by the linker with -ffunction-sections).
 *
 * Blocks are allocated round-robin from the last allocation, which spreads the
 * writes over an EEPROM. The CRC of a file written from start to end in order
 * is kept as it is written (TFS_ATTR_CRC); any other write clears it.
 */

#ifndef VFS_TFS_H
#define VFS_TFS_H

#include "blk.h"

#include <stdint.h>
#include <stdbool.h>

#define TFS_NAME_SIZE	   16u	   /**< File name, including the 0. */
#define TFS_BLOCK_DEFAULT  128u	   /**< Block size used by `mkfs` and `mount -f`. */
#define TFS_ENTRIES_DEFAULT 32u	   /**< Directory entries used by `mkfs` and `mount -f`. */
#define TFS_NONE	   0xFFFFu /**< No block. */

/** tfs_open() modes, OR-ed. */
enum TFS_MODE {
	TFS_WRITE = 0x01,  /**< Writes allowed. */
	TFS_CREATE = 0x02, /**< Create the file if it does not exist. */
	TFS_TRUNC = 0x04,  /**< Start empty; replaces the old file at close. */
	TFS_APPEND = 0x08, /**< Every write goes to the end. */
	TFS_EXCL = 0x10,   /**< With TFS_CREATE: fail if the file exists. */
};

/** Attribute bits of a file. */
enum TFS_ATTR {
	TFS_ATTR_CRC = 0x01, /**< The stored CRC is the CRC of the whole file. */
};

/** A mounted file system. */
typedef struct {
	vfs_blk_t *blk;	  /**< The device. */
	uint32_t fat;	  /**< Address of the FAT. */
	uint32_t data;	  /**< Address of block 0. */
	uint16_t bsize;	  /**< Block size in bytes. */
	uint16_t blocks;  /**< Number of blocks. */
	uint16_t rover;	  /**< Where the next allocation starts looking. */
	uint8_t entries;  /**< Directory entries. */
	uint8_t bshift;	  /**< log2 of the block size. */
} tfs_t;

/** An open file. */
typedef struct {
	tfs_t *fs;     /**< Its file system. */
	uint32_t pos;  /**< Read / write position. */
	uint32_t size; /**< Size in bytes. */
	uint32_t crc;  /**< CRC-32 of the bytes so far, while TFS_ATTR_CRC holds. */
	uint16_t first; /**< First block, TFS_NONE when empty. */
	uint16_t block; /**< A block of the chain, to walk on from... */
	uint16_t bidx;	/**< ...and its index in the chain. */
	uint8_t entry;	/**< Directory entry. */
	uint8_t mode;	/**< enum TFS_MODE plus internal flags. */
} tfs_file_t;

/** What a directory entry says about a file. */
typedef struct {
	char name[TFS_NAME_SIZE]; /**< Name, 0 terminated. */
	uint32_t size;		  /**< Size in bytes. */
	uint32_t crc;		  /**< CRC-32, valid when attr has TFS_ATTR_CRC. */
	uint16_t first;		  /**< First block. */
	uint8_t state;		  /**< 1 being written, 2 valid, 0 free. */
	uint8_t gen;		  /**< Generation, +1 on each replacement. */
	uint8_t attr;		  /**< enum TFS_ATTR. */
} tfs_info_t;

/**
 * @brief Writes an empty file system over the whole device.
 * @param bsize   Block size: a power of two, 16 .. 4096. A multiple of an
 *                EEPROM's page size makes writes faster.
 * @param entries Most files, 1 .. 255.
 * @return 0, or -1 when the device is too small or an argument is bad.
 */
int tfs_format(vfs_blk_t *blk, uint16_t bsize, uint8_t entries);

/**
 * @brief Mounts the file system on @p blk and repairs what a power loss left.
 * @return 0, or -1 when there is no valid file system.
 */
int tfs_mount(tfs_t *fs, vfs_blk_t *blk);

/** @brief Opens @p name. @param mode enum TFS_MODE. @return 0, or -1. */
int tfs_open(tfs_t *fs, tfs_file_t *f, const char *name, uint8_t mode);

/** @brief Reads up to @p length bytes. @return Bytes read, 0 at the end, -1 on error. */
int tfs_read(tfs_file_t *f, void *buffer, unsigned int length);

/**
 * @brief Writes @p length bytes at the position, allocating blocks as needed.
 * @return Bytes written (less when the device is full), or -1.
 */
int tfs_write(tfs_file_t *f, const void *buffer, unsigned int length);

/**
 * @brief Moves the position, as lseek(); 0 .. size.
 * @param whence 0 from the start, 1 from the position, 2 from the end.
 * @return The new position, or -1.
 */
int32_t tfs_seek(tfs_file_t *f, int32_t offset, int whence);

/** @brief Commits size and CRC of a written file. @return 0, or -1. */
int tfs_close(tfs_file_t *f);

/** @brief Deletes a file. @return 0, or -1 when it does not exist. */
int tfs_remove(tfs_t *fs, const char *name);

/** @brief Renames a file, replacing @p to if it exists. @return 0, or -1. */
int tfs_rename(tfs_t *fs, const char *from, const char *to);

/** @brief Reads the entry of @p name. @return 0, or -1 when it does not exist. */
int tfs_stat(tfs_t *fs, const char *name, tfs_info_t *info);

/**
 * @brief Lists the files: call with *index = 0, then again while it returns 0.
 * @return 0 with the next file in @p info, -1 after the last.
 */
int tfs_dir(tfs_t *fs, uint8_t *index, tfs_info_t *info);

/** @brief Returns the free bytes (free blocks times the block size). */
uint32_t tfs_free(tfs_t *fs);

/**
 * @brief CRC-32 (zlib / IEEE 802.3), continued: start with 0.
 * @code
 * uint32_t crc = tfs_crc32(0, "123456789", 9);    // 0xCBF43926
 * @endcode
 */
uint32_t tfs_crc32(uint32_t crc, const void *buffer, unsigned int length);

#endif /* VFS_TFS_H */
