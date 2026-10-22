/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "sram23.h"

#include <stddef.h>

#define SRAM23_WRITE	  0x02u
#define SRAM23_READ	  0x03u
#define SRAM23_WRMR	  0x01u /* write mode register */
#define SRAM23_SEQUENTIAL 0x40u

/* selects the chip and sends a command with its address */
static void sram23_command(sram23_t *ram, uint8_t command, uint32_t addr)
{
	const vfs_spi_t *spi = ram->spi;

	spi->select(true);
	spi->exchange(command);

	if (ram->blk.size > 0x10000ul) {
		spi->exchange((uint8_t)(addr >> 16));
	}

	spi->exchange((uint8_t)(addr >> 8));
	spi->exchange((uint8_t)addr);
}

static int sram23_read(vfs_blk_t *blk, uint32_t addr, void *buffer, unsigned int length)
{
	sram23_t *ram = (sram23_t *)blk;
	uint8_t *p = buffer;

	if (addr + length > blk->size) {
		return -1;
	}

	sram23_command(ram, SRAM23_READ, addr);

	for (unsigned int i = 0; i < length; i++) {
		p[i] = ram->spi->exchange(0xFF);
	}

	ram->spi->select(false);

	return 0;
}

static int sram23_write(vfs_blk_t *blk, uint32_t addr, const void *buffer, unsigned int length)
{
	sram23_t *ram = (sram23_t *)blk;
	const uint8_t *p = buffer;

	if (addr + length > blk->size) {
		return -1;
	}

	sram23_command(ram, SRAM23_WRITE, addr);

	for (unsigned int i = 0; i < length; i++) {
		ram->spi->exchange(p[i]);
	}

	ram->spi->select(false);

	return 0;
}

int sram23_init(sram23_t *ram, const vfs_spi_t *spi, uint32_t size)
{
	if (ram == NULL || spi == NULL || size == 0) {
		return -1;
	}

	ram->blk.read = sram23_read;
	ram->blk.write = sram23_write;
	ram->blk.size = size;
	ram->blk.pos = 0;
	ram->spi = spi;

	spi->select(true);
	spi->exchange(SRAM23_WRMR);
	spi->exchange(SRAM23_SEQUENTIAL);
	spi->select(false);

	return 0;
}
