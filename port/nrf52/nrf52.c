/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * LETS on the nRF52: see nrf52.h. Registers through the MDK (nrf.h), no nrfx drivers.
 */
#include "nrf52.h"

#include "../../vfs/vfs.h"

#include <nrf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- clock ---------------------------------------------------------------------------- */

static volatile uint32_t overflows; /* TIMER1 wraps every 71.6 minutes */
static uint32_t reset_reason;

void TIMER1_IRQHandler(void)
{
	if (NRF_TIMER1->EVENTS_COMPARE[0]) {
		NRF_TIMER1->EVENTS_COMPARE[0] = 0;
		(void)NRF_TIMER1->EVENTS_COMPARE[0];
		overflows++;
	}
}

void nrf52_initialize(void)
{
	reset_reason = NRF_POWER->RESETREAS;
	NRF_POWER->RESETREAS = reset_reason;

	/* the crystal: USB and the radios need the 32 MHz HFXO */
	NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
	NRF_CLOCK->TASKS_HFCLKSTART = 1;
	while (NRF_CLOCK->EVENTS_HFCLKSTARTED == 0) {
	}

	/* the DC/DC regulator, as the board's design has it (inductors fitted on the DWM3001C) */
	NRF_POWER->DCDCEN = 1;

	/* TIMER1: 1 MHz, 32 bits; COMPARE[0] at 0 counts the wraps */
	NRF_TIMER1->TASKS_STOP = 1;
	NRF_TIMER1->TASKS_CLEAR = 1;
	NRF_TIMER1->MODE = TIMER_MODE_MODE_Timer;
	NRF_TIMER1->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
	NRF_TIMER1->PRESCALER = 4; /* 16 MHz / 2^4 */
	NRF_TIMER1->CC[0] = 0;
	NRF_TIMER1->EVENTS_COMPARE[0] = 0;
	NRF_TIMER1->INTENSET = TIMER_INTENSET_COMPARE0_Msk;
	NVIC_SetPriority(TIMER1_IRQn, 1);
	NVIC_EnableIRQ(TIMER1_IRQn);
	NRF_TIMER1->TASKS_START = 1;
}

static uint32_t timer_now(void)
{
	NRF_TIMER1->TASKS_CAPTURE[1] = 1;
	return NRF_TIMER1->CC[1];
}

uint64_t nrf52_time_us(void)
{
	uint32_t high, low;

	/* a wrap between the two reads: read again (as the PIC's TMR2 with its overflow count) */
	do {
		high = overflows;
		low = timer_now();
	} while (high != overflows || (low < 16u && NRF_TIMER1->EVENTS_COMPARE[0]));

	return ((uint64_t)high << 32) | low;
}

void nrf52_delay_us(uint32_t us)
{
	uint32_t start = timer_now();

	while (timer_now() - start < us) {
	}
}

void nrf52_reset(void)
{
	NVIC_SystemReset();
}

uint32_t nrf52_reset_reason(void)
{
	return reset_reason;
}

/* ---- GPIO ----------------------------------------------------------------------------- */

static NRF_GPIO_Type *gpio_port(uint8_t pin)
{
#if defined(NRF_P1)
	return (pin >= 32u) ? NRF_P1 : NRF_P0;
#else
	return NRF_P0;
#endif
}

void nrf52_gpio_output(uint8_t pin, bool high)
{
	NRF_GPIO_Type *p = gpio_port(pin);

	nrf52_gpio_set(pin, high);
	p->PIN_CNF[pin & 31u] = GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos;
}

void nrf52_gpio_input(uint8_t pin, bool pull_up)
{
	gpio_port(pin)->PIN_CNF[pin & 31u] =
		(pull_up ? GPIO_PIN_CNF_PULL_Pullup : GPIO_PIN_CNF_PULL_Disabled) << GPIO_PIN_CNF_PULL_Pos;
}

void nrf52_gpio_set(uint8_t pin, bool high)
{
	NRF_GPIO_Type *p = gpio_port(pin);

	if (high) {
		p->OUTSET = 1ul << (pin & 31u);
	} else {
		p->OUTCLR = 1ul << (pin & 31u);
	}
}

bool nrf52_gpio_get(uint8_t pin)
{
	return (gpio_port(pin)->IN >> (pin & 31u)) & 1u;
}

/* ---- UART0 as a tty ------------------------------------------------------------------- */

static nrf52_tty_t *uart0_tty;

static uint32_t baud_value(uint32_t baud)
{
	switch (baud) {
	case 9600: return UART_BAUDRATE_BAUDRATE_Baud9600;
	case 19200: return UART_BAUDRATE_BAUDRATE_Baud19200;
	case 38400: return UART_BAUDRATE_BAUDRATE_Baud38400;
	case 57600: return UART_BAUDRATE_BAUDRATE_Baud57600;
	case 115200: return UART_BAUDRATE_BAUDRATE_Baud115200;
	case 230400: return UART_BAUDRATE_BAUDRATE_Baud230400;
	case 460800: return UART_BAUDRATE_BAUDRATE_Baud460800;
	case 921600: return UART_BAUDRATE_BAUDRATE_Baud921600;
	case 1000000: return UART_BAUDRATE_BAUDRATE_Baud1M;
	default: return 0;
	}
}

/* the next byte out, from the interrupt or a write: TXDRDY of the last one has come */
static void tx_next(nrf52_tty_t *t)
{
	if (t->tx_head == t->tx_tail) {
		t->tx_busy = false;
		return;
	}

	uint16_t head = t->tx_head;

	t->tx_busy = true;
	NRF_UART0->TXD = t->tx[head++];
	t->tx_head = (head == t->tx_size) ? 0u : head;
	t->out++;
}

void UARTE0_UART0_IRQHandler(void)
{
	nrf52_tty_t *t = uart0_tty;

	if (t == NULL) {
		return;
	}

	while (NRF_UART0->EVENTS_RXDRDY) {
		NRF_UART0->EVENTS_RXDRDY = 0;

		uint8_t b = (uint8_t)NRF_UART0->RXD;
		uint16_t tail = t->rx_tail;
		uint16_t next = (uint16_t)(tail + 1u);

		next = (next == t->rx_size) ? 0u : next;
		if (next == t->rx_head) {
			t->lost++;
		} else {
			t->rx[tail] = b;
			t->rx_tail = next;
			t->in++;
		}
	}

	if (NRF_UART0->EVENTS_ERROR) {
		NRF_UART0->EVENTS_ERROR = 0;
		NRF_UART0->ERRORSRC = NRF_UART0->ERRORSRC;
		t->lost++;
	}

	if (NRF_UART0->EVENTS_TXDRDY) {
		NRF_UART0->EVENTS_TXDRDY = 0;
		tx_next(t);
	}
}

static void tty_kick(nrf52_tty_t *t)
{
	NVIC_DisableIRQ(UARTE0_UART0_IRQn);
	if (!t->tx_busy) {
		tx_next(t);
	}
	NVIC_EnableIRQ(UARTE0_UART0_IRQn);
}

void nrf52_tty_put(nrf52_tty_t *t, const void *bytes, unsigned int length)
{
	const uint8_t *data = bytes;

	for (unsigned int i = 0; i < length; i++) {
		uint16_t tail = t->tx_tail;
		uint16_t next = (uint16_t)(tail + 1u);

		next = (next == t->tx_size) ? 0u : next;

		/* more than the queue holds: this much has to wait */
		while (next == t->tx_head) {
			tty_kick(t);
		}

		t->tx[tail] = data[i];
		t->tx_tail = next;
	}

	tty_kick(t);
}

static int tty_write(void *args, void *buffer, unsigned int length)
{
	nrf52_tty_put(args, buffer, length);
	return (int)length;
}

static int tty_read(void *args, void *buffer, unsigned int length)
{
	nrf52_tty_t *t = args;
	uint8_t *data = buffer;
	unsigned int n = 0;

	while (n < length && t->rx_head != t->rx_tail) {
		uint16_t head = t->rx_head;

		data[n++] = t->rx[head++];
		t->rx_head = (head == t->rx_size) ? 0u : head;
	}

	return (int)n;
}

static int tty_execute(void *args, int argc, char *argv[])
{
	nrf52_tty_t *t = args;

	if (argc == 2 && strcmp(argv[1], "-stat") == 0) {
		printf("%lu lost %lu in %lu out %lu\n", (unsigned long)t->baud, (unsigned long)t->lost,
		       (unsigned long)t->in, (unsigned long)t->out);
		return 0;
	}

	if (argc == 3 && strcmp(argv[1], "-b") == 0) {
		uint32_t v = baud_value((uint32_t)atol(argv[2]));

		if (v == 0) {
			printf("invalid config\n");
			return -1;
		}

		t->baud = (uint32_t)atol(argv[2]);
		NRF_UART0->BAUDRATE = v;
		return 0;
	}

	printf("usage: -b BAUD (9600 .. 1000000), -stat\n");

	return -1;
}

static const vfs_handler_t tty_node = {
	.read_handler = tty_read,
	.write_handler = tty_write,
	.execute_handler = tty_execute,
};

int nrf52_tty_register(const char *name, nrf52_tty_t *t)
{
	uint32_t v = baud_value(t->baud);

	if (v == 0 || uart0_tty != NULL) {
		return -1;
	}

	uart0_tty = t;
	t->tx_head = t->tx_tail = t->rx_head = t->rx_tail = 0;

	nrf52_gpio_output(t->tx_pin, true);
	nrf52_gpio_input(t->rx_pin, true);

	NRF_UART0->ENABLE = UART_ENABLE_ENABLE_Disabled;
	NRF_UART0->PSEL.TXD = t->tx_pin;
	NRF_UART0->PSEL.RXD = t->rx_pin;
	NRF_UART0->PSEL.RTS = 0xFFFFFFFFu;
	NRF_UART0->PSEL.CTS = 0xFFFFFFFFu;
	NRF_UART0->CONFIG = 0;
	NRF_UART0->BAUDRATE = v;
	NRF_UART0->EVENTS_RXDRDY = 0;
	NRF_UART0->EVENTS_TXDRDY = 0;
	NRF_UART0->EVENTS_ERROR = 0;
	NRF_UART0->INTENSET = UART_INTENSET_RXDRDY_Msk | UART_INTENSET_TXDRDY_Msk | UART_INTENSET_ERROR_Msk;
	NRF_UART0->ENABLE = UART_ENABLE_ENABLE_Enabled;
	NRF_UART0->TASKS_STARTRX = 1;
	NRF_UART0->TASKS_STARTTX = 1;
	NVIC_SetPriority(UARTE0_UART0_IRQn, 2);
	NVIC_EnableIRQ(UARTE0_UART0_IRQn);

	return vfs_node_new(name, false, t, &tty_node) < 0 ? -1 : 0;
}
