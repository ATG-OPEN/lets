/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * Host test of the nRF52 tty with EasyDMA (NRF52_TTY_RX_PPI set): port/nrf52/nrf52.c built on
 * the PC against tests/nrf52/nrf.h, the registers as plain memory. The test plays the UARTE,
 * the TIMER and the PPI: bytes received go where RXD.PTR points and the count of RXDRDY is the
 * TIMER's capture; a send is ended with ENDTX and TXD.AMOUNT. It checks the driver's side only;
 * the hardware itself is not tried here.
 */

#include "../port/nrf52/nrf52.h"
#include "../vfs/vfs.h"

#include <nrf.h>
#include <stdio.h>
#include <string.h>

NRF_UARTE_Type fake_uarte0;
NRF_TIMER_Type fake_timer2;
NRF_PPI_Type fake_ppi;
NRF_CLOCK_Type fake_clock = {.EVENTS_HFCLKSTARTED = 1};
NRF_POWER_Type fake_power;
NRF_GPIO_Type fake_p0;

static NRF_TIMER_Type timer1;

NRF_TIMER_Type *fake_timer1(void)
{
	timer1.CC[1] += 1; /* a microsecond per look: delays end */
	return &timer1;
}

void UARTE0_UART0_IRQHandler(void);

static const vfs_handler_t *tty_handlers;

int vfs_node_new(const char *name, bool hidden, void *args, const vfs_handler_t *handlers)
{
	tty_handlers = handlers;
	return 0;
}

static int failures;
#define CHECK(c)                                                                     \
	do {                                                                         \
		if (!(c)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			failures++;                                                  \
		}                                                                    \
	} while (0)

static uint8_t tx[64], rx[16];
static nrf52_tty_t tty = {.tx_pin = 6, .rx_pin = 8, .baud = 115200, .tx = tx, .rx = rx,
			  .tx_size = sizeof(tx), .rx_size = sizeof(rx)};

/* the low 32 bits of an address, as the driver writes it to a register */
#define REG(p) ((uint32_t)(uintptr_t)(p))

/* bytes in: EasyDMA writes them on in the queue, from its start again when full; RXDRDY counts */
static unsigned int rx_at;

static void receive(const char *bytes)
{
	for (; *bytes != '\0'; bytes++) {
		rx[rx_at] = (uint8_t)*bytes;
		rx_at = (rx_at + 1u) % fake_uarte0.RXD.MAXCNT;
		fake_timer2.CC[1] = rx_at; /* the capture the driver asks for */
	}
}

/* what the driver sent: each run it starts is taken whole, then ENDTX */
static char sent[256];

static void send_all(void)
{
	for (int runs = 0; fake_uarte0.TASKS_STARTTX && runs < 10; runs++) {
		unsigned int at = fake_uarte0.TXD.PTR - REG(tx);
		unsigned int n = fake_uarte0.TXD.MAXCNT;

		CHECK(at < sizeof(tx) && at + n <= sizeof(tx) && n > 0);
		strncat(sent, (const char *)tx + at, n);
		fake_uarte0.TASKS_STARTTX = 0;
		fake_uarte0.TXD.AMOUNT = n;
		fake_uarte0.EVENTS_ENDTX = 1;
		UARTE0_UART0_IRQHandler();
	}
}

static int tty_read_all(char *buf, unsigned int size)
{
	int n = tty_handlers->read_handler(&tty, buf, size - 1u);

	buf[(n < 0) ? 0 : n] = '\0';
	return n;
}

int main(void)
{
	char buf[64];

	CHECK(nrf52_tty_register("ttyS0", &tty) == 0 && tty_handlers != NULL);

	/* UARTE0, receiving into the whole queue, again from its start when full */
	CHECK(fake_uarte0.ENABLE == UARTE_ENABLE_ENABLE_Enabled);
	CHECK(fake_uarte0.BAUDRATE == UARTE_BAUDRATE_BAUDRATE_Baud115200);
	CHECK(fake_uarte0.RXD.PTR == REG(rx) && fake_uarte0.RXD.MAXCNT == sizeof(rx));
	CHECK(fake_uarte0.SHORTS == UARTE_SHORTS_ENDRX_STARTRX_Msk && fake_uarte0.TASKS_STARTRX == 1);
	CHECK(fake_uarte0.PSEL.TXD == 6 && fake_uarte0.PSEL.RXD == 8);

	/* the TIMER counts RXDRDY through the PPI channel, and clears at the queue's size */
	CHECK(fake_ppi.CH[NRF52_TTY_RX_PPI].EEP == REG(&fake_uarte0.EVENTS_RXDRDY));
	CHECK(fake_ppi.CH[NRF52_TTY_RX_PPI].TEP == REG(&fake_timer2.TASKS_COUNT));
	CHECK(fake_ppi.CHENSET == (1u << NRF52_TTY_RX_PPI));
	CHECK(fake_timer2.CC[0] == sizeof(rx) && fake_timer2.SHORTS == TIMER_SHORTS_COMPARE0_CLEAR_Msk);
	CHECK(fake_timer2.MODE != TIMER_MODE_MODE_Timer && fake_timer2.TASKS_START == 1);

	/* received: read in order, also across the end of the queue */
	CHECK(tty_read_all(buf, sizeof(buf)) == 0);
	receive("hello");
	CHECK(tty_read_all(buf, sizeof(buf)) == 5 && strcmp(buf, "hello") == 0);
	receive("abcdefghijkl");
	CHECK(tty_read_all(buf, 5) == 4 && strcmp(buf, "abcd") == 0);
	CHECK(tty_read_all(buf, sizeof(buf)) == 8 && strcmp(buf, "efghijkl") == 0);
	CHECK(tty.in == 17);

	/* sent: runs up to the end of the queue, the next from its start */
	nrf52_tty_put(&tty, "0123456789012345678901234567890123456789", 40);
	CHECK(fake_uarte0.TXD.PTR == REG(tx) && fake_uarte0.TXD.MAXCNT == 40);
	send_all();
	CHECK(!tty.tx_busy && tty.tx_head == 40 && tty.out == 40);
	nrf52_tty_put(&tty, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmn", 40);
	CHECK(fake_uarte0.TXD.PTR == REG(tx + 40) && fake_uarte0.TXD.MAXCNT == 24);
	send_all();
	CHECK(strcmp(sent, "0123456789012345678901234567890123456789"
			   "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmn") == 0);
	CHECK(!tty.tx_busy && tty.tx_head == 16 && tty.out == 80);

	/* an error of the UARTE counts as lost */
	fake_uarte0.EVENTS_ERROR = 1;
	fake_uarte0.ERRORSRC = 1;
	UARTE0_UART0_IRQHandler();
	CHECK(tty.lost == 1 && fake_uarte0.EVENTS_ERROR == 0);

	/* the baud rate in UARTE's values */
	char *argv[] = {"ttyS0", "-b", "230400"};
	CHECK(tty_handlers->execute_handler(&tty, 3, argv) == 0);
	CHECK(fake_uarte0.BAUDRATE == UARTE_BAUDRATE_BAUDRATE_Baud230400 && tty.baud == 230400);

	printf(failures ? "nrf52: FAILED\n" : "nrf52: ok\n");

	return failures != 0;
}
