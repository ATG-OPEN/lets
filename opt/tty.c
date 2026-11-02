/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * See tty.h. The interrupts are written here, not called through a pointer: an
 * interrupt that calls a function must save every register, and these run for
 * each byte. 8-bit indexes can be read and written with no lock: a queue has
 * one writer at each end.
 */

#include "tty.h"

#include "../vfs/vfs.h"

#include <xc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* the registers of a UART, one after the other from UxMODE */
typedef struct {
	volatile uint16_t mode;
	volatile uint16_t sta;
	volatile uint16_t txreg;
	volatile uint16_t rxreg;
	volatile uint16_t brg;
} tty_uart_t;

#define TTY_MODE_UARTEN	 0x8000u
#define TTY_MODE_BRGH	 0x0008u
#define TTY_STA_UTXEN	 0x0400u
#define TTY_STA_UTXBF	 0x0200u
#define TTY_STA_TRMT	 0x0100u
#define TTY_STA_OERR	 0x0002u
#define TTY_STA_URXDA	 0x0001u
#define TTY_STA_UTXISEL0 0x2000u /* interrupt when the last bit has gone, not on each byte */

static tty_args_t *tty_ports[TTY_PORTS];

static tty_uart_t *const tty_uarts[TTY_PORTS] = {
	(tty_uart_t *)&U1MODE,
#if TTY_PORTS > 1
	(tty_uart_t *)&U2MODE,
#endif
#if TTY_PORTS > 2
	(tty_uart_t *)&U3MODE,
#endif
#if TTY_PORTS > 3
	(tty_uart_t *)&U4MODE,
#endif
#if TTY_PORTS > 4
	(tty_uart_t *)&U5MODE,
#endif
#if TTY_PORTS > 5
	(tty_uart_t *)&U6MODE,
#endif
};

#define TTY_UART(port) (tty_uarts[(port) - 1])

static inline __attribute__((always_inline)) bool tty_half_duplex(const tty_args_t *a)
{
	return a->mode == TTY_MODE_RS_SPX1 || a->mode == TTY_MODE_RS_SPX2;
}

/* the direction pin; DPX leaves it on, so only a change of mode and SPX need the off state */
static inline __attribute__((always_inline)) void tty_pin(const tty_args_t *a, bool send)
{
	if (a->pin == NULL || a->mode == TTY_MODE_STANDARD) {
		return;
	}

	bool high = (a->mode == TTY_MODE_RS_SPX1 || a->mode == TTY_MODE_RS_DPX1) == send;

	if (high) {
		*a->pin |= a->pin_mask;
	} else {
		*a->pin &= (uint16_t)~a->pin_mask;
	}
}

static inline __attribute__((always_inline)) void tty_receive(tty_args_t *a, tty_uart_t *u)
{
	/* after an overrun the UART takes nothing until this is cleared */
	if (u->sta & TTY_STA_OERR) {
		u->sta &= (uint16_t)~TTY_STA_OERR;
		a->lost++;
	}

	while (u->sta & TTY_STA_URXDA) {
		uint8_t byte = (uint8_t)u->rxreg;
		uint8_t next = (uint8_t)(a->rx_tail + 1u);

		next = (next == a->rx_size) ? 0u : next;

		if (next == a->rx_head) {
			a->lost++;
		} else {
			a->rx[a->rx_tail] = byte;
			a->rx_tail = next;
		}
	}
}

/*
 * Returns false when there is nothing more to do. The UART's FIFO takes what it
 * can; a queue that ran out on half duplex waits for the shifter to empty.
 * TRMT can still read 1 just after a write, so it counts only when nothing was
 * written now; a wait that may have missed the end asks for one more interrupt.
 */
static inline __attribute__((always_inline)) bool tty_transmit(tty_args_t *a, tty_uart_t *u,
							       bool *again)
{
	bool sent = false;

	while (!(u->sta & TTY_STA_UTXBF) && a->tx_head != a->tx_tail) {
		uint8_t head = a->tx_head;

		u->txreg = a->tx[head++];
		a->tx_head = (head == a->tx_size) ? 0u : head;
		sent = true;
	}

	if (a->tx_head != a->tx_tail) {
		return true;
	}

	if (!tty_half_duplex(a) || a->pin == NULL) {
		return false;
	}

	if (!sent && (u->sta & TTY_STA_TRMT)) {
		u->sta &= (uint16_t)~TTY_STA_UTXISEL0;
		tty_pin(a, false);
		return false;
	}

	u->sta |= TTY_STA_UTXISEL0;
	*again = (u->sta & TTY_STA_TRMT) != 0;

	return true;
}

/* a port's interrupts, and what the code outside them needs of its enable bits */
#define TTY_PORT(n)                                                                  \
	void __attribute__((interrupt, no_auto_psv)) _U##n##RXInterrupt(void)        \
	{                                                                            \
		_U##n##RXIF = 0;                                                     \
		tty_receive(tty_ports[n - 1], (tty_uart_t *)&U##n##MODE);            \
	}                                                                            \
                                                                                     \
	void __attribute__((interrupt, no_auto_psv)) _U##n##TXInterrupt(void)        \
	{                                                                            \
		bool again = false;                                                  \
                                                                                     \
		_U##n##TXIF = 0;                                                     \
                                                                                     \
		if (!tty_transmit(tty_ports[n - 1], (tty_uart_t *)&U##n##MODE, &again)) { \
			_U##n##TXIE = 0;                                             \
		}                                                                    \
                                                                                     \
		if (again) {                                                         \
			_U##n##TXIF = 1;                                             \
		}                                                                    \
	}

#define TTY_TX_ON(n)                 \
	case n:                      \
		_U##n##TXIF = on;    \
		_U##n##TXIE = on;    \
		break

#define TTY_RX_ON(n)                 \
	case n:                      \
		_U##n##RXIF = 0;     \
		_U##n##RXIE = 1;     \
		break

TTY_PORT(1)
#if TTY_PORTS > 1
TTY_PORT(2)
#endif
#if TTY_PORTS > 2
TTY_PORT(3)
#endif
#if TTY_PORTS > 3
TTY_PORT(4)
#endif
#if TTY_PORTS > 4
TTY_PORT(5)
#endif
#if TTY_PORTS > 5
TTY_PORT(6)
#endif

/* the transmit interrupt; on starts it, as nothing else would when the UART is idle */
static void tty_tx_interrupt(const tty_args_t *a, bool on)
{
	switch (a->port) {
		TTY_TX_ON(1);
#if TTY_PORTS > 1
		TTY_TX_ON(2);
#endif
#if TTY_PORTS > 2
		TTY_TX_ON(3);
#endif
#if TTY_PORTS > 3
		TTY_TX_ON(4);
#endif
#if TTY_PORTS > 4
		TTY_TX_ON(5);
#endif
#if TTY_PORTS > 5
		TTY_TX_ON(6);
#endif
	}
}

/* the interrupt is off, or would not touch these: the next byte is on its way */
static void tty_start(tty_args_t *a, tty_uart_t *u)
{
	tty_tx_interrupt(a, false);
	u->sta &= (uint16_t)~TTY_STA_UTXISEL0;
	tty_pin(a, true);
}

static int tty_write(void *args, void *buffer, unsigned int length)
{
	tty_args_t *a = args;
	tty_uart_t *u = TTY_UART(a->port);
	const uint8_t *data = buffer;

	/* nothing to send: a link that gets no answer tries the wires the other way round */
	if (length == 0) {
		if (a->swap && a->mode != TTY_MODE_STANDARD) {
			a->mode = (a->mode & 1u) ? a->mode + 1 : a->mode - 1;
		}

		return 0;
	}

	tty_start(a, u);

	for (unsigned int i = 0; i < length; i++) {
		uint8_t tail = a->tx_tail;
		uint8_t next = (uint8_t)(tail + 1u);

		next = (next == a->tx_size) ? 0u : next;

		/* more than the queue holds: this much has to wait */
		if (next == a->tx_head) {
			tty_tx_interrupt(a, true);

			while (next == a->tx_head) {
			}
		}

		a->tx[tail] = data[i];
		a->tx_tail = next;
	}

	tty_start(a, u);
	tty_tx_interrupt(a, true);

	return (int)length;
}

static int tty_read(void *args, void *buffer, unsigned int length)
{
	tty_args_t *a = args;
	uint8_t *data = buffer;
	unsigned int n = 0;

	while (n < length && a->rx_head != a->rx_tail) {
		uint8_t head = a->rx_head;

		data[n++] = a->rx[head++];
		a->rx_head = (head == a->rx_size) ? 0u : head;
	}

	return (int)n;
}

static void tty_baud(tty_args_t *a, uint32_t baud)
{
	/* BRGH = 1: 4 clocks a bit, of the instruction clock, rounded */
	TTY_UART(a->port)->brg = (uint16_t)((a->clock + 4u * baud) / (8u * baud) - 1u);
	a->baud = baud;
}

static const char *const tty_modes[] = {"STD", "SPX1", "SPX2", "DPX1", "DPX2"};

static int tty_execute(void *args, int argc, char *argv[])
{
	tty_args_t *a = args;

	if (argc == 2 && strcmp(argv[1], "-stat") == 0) {
		printf("%lu %s lost %u\n", (unsigned long)a->baud, tty_modes[a->mode], a->lost);
		return 0;
	}

	if (argc == 3 && strcmp(argv[1], "-b") == 0) {
		long baud = atol(argv[2]);

		if (baud < 300 || (a->clock + 4u * (uint32_t)baud) / (8u * (uint32_t)baud) > 65536ul) {
			printf("invalid config\n");
			return -1;
		}

		tty_baud(a, (uint32_t)baud);

		return 0;
	}

	if (argc == 3 && strcmp(argv[1], "-m") == 0) {
		for (unsigned int m = 0; m < sizeof(tty_modes) / sizeof(tty_modes[0]); m++) {
			if (strcmp(argv[2], tty_modes[m]) == 0) {
				a->mode = (tty_mode_t)m;
				tty_pin(a, m >= TTY_MODE_RS_DPX1);
				return 0;
			}
		}
	}

	printf("usage: -b BAUD, -m STD/SPX1/SPX2/DPX1/DPX2, -stat\n");

	return -1;
}

static const vfs_handler_t tty_node = {
	.read_handler = tty_read,
	.write_handler = tty_write,
	.execute_handler = tty_execute,
};

int tty_register_node(char *name, tty_args_t *a)
{
	if (name == NULL || a == NULL || a->port < 1 || a->port > TTY_PORTS) {
		return -1;
	}

	tty_uart_t *u = TTY_UART(a->port);

	tty_ports[a->port - 1] = a;

	u->mode = TTY_MODE_BRGH; /* 8N1, stopped */
	u->sta = 0;

	if (a->baud != 0) {
		tty_baud(a, a->baud);
	}

	tty_pin(a, a->mode >= TTY_MODE_RS_DPX1);

	u->mode |= TTY_MODE_UARTEN;
	u->sta |= TTY_STA_UTXEN;

	switch (a->port) {
		TTY_RX_ON(1);
#if TTY_PORTS > 1
		TTY_RX_ON(2);
#endif
#if TTY_PORTS > 2
		TTY_RX_ON(3);
#endif
#if TTY_PORTS > 3
		TTY_RX_ON(4);
#endif
#if TTY_PORTS > 4
		TTY_RX_ON(5);
#endif
#if TTY_PORTS > 5
		TTY_RX_ON(6);
#endif
	}

	return (vfs_node_create(name, false, a, &tty_node) < 0) ? -1 : 0;
}
