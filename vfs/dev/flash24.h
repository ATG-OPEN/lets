/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file flash24.h
 * @brief Two pages of a PIC24FJ's own program flash as a block device.
 *
 * Non-volatile storage on a board without an EEPROM: /boot and /home in the
 * MCU's flash (PIC24FJ GA7 / GB4 / GL / GU families: 1024-instruction erase
 * pages, double-word programming, ECC).
 *
 * Each instruction keeps 16 bits of data (its low word), so a page holds 2048
 * bytes; the last 4 are a header, so the device is 2044 bytes. Two pages take
 * turns:
 *
 * - a write that only fills erased double words is programmed in place;
 * - any other write copies the live page, changed, to the other page and
 *   programs that page's header (magic and generation + 1) last. A power
 *   loss before that leaves the old page live, so every write is all or
 *   nothing, and each page is erased once per such copy.
 *
 * A double word is never programmed twice between erases (ECC). A copy takes
 * about one page erase plus a few hundred double-word programs, during which
 * the CPU stalls (some tens of ms): fine for configs and scripts, not for
 * logging. Endurance is about 10 000 erases per page.
 *
 * The pages must be kept free by the linker and by the programmer:
 * @code
 * #define NV_ADDRESS 0x14800ul      // two erase pages below the configuration page
 *
 * static const uint16_t nv_space[2048] __attribute__((space(prog), address(NV_ADDRESS), noload));
 * static flash24_t nv;
 *
 * flash24_init(&nv, NV_ADDRESS);
 * @endcode
 * and `ipecmd ... -OP14800-157FF` keeps them when the program is updated.
 *
 * On a PC (tests) the three flash operations are flash24_hw_read(),
 * flash24_hw_erase() and flash24_hw_program(), provided by the test.
 */

#ifndef VFS_FLASH24_H
#define VFS_FLASH24_H

#include "../blk.h"

#define FLASH24_PAGE 0x800ul /**< An erase page, in program addresses (1024 instructions). */

/** Two flash pages. */
typedef struct {
	vfs_blk_t blk;	/**< Must be first. */
	uint32_t base;	/**< Program address of the first page. */
	uint16_t gen;	/**< Generation of the live page. */
	uint8_t live;	/**< The live page: 0 or 1. */
} flash24_t;

/**
 * @brief Finds the live page of the two at @p address (page aligned).
 * @return 0, or -1 for a bad address.
 */
int flash24_init(flash24_t *f, uint32_t address);

#if !defined(__XC16__)
/** Host build: the instruction (24 bits) at program address @p addr. */
uint32_t flash24_hw_read(uint32_t addr);
/** Host build: erases the page at @p addr; 0 or -1. */
int flash24_hw_erase(uint32_t addr);
/** Host build: programs the double word at @p addr; 0 or -1. */
int flash24_hw_program(uint32_t addr, uint32_t w0, uint32_t w1);
#endif

#endif /* VFS_FLASH24_H */
