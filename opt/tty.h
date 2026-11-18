/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file tty.h
 * @brief A UART of a PIC24F or dsPIC33E as a node, sent and received by interrupts.
 *
 * A write puts the bytes in a queue and returns; the transmit interrupt sends
 * them. It waits only for what does not fit in the queue, so make the queue
 * hold the largest packet. The modes drive an RS485 direction pin, if there is
 * one, from the interrupt, when the last bit has gone:
 *
 *     STD    no pin
 *     SPX1   high to send, low after the last bit
 *     SPX2   low to send, high after the last bit
 *     DPX1   high to send, and it stays high
 *     DPX2   low to send, and it stays low
 *
 * DPX is a full-duplex RS485 port on a straight cable: the pin picks the pair it sends on, so
 * the two ends must be in opposite modes. A DPX port that receives nothing for 2 to 4 s (a
 * pseudo-random wait per port, tty_dpx.h) swaps DPX1 <-> DPX2 by itself, so two ends that start
 * alike find each other; one that hears bytes never swaps (`-a 0`, or .fixed, turns it off;
 * -DTTY_DPX_AUTO=0 leaves it out of the build).
 * Only the reading of a port (a task's read(), even one that finds nothing) runs the swap.
 *
 * A board gives each port its queues, and its pin:
 * @code
 * static uint8_t s1_tx[160], s1_rx[128];
 *
 * tty_args_t S1 = {
 *     .port = 1, .baud = 115200, .clock = _XTAL_FREQ,
 *     .tx = s1_tx, .tx_size = sizeof(s1_tx),
 *     .rx = s1_rx, .rx_size = sizeof(s1_rx),
 *     .pin = &LATB, .pin_mask = 1u << 5,        // set as an output by the board
 * };
 *
 * tty_register_node("ttyS1", &S1);
 * @endcode
 * Ports can share a LATx: a task changes a pin with all interrupts held off,
 * the UART interrupts (all of one priority) change theirs in turn. Other code
 * that changes bits of that LATx must not undo it: use a bit write
 * (`LATBbits.LATB4 = 1`, one instruction) or hold interrupts off too.
 *
 * Commands: `ttyS1 -b BAUD`, `ttyS1 -m STD/SPX1/SPX2/DPX1/DPX2`, `ttyS1 -a 0/1` (DPX auto
 * direction, on by default), `ttyS1 -stat` (DPX: also the swaps).
 *
 * It owns the UART: leave it out of MCC, or take MCC's uartN.c out of the
 * project. TTY_PORTS (1) is the most ports a project uses, up to 6.
 */

#ifndef TTY_H
#define TTY_H

#include "tty_dpx.h"

#include <stdint.h>
#include <stdbool.h>

#ifndef TTY_PORTS
#define TTY_PORTS 1
#endif

#ifndef TTY_DPX_AUTO
#define TTY_DPX_AUTO 1 /**< 0: DPX keeps its direction: ~600 B less code, 10 B less RAM a port. */
#endif

typedef enum {
	TTY_MODE_STANDARD = 0,
	TTY_MODE_RS_SPX1 = 1,
	TTY_MODE_RS_SPX2 = 2,
	TTY_MODE_RS_DPX1 = 3,
	TTY_MODE_RS_DPX2 = 4,
} tty_mode_t;

typedef struct {
	uint8_t port;	   /**< 1 .. TTY_PORTS. */
	uint32_t baud;	   /**< Up to about 4 Mbaud; 0 keeps what is set. */
	uint32_t clock;	   /**< The oscillator, twice the instruction clock: _XTAL_FREQ. */
	uint8_t *tx;	   /**< The send queue: holds tx_size - 1 bytes. */
	uint8_t *rx;	   /**< The receive queue: holds rx_size - 1 bytes. */
	uint8_t tx_size;
	uint8_t rx_size;
	volatile uint16_t *pin; /**< LATx of the direction pin; NULL: none. */
	uint16_t pin_mask;	/**< Its bit. */
	tty_mode_t mode;
	bool swap;		/**< A write of 0 bytes swaps 1 and 2 of the mode: for a link protocol that switches its framing. */
	bool fixed;		/**< DPX keeps its direction: no swap while it hears nothing. */

	volatile uint8_t tx_head; /* the next byte to send */
	volatile uint8_t tx_tail; /* where a write puts the next byte */
	volatile uint8_t rx_head;
	volatile uint8_t rx_tail;
	volatile uint16_t lost;	  /* bytes that found the receive queue full, or an overrun */
#if TTY_DPX_AUTO
	tty_dpx_t dpx;		  /**< DPX: when to swap the direction (private). */
	uint16_t swaps;		  /**< DPX: swaps made while nothing was heard (private). */
#endif
} tty_args_t;

/** @brief Starts the UART and makes it the node @p name. @return 0 or -1. */
int tty_register_node(char *name, tty_args_t *args);

#endif /* TTY_H */
