/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * EFR32 Series 2 flash as a LETS block device: see efr32.h. The MSC directly, as emlib's
 * MSC_ErasePage / MSC_WriteWord: the erase and the writes run from RAM (section .ram_text,
 * which the linker script puts in .data) with interrupts off, since the flash cannot be read
 * meanwhile and the interrupt handlers live there. ~12 ms an 8 KiB page, ~10 us a word.
 */
#include "efr32.h"

#include "../../kernel/task.h"

#include <em_device.h>
#include <string.h>

#define FLASH_MAGIC 0x4C455453u /* "LETS" */

#define RAMFUNC __attribute__((section(".ram_text"), noinline, long_call))

/* BUSY or PENDING, then LOCKED / INVADDR / REGLOCK are errors. TIMEOUT comes up after a
 * burst's WRITEEND even when all went well (emlib ignores it too). */
static RAMFUNC int msc_wait(void)
{
	while (MSC->STATUS & (MSC_STATUS_BUSY | MSC_STATUS_PENDING)) {
	}
	return (MSC->STATUS & (MSC_STATUS_LOCKED | MSC_STATUS_INVADDR | MSC_STATUS_REGLOCK)) ? -1 : 0;
}

static RAMFUNC int erase_page(uint32_t addr)
{
	int r = -1;

	MSC->LOCK = MSC_LOCK_LOCKKEY_UNLOCK;
	MSC->WRITECTRL_SET = MSC_WRITECTRL_WREN;
	MSC->ADDRB = addr;
	if (!(MSC->STATUS & (MSC_STATUS_INVADDR | MSC_STATUS_LOCKED))) {
		MSC->WRITECMD = MSC_WRITECMD_ERASEPAGE;
		r = msc_wait();
	}
	MSC->WRITECTRL_CLR = MSC_WRITECTRL_WREN;
	return r;
}

/* n words, not across a page boundary */
static RAMFUNC int write_burst(uint32_t addr, const uint32_t *words, unsigned int n)
{
	int r = -1;

	MSC->LOCK = MSC_LOCK_LOCKKEY_UNLOCK;
	MSC->WRITECTRL_SET = MSC_WRITECTRL_WREN;
	MSC->ADDRB = addr;
	if (!(MSC->STATUS & (MSC_STATUS_INVADDR | MSC_STATUS_LOCKED))) {
		for (unsigned int i = 0; i < n; i++) {
			while (!(MSC->STATUS & MSC_STATUS_WDATAREADY)) {
			}
			MSC->WDATA = words[i];
		}
		MSC->WRITECMD = MSC_WRITECMD_WRITEEND;
		r = msc_wait();
	}
	MSC->WRITECTRL_CLR = MSC_WRITECTRL_WREN;
	return r;
}

int efr32_flash_erase(uint32_t addr)
{
	uint32_t primask = __get_PRIMASK();

	__disable_irq();
	int r = erase_page(addr & ~(EFR32_FLASH_PAGE - 1u));
	__set_PRIMASK(primask);
	return r;
}

int efr32_flash_program(uint32_t addr, const uint32_t *words, unsigned int n)
{
	int r = 0;

	while (n > 0 && r == 0) {
		unsigned int room = (EFR32_FLASH_PAGE - (addr & (EFR32_FLASH_PAGE - 1u))) / 4u;
		unsigned int k = n < room ? n : room;
		uint32_t primask = __get_PRIMASK();

		/* the words may be in flash: copy a little at a time to the stack */
		uint32_t buf[64];

		if (k > 64u) {
			k = 64u;
		}
		memcpy(buf, words, 4u * k);
		__disable_irq();
		r = write_burst(addr, buf, k);
		__set_PRIMASK(primask);
		addr += 4u * k;
		words += k;
		n -= k;
	}
	return r;
}

static const uint32_t *page(const efr32_flash_t *f, uint8_t which)
{
	return (const uint32_t *)(f->base + (uint32_t)which * EFR32_FLASH_PAGE);
}

static bool page_valid(const uint32_t *p, uint32_t *gen)
{
	if (p[EFR32_FLASH_DATA / 4u] != FLASH_MAGIC) {
		return false;
	}
	*gen = p[EFR32_FLASH_DATA / 4u + 1u];
	return *gen != 0xFFFFFFFFu;
}

void efr32_flash_sync(efr32_flash_t *f)
{
	if (!f->dirty) {
		return;
	}

	uint8_t next = f->live ^ 1u;
	uint32_t addr = f->base + (uint32_t)next * EFR32_FLASH_PAGE;
	uint32_t trailer[2] = {FLASH_MAGIC, f->gen + 1u};

	efr32_flash_erase(addr);
	efr32_flash_program(addr, (const uint32_t *)f->data, EFR32_FLASH_DATA / 4u);
	efr32_flash_program(addr + EFR32_FLASH_DATA, trailer, 2);

	f->gen++;
	f->live = next;
	f->dirty = false;
}

static int flash_read(vfs_blk_t *blk, uint32_t addr, void *buffer, unsigned int length)
{
	efr32_flash_t *f = (efr32_flash_t *)blk;

	if (addr + length > EFR32_FLASH_DATA) {
		return -1;
	}
	memcpy(buffer, f->data + addr, length);
	return 0;
}

static int flash_write(vfs_blk_t *blk, uint32_t addr, const void *buffer, unsigned int length)
{
	efr32_flash_t *f = (efr32_flash_t *)blk;

	if (addr + length > EFR32_FLASH_DATA) {
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
	efr32_flash_t *f = arg;

	if (f->dirty && (uint32_t)(now / 1000u) - f->dirty_at >= 300u) {
		efr32_flash_sync(f);
	}
	return 0;
}

int efr32_flash_init(efr32_flash_t *f, uint32_t base)
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
		memcpy(f->data, page(f, f->live), EFR32_FLASH_DATA);
	} else {
		memset(f->data, 0xFF, EFR32_FLASH_DATA);
	}
	f->dirty = false;

	f->blk.read = flash_read;
	f->blk.write = flash_write;
	f->blk.discard = NULL;
	f->blk.size = EFR32_FLASH_DATA;
	f->blk.pos = 0;

	/* f is not from the heap: the kernel never frees it (alloc_change_owner ignores it) */
	return task_create("flash", flash_task, f) != NULL ? 0 : -1;
}
