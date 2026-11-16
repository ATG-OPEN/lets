/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file nrf52.h
 * @brief LETS on the Nordic nRF52 (Cortex-M4F): the clock, the UART as a ttyS node, GPIO for gpa.
 *
 * A board calls nrf52_initialize() first (the crystal, the microsecond clock), then
 * task_initialize(nrf52_time_us) as on a PIC, and registers its nodes:
 * @code
 * static uint8_t s0_tx[1024], s0_rx[512];
 * nrf52_tty_t S0 = { .tx_pin = 19, .rx_pin = 15, .baud = 230400,
 *                    .tx = s0_tx, .tx_size = sizeof s0_tx, .rx = s0_rx, .rx_size = sizeof s0_rx };
 * nrf52_tty_register("ttyS0", &S0);
 * @endcode
 * Commands: `ttyS0 -b BAUD`, `ttyS0 -stat`.
 *
 * Resources: TIMER1 (the clock, 1 MHz, 32 bits + an overflow count), UART0 (one tty); with
 * NRF52_TTY_RX_PPI set, UARTE0, NRF52_TTY_RX_TIMER and that PPI channel instead.
 */
#ifndef LETS_NRF52_H
#define LETS_NRF52_H

#include <stdbool.h>
#include <stdint.h>

/** Starts the 32 MHz crystal (USB and the radios need it) and the microsecond clock. */
void nrf52_initialize(void);

/** Microseconds since nrf52_initialize(): for task_initialize(). Never goes back. */
uint64_t nrf52_time_us(void);

/** Busy wait. */
void nrf52_delay_us(uint32_t us);

/**
 * The PPI channel (0 .. 19) that lets the tty receive through EasyDMA, or -1 (the default) for
 * the receive interrupt. An erase of a flash page stalls the CPU ~85 ms, and no interrupt runs
 * meanwhile: the UART holds 6 bytes, so what comes in during a write of /boot is lost. With a
 * channel set, the tty is UARTE0: it receives into the receive queue by EasyDMA (RAM, no CPU),
 * started again at its start each time it is full, and NRF52_TTY_RX_TIMER counts the bytes
 * (each RXDRDY event, through the channel): the count is where EasyDMA writes next. It sends
 * from the send queue by EasyDMA too. The queue must hold what comes in during an erase
 * (115200 baud: 980 bytes; 230400: 1960). Built only: not yet tried on a board.
 */
#ifndef NRF52_TTY_RX_PPI
#define NRF52_TTY_RX_PPI -1
#endif

#ifndef NRF52_TTY_RX_TIMER
#define NRF52_TTY_RX_TIMER NRF_TIMER2 /**< Counts the received bytes, with NRF52_TTY_RX_PPI set. */
#endif

/**
 * A UART: pins (P0.n = n, P1.n = 32 + n), baud, the queues (each holds size - 1 bytes; both in
 * RAM). With NRF52_TTY_RX_PPI set, a receive queue that is not read in time loses what it held,
 * and `lost` counts only the UARTE's own errors.
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
	volatile bool tx_busy;
	volatile uint32_t lost;	   /* bytes that found the receive queue full, or an error */
	volatile uint32_t in, out; /* bytes */
} nrf52_tty_t;

/** UART0 as node @p name (one tty: UART0). */
int nrf52_tty_register(const char *name, nrf52_tty_t *tty);

/** Writes @p n bytes to the tty, waiting for room: for printf before / outside the tasks. */
void nrf52_tty_put(nrf52_tty_t *tty, const void *bytes, unsigned int n);

/** GPIO: a pin as output (level), or input with pull-up. */
void nrf52_gpio_output(uint8_t pin, bool high);
void nrf52_gpio_input(uint8_t pin, bool pull_up);
void nrf52_gpio_set(uint8_t pin, bool high);
bool nrf52_gpio_get(uint8_t pin);

/** Resets the chip. */
void nrf52_reset(void);

/** Why the chip reset (RESETREAS, read and cleared at nrf52_initialize()). */
uint32_t nrf52_reset_reason(void);

/* ---- flash: a small file system's memory (vfs_blk_t), for /boot ---------------------- */

#include "../../vfs/blk.h"

#define NRF52_FLASH_PAGE 4096u
#define NRF52_FLASH_DATA (NRF52_FLASH_PAGE - 8u) /**< the page's last 8 bytes: magic, generation */

/**
 * 4 KiB of data kept in two flash pages at @p base (8 KiB, page aligned): reads come from a RAM
 * copy; a write changes the copy, and the task "flash" writes it to the other page 0.3 s after
 * the last write, with the next generation (an erase stalls the CPU ~85 ms: done in one go, not
 * per write). A reset in between keeps the page before.
 */
typedef struct {
	vfs_blk_t blk; /**< Must be first. */
	uint32_t base;
	uint32_t gen;
	uint8_t live; /* 0 or 1 */
	bool dirty;
	uint32_t dirty_at; /* ms */
	uint8_t data[NRF52_FLASH_PAGE];
} nrf52_flash_t;

int nrf52_flash_init(nrf52_flash_t *f, uint32_t base);

/** Writes what waits now (before a reset). */
void nrf52_flash_sync(nrf52_flash_t *f);

/** Raw flash: erase the page at @p addr; program @p n words (4-byte aligned). */
void nrf52_flash_erase(uint32_t addr);
void nrf52_flash_program(uint32_t addr, const uint32_t *words, unsigned int n);

#endif
