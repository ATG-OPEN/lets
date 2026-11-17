/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * Host test of the PIC24 tty (opt/tty.c) against tests/pic24/xc.h. Two ports keep their RS485
 * direction pins in one LATF. A second thread plays the interrupts: it runs port 2's transmit
 * interrupt, which drops port 2's pin after the last bit, while this thread changes port 1's
 * pin as a task does. An interrupt between a task's read and write of LATF would put port 2's
 * pin back on (a transmitter left on); the test counts such lost updates and wants none.
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../opt/tty.c"

volatile uint16_t fake_u1[5], fake_u2[5];
volatile uint16_t LATF;
volatile unsigned int _U1TXIF, _U1TXIE, _U1RXIF, _U1RXIE;
volatile unsigned int _U2TXIF, _U2TXIE, _U2RXIF, _U2RXIE;

static pthread_mutex_t cpu;
static _Thread_local unsigned int cpu_ipl;

unsigned int fake_ipl_raise(unsigned int ipl)
{
	unsigned int was = cpu_ipl;

	pthread_mutex_lock(&cpu);
	cpu_ipl = ipl;
	return was;
}

void fake_ipl_restore(unsigned int ipl)
{
	cpu_ipl = ipl;
	pthread_mutex_unlock(&cpu);
}

int vfs_node_create(const char *name, bool hidden, void *args, const vfs_handler_t *handlers)
{
	(void)name;
	(void)hidden;
	(void)args;
	(void)handlers;
	return 0;
}

static int failures;

#define CHECK(c)                                                         \
	do {                                                             \
		if (!(c)) {                                              \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			failures++;                                      \
		}                                                        \
	} while (0)

#define PIN1 0x0001u
#define PIN2 0x0004u

static uint8_t tx1[16], rx1[16], tx2[16], rx2[16];

static tty_args_t s1 = {.port = 1, .clock = 32000000, .tx = tx1, .tx_size = sizeof(tx1),
			.rx = rx1, .rx_size = sizeof(rx1), .pin = &LATF, .pin_mask = PIN1,
			.mode = TTY_MODE_RS_SPX1};
static tty_args_t s2 = {.port = 2, .clock = 32000000, .tx = tx2, .tx_size = sizeof(tx2),
			.rx = rx2, .rx_size = sizeof(rx2), .pin = &LATF, .pin_mask = PIN2,
			.mode = TTY_MODE_RS_SPX1};

/* a write goes out by the interrupt; the pin is on for it and off after the last bit */
static void test_write(void)
{
	fake_u1[1] = TTY_STA_TRMT;
	CHECK(tty_write(&s1, (char[]){"abc"}, 3) == 3);
	CHECK(LATF & PIN1);
	CHECK(_U1TXIE && _U1TXIF);

	_U1TXInterrupt(); /* the FIFO takes it all (UTXBF never set here) */
	CHECK(fake_u1[2] == 'c');
	CHECK(LATF & PIN1);

	_U1TXInterrupt(); /* nothing written now and TRMT: the last bit has gone */
	CHECK(!(LATF & PIN1));
	CHECK(!_U1TXIE);
}

static volatile bool stop;
static unsigned long lost;

/* port 2: a task turns its pin on (atomically), then the interrupt turns it off */
static void *interrupts(void *unused)
{
	(void)unused;

	for (unsigned long i = 0; i < 2000000ul; i++) {
		unsigned int ipl = fake_ipl_raise(7);

		if (LATF & PIN2) {
			lost++;
		}

		LATF |= PIN2;
		fake_ipl_restore(ipl);

		ipl = fake_ipl_raise(7); /* an interrupt runs while the task stands */
		_U2TXInterrupt();
		fake_ipl_restore(ipl);
	}

	stop = true;
	return NULL;
}

static void test_shared_lat(void)
{
	pthread_t t;
	unsigned long turns = 0;

	fake_u2[1] = TTY_STA_TRMT;
	stop = false;
	lost = 0;
	CHECK(pthread_create(&t, NULL, interrupts, NULL) == 0);

	while (!stop) {
		tty_pin(&s1, (turns++ & 1u) != 0);
	}

	pthread_join(t, NULL);
	printf("tty: %lu pin changes against 2000000 interrupts, %lu lost\n", turns, lost);
	CHECK(lost == 0);
}

int main(void)
{
	pthread_mutexattr_t attr;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&cpu, &attr);

	CHECK(tty_register_node("ttyS1", &s1) == 0);
	CHECK(tty_register_node("ttyS2", &s2) == 0);
	CHECK(!(LATF & (PIN1 | PIN2)));

	test_write();
	test_shared_lat();

	printf(failures ? "test_tty: %d FAILED\n" : "test_tty: ok\n", failures);
	return failures ? 1 : 0;
}
