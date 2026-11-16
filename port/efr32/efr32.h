/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file efr32.h
 * @brief LETS on the Silicon Labs EFR32 Series 2 (Cortex-M33; written on the EFR32ZG23): the
 * clock, USART0 as a ttyS node, GPIO for gpa, the flash for /boot.
 *
 * A board calls efr32_initialize() first (the core clock, the microsecond clock), then
 * task_initialize(efr32_time_us) as on a PIC, and registers its nodes:
 * @code
 * static uint8_t s0_tx[1024], s0_rx[512];
 * efr32_tty_t S0 = { .tx_pin = EFR32_PIN('A', 8), .rx_pin = EFR32_PIN('A', 9), .baud = 115200,
 *                    .tx = s0_tx, .tx_size = sizeof s0_tx, .rx = s0_rx, .rx_size = sizeof s0_rx };
 * efr32_tty_register("ttyS0", &S0);
 * @endcode
 * Commands: `ttyS0 -b BAUD`, `ttyS0 -stat`.
 *
 * Registers through the vendor's CMSIS device header (em_device.h), no emlib. The whole program
 * runs secure (-DSL_TRUSTZONE_SECURE), as the chip comes out of reset.
 *
 * Resources: the 39 MHz crystal (HFXO) as SYSCLK, HCLK, PCLK and peripheral groups A and C, or
 * HFRCODPLL at 38 MHz when the crystal does not start; TIMER0 (the clock, 1 MHz, 32 bits + an
 * overflow count), USART0 (one tty), MSC (the flash).
 */
#ifndef LETS_EFR32_H
#define LETS_EFR32_H

#include <stdbool.h>
#include <stdint.h>

/** A pin: port letter ('A'..'D') and number, as one byte (port * 16 + pin). */
#define EFR32_PIN(port, pin) ((uint8_t)((((port) - 'A') << 4) | (pin)))

/** The board's crystal (Hz) and its load capacitance (CTUNE, 0..255; from the board's
 * sl_clock_manager_oscillator_config.h); a board may set them with -D. */
#ifndef EFR32_HFXO_HZ
#define EFR32_HFXO_HZ 39000000u
#endif
#ifndef EFR32_HFXO_CTUNE
#define EFR32_HFXO_CTUNE 121u
#endif

/** The core clock (Hz) after efr32_initialize(): EFR32_HFXO_HZ, or 38 MHz (HFRCODPLL). */
extern uint32_t efr32_hz;

/** Moves the core to the crystal (or HFRCODPLL at 38 MHz) and starts the microsecond clock. */
void efr32_initialize(void);

/** Microseconds since efr32_initialize(): for task_initialize(). Never goes back. */
uint64_t efr32_time_us(void);

/** Busy wait. */
void efr32_delay_us(uint32_t us);

/**
 * The LDMA channel that receives for the tty (0 .. 7), or -1 (the default) for the receive
 * interrupt. The LDMA goes on while the flash is erased or written with interrupts off
 * (~12 ms a page), which the interrupt cannot: the USART holds only 2 bytes, so with the
 * interrupt what comes in during a write of /boot is lost. Opt in with -DEFR32_TTY_LDMA_CH=7
 * (or a channel the board does not use). Not yet tried on a board: off by default until it is.
 */
#ifndef EFR32_TTY_LDMA_CH
#define EFR32_TTY_LDMA_CH -1
#endif

/**
 * A UART: pins (EFR32_PIN), baud, the queues (each holds size - 1 bytes; the receive queue at
 * most 2048 bytes). With the LDMA a receive queue that is not read in time loses what it held,
 * and `lost` counts only the USART's own overruns.
 */
typedef struct {
	uint8_t tx_pin;
	uint8_t rx_pin;
	uint32_t baud;
	uint8_t *tx;
	uint8_t *rx;
	uint16_t tx_size;
	uint16_t rx_size;

	volatile uint16_t tx_head; /* the next byte to send */
	volatile uint16_t tx_tail; /* where a write puts the next byte */
	volatile uint16_t rx_head;
	volatile uint16_t rx_tail;
	volatile uint32_t lost;	   /* bytes that found the receive queue full, or an overrun */
	volatile uint32_t in, out; /* bytes */
} efr32_tty_t;

/** USART0 as node @p name (one tty: USART0). */
int efr32_tty_register(const char *name, efr32_tty_t *tty);

/** Writes @p n bytes to the tty, waiting for room: for printf before / outside the tasks. */
void efr32_tty_put(efr32_tty_t *tty, const void *bytes, unsigned int n);

/** GPIO: a pin as push-pull output (level), or input (with pull-up). */
void efr32_gpio_output(uint8_t pin, bool high);
void efr32_gpio_input(uint8_t pin, bool pull_up);
void efr32_gpio_set(uint8_t pin, bool high);
bool efr32_gpio_get(uint8_t pin);

/** Resets the chip. */
void efr32_reset(void);

/** Why the chip reset (EMU RSTCAUSE, read and cleared at efr32_initialize()). */
uint32_t efr32_reset_reason(void);

/* ---- flash: a small file system's memory (vfs_blk_t), for /boot ---------------------- */

#include "../../vfs/blk.h"

#define EFR32_FLASH_PAGE  8192u /**< Series 2 main flash page */
#define EFR32_FLASH_BLOCK 4096u /**< what one copy keeps */
#define EFR32_FLASH_DATA  (EFR32_FLASH_BLOCK - 8u) /**< then magic, generation */

/**
 * 4 KiB of data kept in two flash pages at @p base (16 KiB, page aligned): reads come from a RAM
 * copy; a write changes the copy, and the task "flash" writes it to the other page 0.3 s after
 * the last write, with the next generation. A reset in between keeps the page before. The
 * erase and the writes run from RAM with interrupts off (~15 ms): with the receive interrupt
 * (the default) the tty loses what comes in meanwhile; with EFR32_TTY_LDMA_CH set it receives
 * through the LDMA.
 */
typedef struct {
	vfs_blk_t blk; /**< Must be first. */
	uint32_t base;
	uint32_t gen;
	uint8_t live; /* 0 or 1 */
	bool dirty;
	uint32_t dirty_at; /* ms */
	uint8_t data[EFR32_FLASH_BLOCK];
} efr32_flash_t;

int efr32_flash_init(efr32_flash_t *f, uint32_t base);

/** Writes what waits now (before a reset). */
void efr32_flash_sync(efr32_flash_t *f);

/** Raw flash: erase the page at @p addr; program @p n words (4-byte aligned). 0: done. */
int efr32_flash_erase(uint32_t addr);
int efr32_flash_program(uint32_t addr, const uint32_t *words, unsigned int n);

#endif
