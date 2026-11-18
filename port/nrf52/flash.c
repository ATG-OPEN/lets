/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * nRF52 flash as a LETS block device: see nrf52.h. NVMC directly. While the NVMC erases or
 * programs, the CPU stalls (no interrupt runs): ~85 ms an erase, ~41 us a word.
 */
#include "nrf52.h"

#include "../../kernel/task.h"

#include <nrf.h>
#include <string.h>

#define FLASH_MAGIC 0x4C455453u /* "LETS" */

void nrf52_flash_erase(uint32_t addr)
{
	task_busy(); /* ~85 ms: a long write erases page after page in one call */
	NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Een;
	while (!NRF_NVMC->READY) {
	}
	NRF_NVMC->ERASEPAGE = addr;
	while (!NRF_NVMC->READY) {
	}
	NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
}

void nrf52_flash_program(uint32_t addr, const uint32_t *words, unsigned int n)
{
	NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen;
	for (unsigned int i = 0; i < n; i++) {
		while (!NRF_NVMC->READY) {
		}
		*(volatile uint32_t *)(addr + 4u * i) = words[i];
	}
	while (!NRF_NVMC->READY) {
	}
	NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
}

static const uint32_t *page(const nrf52_flash_t *f, uint8_t which)
{
	return (const uint32_t *)(f->base + (uint32_t)which * NRF52_FLASH_PAGE);
}

static bool page_valid(const uint32_t *p, uint32_t *gen)
{
	if (p[NRF52_FLASH_DATA / 4u] != FLASH_MAGIC) {
		return false;
	}
	*gen = p[NRF52_FLASH_DATA / 4u + 1u];
	return *gen != 0xFFFFFFFFu;
}

void nrf52_flash_sync(nrf52_flash_t *f)
{
	if (!f->dirty) {
		return;
	}

	uint8_t next = f->live ^ 1u;
	uint32_t addr = f->base + (uint32_t)next * NRF52_FLASH_PAGE;
	uint32_t trailer[2] = {FLASH_MAGIC, f->gen + 1u};

	nrf52_flash_erase(addr);
	nrf52_flash_program(addr, (const uint32_t *)f->data, NRF52_FLASH_DATA / 4u);
	nrf52_flash_program(addr + NRF52_FLASH_DATA, trailer, 2);

	f->gen++;
	f->live = next;
	f->dirty = false;
}

/* vfs_sync(): `sync`, `reboot` */
static int flash_sync(vfs_blk_t *blk)
{
	nrf52_flash_sync((nrf52_flash_t *)blk);
	return 0;
}

static int flash_read(vfs_blk_t *blk, uint32_t addr, void *buffer, unsigned int length)
{
	nrf52_flash_t *f = (nrf52_flash_t *)blk;

	if (addr + length > NRF52_FLASH_DATA) {
		return -1;
	}
	memcpy(buffer, f->data + addr, length);
	return 0;
}

static int flash_write(vfs_blk_t *blk, uint32_t addr, const void *buffer, unsigned int length)
{
	nrf52_flash_t *f = (nrf52_flash_t *)blk;

	if (addr + length > NRF52_FLASH_DATA) {
		return -1;
	}
	if (memcmp(f->data + addr, buffer, length) != 0) {
		memcpy(f->data + addr, buffer, length);
		f->dirty = true;
	}
	f->dirty_at = (uint32_t)(task_get_system_time() / 1000u);
	return 0;
}

static int flash_task(void *arg, uint64_t now)
{
	nrf52_flash_t *f = arg;

	if (f->dirty && (uint32_t)(now / 1000u) - f->dirty_at >= 300u) {
		nrf52_flash_sync(f);
	}
	return 0;
}

int nrf52_flash_init(nrf52_flash_t *f, uint32_t base)
{
	uint32_t g0 = 0, g1 = 0;
	bool v0 = false, v1 = false;

	f->base = base;
	v0 = page_valid(page(f, 0), &g0);
	v1 = page_valid(page(f, 1), &g1);

	if (v0 && (!v1 || (int32_t)(g0 - g1) > 0)) {
		f->live = 0;
		f->gen = g0;
	} else if (v1) {
		f->live = 1;
		f->gen = g1;
	} else {
		f->live = 1; /* nothing yet: the first write goes to page 0 */
		f->gen = 0;
	}

	if (v0 || v1) {
		memcpy(f->data, page(f, f->live), NRF52_FLASH_DATA);
	} else {
		memset(f->data, 0xFF, NRF52_FLASH_DATA);
	}
	f->dirty = false;

	f->blk.read = flash_read;
	f->blk.write = flash_write;
	f->blk.discard = NULL;
	f->blk.sync = flash_sync;
	f->blk.size = NRF52_FLASH_DATA;
	f->blk.pos = 0;

	/* f is not from the heap: the kernel never frees it (alloc_change_owner ignores it) */
	return task_create("flash", flash_task, f) != NULL ? 0 : -1;
}
