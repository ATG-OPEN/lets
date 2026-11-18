/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * PIC24 / dsPIC33: the hardware side of the scheduler's watchdog (task.h "port hooks",
 * docs/porting.md "Watchdog"). Every PIC24 / dsPIC33 project links this file, with LETS_WDT
 * or without: the scheduler kicks on every loop and reads the reset cause at start-up.
 *
 * The period is in the config bits (FWPSA, WDTPS); with FWDTEN = ON_SWDTEN the kernel turns
 * the watchdog on. A board with its own hooks leaves this file out.
 */
#include "../../kernel/task.h"

#include <xc.h>

#define WDT_RESET_FLAGS 0xC2DFu /* in RCON: the flags, not the settings */

task_reset_t port_wdt_reset_cause(uint16_t *raw)
{
	uint16_t r = RCON;

	*raw = r;
	RCON &= (uint16_t)~WDT_RESET_FLAGS;

	if (r & 0xC000u) { /* TRAPR, IOPUWR (illegal opcode) */
		return TASK_RESET_TRAP;
	}

	return (r & 0x0010u)   ? TASK_RESET_WATCHDOG
	       : (r & 0x0040u) ? TASK_RESET_SOFTWARE
	       : (r & 0x0080u) ? TASK_RESET_PIN
	       : (r & 0x0001u) ? TASK_RESET_POWER_ON
	       : (r & 0x0002u) ? TASK_RESET_BROWN_OUT
			       : TASK_RESET_OTHER;
}

void port_wdt_start(uint16_t ms)
{
	(void)ms;
	RCONbits.SWDTEN = 1;
}

/* off (SWDTEN 0) it does nothing; on from the config bits (FWDTEN ON) it keeps it fed */
void port_wdt_kick(void)
{
	__asm__ volatile("clrwdt");
}

void port_wdt_reset_now(void)
{
	__asm__ volatile("reset");
	for (;;) {
	}
}

/* MCC's traps.c ends every trap here (its version is weak): record it and start again */
#if LETS_WDT && !defined(LETS_WDT_NO_TRAPS)
void TRAPS_halt_on_error(uint16_t code)
{
	task_trap(code);
}
#endif
