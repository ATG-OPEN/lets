/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "flash24.h"

#include <stddef.h>
#include <stdbool.h>

#define FLASH24_DATA   2048u		    /* data bytes in a page: 16 bits per instruction */
#define FLASH24_HEADER (FLASH24_DATA - 4u) /* the last double word: magic, generation */
#define FLASH24_MAGIC  0xA55Au
#define FLASH24_ERASED 0xFFFFFFul

/* -------------------------------------------------------------------------- */
/* the flash itself */

#if defined(__XC16__)
#include <xc.h>

#define FLASH24_LATCH	  0xFAu	  /* TBLPAG of the write latches */
#define FLASH24_ERASE_OP  0x4003u /* WREN, page erase */
#define FLASH24_DOUBLE_OP 0x4001u /* WREN, double-word program */

static uint32_t flash24_hw_read(uint32_t addr)
{
	uint16_t page = TBLPAG;

	TBLPAG = (uint16_t)(addr >> 16);

	uint16_t lo = __builtin_tblrdl((uint16_t)addr);
	uint16_t hi = __builtin_tblrdh((uint16_t)addr);

	TBLPAG = page;

	return ((uint32_t)(hi & 0xFFu) << 16) | lo;
}

static int flash24_hw_nvm(uint32_t addr, uint16_t op)
{
	NVMADRU = (uint16_t)(addr >> 16);
	NVMADR = (uint16_t)addr;
	NVMCON = op;

	__builtin_write_NVM(); /* the unlock sequence, then WR; the CPU stalls until done */

	while (NVMCONbits.WR) {
	}

	NVMCONbits.WREN = 0;

	return NVMCONbits.WRERR ? -1 : 0;
}

static int flash24_hw_erase(uint32_t addr)
{
	return flash24_hw_nvm(addr, FLASH24_ERASE_OP);
}

static int flash24_hw_program(uint32_t addr, uint32_t w0, uint32_t w1)
{
	uint16_t page = TBLPAG;

	TBLPAG = FLASH24_LATCH;
	__builtin_tblwtl(0, (uint16_t)w0);
	__builtin_tblwth(0, (uint16_t)(w0 >> 16));
	__builtin_tblwtl(2, (uint16_t)w1);
	__builtin_tblwth(2, (uint16_t)(w1 >> 16));
	TBLPAG = page;

	return flash24_hw_nvm(addr, FLASH24_DOUBLE_OP);
}
#endif

/* -------------------------------------------------------------------------- */

static uint32_t flash24_page(const flash24_t *f, uint8_t page)
{
	return f->base + (page ? FLASH24_PAGE : 0u);
}

/* the 16 data bits at data address @a (even) of @page */
static uint16_t flash24_word(const flash24_t *f, uint8_t page, uint32_t a)
{
	return (uint16_t)flash24_hw_read(flash24_page(f, page) + a);
}

/* @old, the word at data address @a, after @p[0 .. length) is written at @addr */
static uint16_t flash24_merge(uint32_t a, uint16_t old, uint32_t addr, const uint8_t *p,
			      unsigned int length)
{
	if (a >= addr && a < addr + length) {
		old = (uint16_t)((old & 0xFF00u) | p[a - addr]);
	}

	if (a + 1u >= addr && a + 1u < addr + length) {
		old = (uint16_t)((old & 0x00FFu) | ((uint16_t)p[a + 1u - addr] << 8));
	}

	return old;
}

static bool flash24_header(const flash24_t *f, uint8_t page, uint16_t *gen)
{
	uint32_t at = flash24_page(f, page) + FLASH24_HEADER;

	if (flash24_hw_read(at) != FLASH24_MAGIC) {
		return false;
	}

	*gen = (uint16_t)flash24_hw_read(at + 2u);

	return true;
}

static int flash24_read(vfs_blk_t *blk, uint32_t addr, void *buffer, unsigned int length)
{
	flash24_t *f = (flash24_t *)blk;
	uint8_t *p = buffer;

	if (addr + length > blk->size) {
		return -1;
	}

	for (unsigned int i = 0; i < length; i++) {
		uint32_t a = addr + i;
		uint16_t w = flash24_word(f, f->live, a & ~1ul);

		p[i] = (uint8_t)((a & 1u) ? (w >> 8) : w);
	}

	return 0;
}

/* the live page, changed, into the other one; its header last */
static int flash24_copy(flash24_t *f, uint32_t addr, const uint8_t *p, unsigned int length)
{
	uint8_t other = (uint8_t)(f->live ^ 1u);
	uint32_t to = flash24_page(f, other);

	if (flash24_hw_erase(to) < 0) {
		return -1;
	}

	for (uint32_t d = 0; d < FLASH24_HEADER; d += 4u) {
		uint16_t w0 = flash24_merge(d, flash24_word(f, f->live, d), addr, p, length);
		uint16_t w1 = flash24_merge(d + 2u, flash24_word(f, f->live, d + 2u), addr, p, length);

		if ((w0 & w1) == 0xFFFFu) {
			continue; /* stays erased */
		}

		if (flash24_hw_program(to + d, w0, w1) < 0) {
			return -1;
		}
	}

	if (flash24_hw_program(to + FLASH24_HEADER, FLASH24_MAGIC, (uint16_t)(f->gen + 1u)) < 0) {
		return -1;
	}

	f->live = other;
	f->gen++;

	return 0;
}

static int flash24_write(vfs_blk_t *blk, uint32_t addr, const void *buffer, unsigned int length)
{
	flash24_t *f = (flash24_t *)blk;
	const uint8_t *p = buffer;
	uint32_t at = flash24_page(f, f->live);
	uint32_t end = addr + length;

	if (end > blk->size) {
		return -1;
	}

	/* two passes: are all changed double words erased? then program them in place */
	for (int program = 0; program < 2; program++) {
		for (uint32_t d = addr & ~3ul; d < end; d += 4u) {
			uint32_t i0 = flash24_hw_read(at + d);
			uint32_t i1 = flash24_hw_read(at + d + 2u);
			uint16_t w0 = flash24_merge(d, (uint16_t)i0, addr, p, length);
			uint16_t w1 = flash24_merge(d + 2u, (uint16_t)i1, addr, p, length);

			if (w0 == (uint16_t)i0 && w1 == (uint16_t)i1) {
				continue; /* unchanged */
			}

			if (i0 != FLASH24_ERASED || i1 != FLASH24_ERASED) {
				return flash24_copy(f, addr, p, length);
			}

			if (program && flash24_hw_program(at + d, w0, w1) < 0) {
				return -1;
			}
		}
	}

	return 0;
}

int flash24_init(flash24_t *f, uint32_t address)
{
	uint16_t g0, g1;

	if (f == NULL || (address & (FLASH24_PAGE - 1u)) != 0) {
		return -1;
	}

	f->blk.read = flash24_read;
	f->blk.write = flash24_write;
	f->blk.size = FLASH24_HEADER;
	f->blk.pos = 0;
	f->base = address;
	f->live = 0;
	f->gen = 0;

	bool v0 = flash24_header(f, 0, &g0);
	bool v1 = flash24_header(f, 1, &g1);

	/* the newer valid page; none: page 0, never written */
	if (v1 && (!v0 || (int16_t)(g1 - g0) > 0)) {
		f->live = 1;
		f->gen = g1;
	} else if (v0) {
		f->gen = g0;
	}

	return 0;
}
