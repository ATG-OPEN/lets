/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * PIC24FJ: the program flash under vfs/dev/flash24.c (flash24.h, "the port's hooks"): a read of
 * one instruction, a page erase and a double-word program, on the NVM registers. A board that
 * keeps files in its own flash links this file with vfs/dev/flash24.c.
 */
#include "../../vfs/dev/flash24.h"

#include <xc.h>
#include <stdint.h>

#define FLASH24_LATCH	  0xFAu	  /* TBLPAG of the write latches */
#define FLASH24_ERASE_OP  0x4003u /* WREN, page erase */
#define FLASH24_DOUBLE_OP 0x4001u /* WREN, double-word program */

uint32_t flash24_hw_read(uint32_t addr)
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

	/*
	 * No interrupt from the unlock sequence to the end of the operation: an
	 * interrupt taken while it runs is an address error trap. The builtin
	 * does not do this. UART bytes pile up in the FIFO meanwhile; an erase
	 * takes about 20 ms, so a fast line overruns and loses bytes.
	 */
	INTCON2bits.GIE = 0;
	__builtin_nop();
	__builtin_nop();

	__builtin_write_NVM(); /* the unlock sequence, then WR; the CPU stalls until done */

	while (NVMCONbits.WR) {
	}

	INTCON2bits.GIE = 1;

	NVMCONbits.WREN = 0;

	return NVMCONbits.WRERR ? -1 : 0;
}

int flash24_hw_erase(uint32_t addr)
{
	return flash24_hw_nvm(addr, FLASH24_ERASE_OP);
}

int flash24_hw_program(uint32_t addr, uint32_t w0, uint32_t w1)
{
	uint16_t page = TBLPAG;

	TBLPAG = FLASH24_LATCH;
	__builtin_tblwtl(0, (uint16_t)w0);
	__builtin_tblwth(0, (uint16_t)(w0 >> 16));
	__builtin_tblwtl(2, (uint16_t)w1);
	__builtin_tblwth(2, (uint16_t)(w1 >> 16));
	TBLPAG = page;

	if (flash24_hw_nvm(addr, FLASH24_DOUBLE_OP) < 0) {
		return -1;
	}

	/* a program that did not happen raises no error: look */
	return ((uint16_t)flash24_hw_read(addr) == (uint16_t)w0 &&
		(uint16_t)flash24_hw_read(addr + 2u) == (uint16_t)w1) ? 0 : -1;
}
