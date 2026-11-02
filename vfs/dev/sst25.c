/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "sst25.h"

#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#define SST25_WRSR    0x01u
#define SST25_PROGRAM 0x02u /* one byte */
#define SST25_READ    0x03u
#define SST25_WRDI    0x04u
#define SST25_RDSR    0x05u
#define SST25_WREN    0x06u
#define SST25_ERASE   0x20u /* a 4 KiB sector */
#define SST25_EWSR    0x50u
#define SST25_JEDEC   0x9Fu
#define SST25_AAI     0xADu /* auto address increment, a word at a time */
#define SST25_BUSY    0x01u /* status: programming or erasing */

#define SST25_POLLS 50000u /* status reads before an erase counts as failed (> 100 ms) */
#define SST25_PIECE 32u	   /* bytes on the stack at a time */

static void sst25_simple(sst25_t *sf, uint8_t command)
{
	sf->spi->select(true);
	sf->spi->exchange(command);
	sf->spi->select(false);
}

/* selects the chip and sends a command with its address */
static void sst25_command(sst25_t *sf, uint8_t command, uint32_t addr)
{
	const vfs_spi_t *spi = sf->spi;

	spi->select(true);
	spi->exchange(command);
	spi->exchange((uint8_t)(addr >> 16));
	spi->exchange((uint8_t)(addr >> 8));
	spi->exchange((uint8_t)addr);
}

/* waits until the chip has finished programming or erasing */
static int sst25_wait(sst25_t *sf)
{
	const vfs_spi_t *spi = sf->spi;

	for (unsigned int i = 0; i < SST25_POLLS; i++) {
		spi->select(true);
		spi->exchange(SST25_RDSR);
		uint8_t status = spi->exchange(0xFF);
		spi->select(false);

		if (!(status & SST25_BUSY)) {
			return 0;
		}
	}

	return -1;
}

static int sst25_read(vfs_blk_t *blk, uint32_t addr, void *buffer, unsigned int length)
{
	sst25_t *sf = (sst25_t *)blk;
	uint8_t *p = buffer;

	if (addr + length > blk->size) {
		return -1;
	}

	sst25_command(sf, SST25_READ, addr);

	for (unsigned int i = 0; i < length; i++) {
		p[i] = sf->spi->exchange(0xFF);
	}

	sf->spi->select(false);

	return 0;
}

static int sst25_erase(sst25_t *sf, uint32_t addr)
{
	sst25_simple(sf, SST25_WREN);
	sst25_command(sf, SST25_ERASE, addr);
	sf->spi->select(false);
	sf->erases++;

	return sst25_wait(sf);
}

static int sst25_byte(sst25_t *sf, uint32_t addr, uint8_t byte)
{
	sst25_simple(sf, SST25_WREN);
	sst25_command(sf, SST25_PROGRAM, addr);
	sf->spi->exchange(byte);
	sf->spi->select(false);

	return sst25_wait(sf);
}

/* programs @n bytes at @addr: an odd byte at either end alone, the words between in AAI */
static int sst25_program(sst25_t *sf, uint32_t addr, const uint8_t *p, unsigned int n)
{
	const vfs_spi_t *spi = sf->spi;

	if (n > 0 && (addr & 1u)) {
		if (sst25_byte(sf, addr, *p) < 0) {
			return -1;
		}

		addr++;
		p++;
		n--;
	}

	if (n >= 2) {
		int r = 0;

		sst25_simple(sf, SST25_WREN);
		sst25_command(sf, SST25_AAI, addr);

		for (;;) {
			spi->exchange(p[0]);
			spi->exchange(p[1]);
			spi->select(false);

			addr += 2;
			p += 2;
			n -= 2;

			if (sst25_wait(sf) < 0) {
				r = -1;
				break;
			}

			if (n < 2) {
				break;
			}

			spi->select(true);
			spi->exchange(SST25_AAI);
		}

		sst25_simple(sf, SST25_WRDI); /* ends AAI */

		if (r < 0) {
			return -1;
		}
	}

	if (n > 0 && sst25_byte(sf, addr, *p) < 0) {
		return -1;
	}

	return 0;
}

/* programs the bytes of @p that differ from @old (the change only clears bits) */
static int sst25_put(sst25_t *sf, uint32_t addr, const uint8_t *old, const uint8_t *p, unsigned int n)
{
	unsigned int i = 0;

	while (i < n) {
		if (old[i] == p[i]) {
			i++;
			continue;
		}

		unsigned int start = i;

		while (i < n && old[i] != p[i]) {
			i++;
		}

		if (sst25_program(sf, addr + start, p + start, i - start) < 0) {
			return -1;
		}
	}

	return 0;
}

/* @n bytes at @addr, in one sector, that need an erase: the sector goes through the scratch */
static int sst25_rewrite(sst25_t *sf, uint32_t addr, const uint8_t *p, unsigned int n)
{
	vfs_blk_t *s = sf->scratch;
	uint32_t base = addr & ~(uint32_t)(SST25_SECTOR - 1u);
	uint32_t from = addr - base; /* the new bytes are [from, from + n) of the sector */
	uint8_t b[SST25_PIECE];
	uint8_t erased[SST25_PIECE];

	for (uint32_t o = 0; o < SST25_SECTOR; o += SST25_PIECE) {
		if (sst25_read(&sf->blk, base + o, b, SST25_PIECE) < 0) {
			return -1;
		}

		for (unsigned int j = 0; j < SST25_PIECE; j++) {
			if (o + j >= from && o + j < from + n) {
				b[j] = p[o + j - from];
			}
		}

		if (s->write(s, o, b, SST25_PIECE) < 0) {
			return -1;
		}
	}

	if (sst25_erase(sf, base) < 0) {
		return -1;
	}

	memset(erased, 0xFF, sizeof(erased));

	for (uint32_t o = 0; o < SST25_SECTOR; o += SST25_PIECE) {
		if (s->read(s, o, b, SST25_PIECE) < 0 || sst25_put(sf, base + o, erased, b, SST25_PIECE) < 0) {
			return -1;
		}
	}

	return 0;
}

/* @n bytes at @addr, in one sector: programmed in place when they only clear bits */
static int sst25_update(sst25_t *sf, uint32_t addr, const uint8_t *p, unsigned int n)
{
	uint8_t old[SST25_PIECE];

	for (unsigned int i = 0; i < n; i += SST25_PIECE) {
		unsigned int k = (n - i < SST25_PIECE) ? n - i : SST25_PIECE;

		if (sst25_read(&sf->blk, addr + i, old, k) < 0) {
			return -1;
		}

		for (unsigned int j = 0; j < k; j++) {
			if ((old[j] & p[i + j]) != p[i + j]) {
				return sst25_rewrite(sf, addr, p, n);
			}
		}
	}

	for (unsigned int i = 0; i < n; i += SST25_PIECE) {
		unsigned int k = (n - i < SST25_PIECE) ? n - i : SST25_PIECE;

		if (sst25_read(&sf->blk, addr + i, old, k) < 0 || sst25_put(sf, addr + i, old, p + i, k) < 0) {
			return -1;
		}
	}

	return 0;
}

static int sst25_write(vfs_blk_t *blk, uint32_t addr, const void *buffer, unsigned int length)
{
	sst25_t *sf = (sst25_t *)blk;
	const uint8_t *p = buffer;

	if (addr + length > blk->size) {
		return -1;
	}

	while (length > 0) {
		/* up to the end of the sector */
		unsigned int n = SST25_SECTOR - (unsigned int)(addr & (SST25_SECTOR - 1u));

		if (n > length) {
			n = length;
		}

		if (sst25_update(sf, addr, p, n) < 0) {
			return -1;
		}

		addr += n;
		p += n;
		length -= n;
	}

	return 0;
}

/* is the sector at @addr erased? */
static bool sst25_erased(sst25_t *sf, uint32_t addr)
{
	bool erased = true;

	sst25_command(sf, SST25_READ, addr);

	for (unsigned int i = 0; i < SST25_SECTOR && erased; i++) {
		erased = sf->spi->exchange(0xFF) == 0xFF;
	}

	sf->spi->select(false);

	return erased;
}

/* erases the sectors that are whole inside, unless they are erased already */
static int sst25_discard(vfs_blk_t *blk, uint32_t addr, uint32_t length)
{
	sst25_t *sf = (sst25_t *)blk;
	uint32_t end = addr + length;

	if (end > blk->size) {
		return -1;
	}

	for (uint32_t a = (addr + SST25_SECTOR - 1u) & ~(uint32_t)(SST25_SECTOR - 1u); a + SST25_SECTOR <= end;
	     a += SST25_SECTOR) {
		if (!sst25_erased(sf, a) && sst25_erase(sf, a) < 0) {
			return -1;
		}
	}

	return 0;
}

int sst25_init(sst25_t *sf, const vfs_spi_t *spi, uint32_t size, vfs_blk_t *scratch)
{
	if (sf == NULL || spi == NULL || scratch == NULL || scratch->size < SST25_SECTOR || size == 0 ||
	    (size & (SST25_SECTOR - 1u))) {
		return -1;
	}

	sf->blk.read = sst25_read;
	sf->blk.write = sst25_write;
	sf->blk.discard = sst25_discard;
	sf->blk.size = size;
	sf->blk.pos = 0;
	sf->spi = spi;
	sf->scratch = scratch;
	sf->erases = 0;

	spi->select(true);
	spi->exchange(SST25_JEDEC);
	sf->id = (uint32_t)spi->exchange(0xFF) << 16;
	sf->id |= (uint32_t)spi->exchange(0xFF) << 8;
	sf->id |= spi->exchange(0xFF);
	spi->select(false);

	/* no chip: the bus reads all 0 or all 1 */
	if ((sf->id >> 16) == 0x00u || (sf->id >> 16) == 0xFFu) {
		return -1;
	}

	/* the block protection bits are set at power-up */
	sst25_simple(sf, SST25_EWSR);
	spi->select(true);
	spi->exchange(SST25_WRSR);
	spi->exchange(0x00);
	spi->select(false);

	return sst25_wait(sf);
}
