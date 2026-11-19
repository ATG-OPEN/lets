/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * PIC24 / dsPIC33: the hardware side of the scheduler's watchdog and of its stack (task.h "port
 * hooks", docs/porting.md "Watchdog"). Every PIC24 / dsPIC33 project links this file, with
 * LETS_WDT or without: the scheduler kicks on every loop, reads the reset cause at start-up and
 * measures the stack for `top`.
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

#if defined(__dsPIC33C__) || defined(__dsPIC33E__) && defined(_WDTCONL_ON_POSITION)
/* dsPIC33C (dsPIC33CK, dsPIC33CH): the watchdog is WDTCONL.ON (FWDTEN = ON_SW in the config bits),
   fed by writing the key 0x5743 to WDTCONH (WDTCLRKEY); RCON has no SWDTEN */
void port_wdt_start(uint16_t ms)
{
	(void)ms;
	WDTCONLbits.ON = 1;
}

void port_wdt_kick(void)
{
	WDTCONH = 0x5743u;
}
#else
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
#endif

void port_wdt_reset_now(void)
{
	__asm__ volatile("reset");
	for (;;) {
	}
}

/* -------------------------------------------------------------------------- */
/* the stack: from _SP_init up to SPLIM */

#define WDT_STACK_PATTERN 0x5AA5u

extern char _SP_init; /* from the linker: the bottom of the stack */

/* what is above the stack pointer now: port_stack() reads how far it was used */
void port_stack_init(void)
{
	uint16_t *sp;

	__asm__ volatile("mov w15, %0" : "=r"(sp)); /* the stack pointer, read safely */
	for (uint16_t *p = sp + 1; p < (uint16_t *)SPLIM; p++) {
		*p = WDT_STACK_PATTERN;
	}
}

int port_stack(size_t *peak, size_t *size)
{
	uint16_t *start = (uint16_t *)&_SP_init;
	uint16_t *end = (uint16_t *)SPLIM;
	uint16_t *p = end;

	while (p > start && p[-1] == WDT_STACK_PATTERN) {
		p--;
	}

	*peak = (size_t)((char *)p - (char *)start);
	*size = (size_t)((char *)end - (char *)start);

	return 0;
}

/* the stack grows up to SPLIM */
size_t port_stack_left(void)
{
	uint16_t sp;

	__asm__ volatile("mov w15, %0" : "=r"(sp));

	return (sp < SPLIM) ? (size_t)(SPLIM - sp) : 0u;
}

/* MCC's traps.c ends every trap here (its version is weak): record it and start again */
#if LETS_WDT && !defined(LETS_WDT_NO_TRAPS)
void TRAPS_halt_on_error(uint16_t code)
{
	task_trap(code);
}
#endif
