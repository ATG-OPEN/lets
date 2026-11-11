/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * EFR32ZG23 (512 KiB flash at 0x08000000, 64 KiB RAM) for LETS, with the vendor's startup file
 * (startup_efr32zg23.c, built with -D__START=main: Reset_Handler copies .data from __etext, zeroes
 * .bss, calls main). Functions in section .ram_text (flash.c) go into .data, so they are copied
 * to RAM with it. A board keeps its own flash pages (/boot, keys) at the top: it sets
 * __flash_reserved (bytes) with -Wl,--defsym, and the program must end below.
 */
__STACK_SIZE = DEFINED(__STACK_SIZE) ? __STACK_SIZE : 0x2000;
__HEAP_SIZE = DEFINED(__HEAP_SIZE) ? __HEAP_SIZE : 0x400;
__flash_reserved = DEFINED(__flash_reserved) ? __flash_reserved : 0;

MEMORY
{
	FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
	RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}

ENTRY(Reset_Handler)

SECTIONS
{
	.text :
	{
		KEEP(*(.vectors))
		*(.text*)
		*(.libc*)	/* the kernel's C-library functions (vfs/posix.c, kernel/alloc.c) */
		KEEP(*(.init))
		KEEP(*(.fini))
		*(.rodata*)
		. = ALIGN(4);
	} > FLASH

	.ARM.exidx :
	{
		__exidx_start = .;
		*(.ARM.exidx* .gnu.linkonce.armexidx.*)
		__exidx_end = .;
	} > FLASH

	. = ALIGN(4);
	__etext = .;

	.data : AT (__etext)
	{
		__data_start__ = .;
		*(.ram_text*)
		*(.data*)
		. = ALIGN(4);
		__data_end__ = .;
	} > RAM

	.bss (NOLOAD) :
	{
		. = ALIGN(4);
		__bss_start__ = .;
		*(.bss*)
		*(COMMON)
		. = ALIGN(4);
		__bss_end__ = .;
	} > RAM

	.heap (NOLOAD) :
	{
		. = ALIGN(8);
		__HeapBase = .;
		. = . + __HEAP_SIZE;
		__HeapLimit = .;
	} > RAM

	.stack (NOLOAD) :
	{
		. = ALIGN(8);
		__StackLimit = .;	/* CMSIS's __STACK_LIMIT */
		. = . + __STACK_SIZE;
		__StackTop = .;		/* CMSIS's __INITIAL_SP */
	} > RAM

	/* the startup's CopyToRam(): nothing apart (.ram_text rides in .data) */
	__lma_ramfuncs_start__ = __etext;
	__lma_ramfuncs_end__ = __etext;
	__ramfuncs_start__ = __data_start__;

	__flash_end__ = __etext + SIZEOF(.data);
	ASSERT(__flash_end__ <= ORIGIN(FLASH) + LENGTH(FLASH) - __flash_reserved, "the program runs into the reserved flash pages")

	/DISCARD/ : { *(.ARM.attributes) }
}
