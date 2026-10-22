/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "ee25.h"

#include <stddef.h>

#define EE25_WRITE 0x02u
#define EE25_READ  0x03u
#define EE25_RDSR  0x05u
#define EE25_WREN  0x06u
#define EE25_WIP   0x01u /* status: write in progress */

#define EE25_POLLS 30000u /* status reads before a write counts as failed (> 10 ms) */

/* selects the chip and sends a command with its address */
static void ee25_command(ee25_t *ee, uint8_t command, uint32_t addr)
{
	const vfs_spi_t *spi = ee->spi;

	spi->select(true);
	spi->exchange(command);

	if (ee->blk.size > 0x10000ul) {
		spi->exchange((uint8_t)(addr >> 16));
	}

	spi->exchange((uint8_t)(addr >> 8));
	spi->exchange((uint8_t)addr);
}

static int ee25_read(vfs_blk_t *blk, uint32_t addr, void *buffer, unsigned int length)
{
	ee25_t *ee = (ee25_t *)blk;
	uint8_t *p = buffer;

	if (addr + length > blk->size) {
		return -1;
	}

	ee25_command(ee, EE25_READ, addr);

	for (unsigned int i = 0; i < length; i++) {
		p[i] = ee->spi->exchange(0xFF);
	}

	ee->spi->select(false);

	return 0;
}

/* waits until the chip has finished programming a page */
static int ee25_wait(ee25_t *ee)
{
	const vfs_spi_t *spi = ee->spi;
	uint8_t status = EE25_WIP;

	spi->select(true);
	spi->exchange(EE25_RDSR);

	for (unsigned int i = 0; i < EE25_POLLS && (status & EE25_WIP); i++) {
		status = spi->exchange(0xFF);
	}

	spi->select(false);

	return (status & EE25_WIP) ? -1 : 0;
}

static int ee25_write(vfs_blk_t *blk, uint32_t addr, const void *buffer, unsigned int length)
{
	ee25_t *ee = (ee25_t *)blk;
	const uint8_t *p = buffer;

	if (addr + length > blk->size) {
		return -1;
	}

	while (length > 0) {
		/* up to the end of the page */
		unsigned int n = ee->page - (unsigned int)(addr & (ee->page - 1u));

		if (n > length) {
			n = length;
		}

		ee->spi->select(true);
		ee->spi->exchange(EE25_WREN);
		ee->spi->select(false);

		ee25_command(ee, EE25_WRITE, addr);

		for (unsigned int i = 0; i < n; i++) {
			ee->spi->exchange(p[i]);
		}

		ee->spi->select(false);

		if (ee25_wait(ee) < 0) {
			return -1;
		}

		addr += n;
		p += n;
		length -= n;
	}

	return 0;
}

int ee25_init(ee25_t *ee, const vfs_spi_t *spi, uint32_t size, uint16_t page)
{
	if (ee == NULL || spi == NULL || size <= 512u || page < 16u || (page & (page - 1u))) {
		return -1;
	}

	ee->blk.read = ee25_read;
	ee->blk.write = ee25_write;
	ee->blk.size = size;
	ee->blk.pos = 0;
	ee->spi = spi;
	ee->page = page;

	return 0;
}
