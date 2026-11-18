/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * Host test of the PIC24 tty (port/pic24/tty.c) against tests/pic24/xc.h. Two ports keep their RS485
 * direction pins in one LATF. A second thread plays the interrupts: it runs port 2's transmit
 * interrupt, which drops port 2's pin after the last bit, while this thread changes port 1's
 * pin as a task does. An interrupt between a task's read and write of LATF would put port 2's
 * pin back on (a transmitter left on); the test counts such lost updates and wants none.
 *
 * Then DPX auto direction on a fake clock: a DPX port that reads nothing swaps DPX1 <-> DPX2
 * (and its pin) after 2 to 4 s, never while bytes come; `-a 0`, SPX and STD never swap.
 */

#include <pthread.h>
#include <stdio.h>

#include "../port/pic24/tty.c"

volatile uint16_t fake_u1[5], fake_u2[5];
volatile uint16_t LATF;
volatile unsigned int _U1TXIF, _U1TXIE, _U1RXIF, _U1RXIE;
volatile unsigned int _U2TXIF, _U2TXIE, _U2RXIF, _U2RXIE;

static uint64_t fake_us;

uint64_t task_get_system_time(void)
{
	return fake_us;
}

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

/* reads port 1 every tick (1.024 ms) for @ms ticks; a byte arrives every @every ms (0: none); returns the swaps */
static unsigned int run(unsigned int ms, unsigned int every)
{
	uint16_t before = s1.swaps;
	uint8_t b[8];

	for (unsigned int t = 1; t <= ms; t++) {
		fake_us += 1024u; /* the tty counts in 1.024 ms */

		if (every != 0 && t % every == 0) {
			/* what the receive interrupt does with a byte */
			s1.rx[s1.rx_tail] = 'p';
			s1.rx_tail = (uint8_t)((s1.rx_tail + 1u) % s1.rx_size);
		}

		tty_read(&s1, b, sizeof(b));
	}

	return (unsigned int)(uint16_t)(s1.swaps - before);
}

static void test_dpx(void)
{
	char *dpx1[] = {"ttyS1", "-m", "DPX1"};
	char *spx1[] = {"ttyS1", "-m", "SPX1"};
	char *std[] = {"ttyS1", "-m", "STD"};
	char *fix[] = {"ttyS1", "-a", "0"};
	char *unfix[] = {"ttyS1", "-a", "1"};

	CHECK(tty_execute(&s1, 3, dpx1) == 0);
	CHECK(s1.mode == TTY_MODE_RS_DPX1 && (LATF & PIN1));

	/* nothing heard: the first swap within 2 .. 4 s, and the pin follows at once */
	CHECK(run(1999, 0) == 0);
	CHECK(run(2049, 0) == 1);
	CHECK(s1.mode == TTY_MODE_RS_DPX2 && !(LATF & PIN1));

	/* and on: 60 s of silence swaps every 2 .. 4 s */
	unsigned int n = run(60000, 0);

	CHECK(n >= 15 && n <= 30);

	/* a ping a second (and its silence in between) never swaps */
	CHECK(run(1, 1) == 0); /* the first byte */

	tty_mode_t m = s1.mode;

	CHECK(run(600000, 1000) == 0 && s1.mode == m);

	/* -a 0 keeps it; -a 1 swaps again */
	CHECK(tty_execute(&s1, 3, fix) == 0);
	CHECK(run(30000, 0) == 0);
	CHECK(tty_execute(&s1, 3, unfix) == 0);
	CHECK(run(4100, 0) == 1);

	/* SPX and STD never swap by themselves */
	CHECK(tty_execute(&s1, 3, spx1) == 0);
	CHECK(run(30000, 0) == 0 && s1.mode == TTY_MODE_RS_SPX1);
	CHECK(tty_execute(&s1, 3, std) == 0);
	CHECK(run(30000, 0) == 0 && s1.mode == TTY_MODE_STANDARD);

	/* two ports registered alike do not share a seed */
	CHECK(s1.dpx.state != s2.dpx.state);
	printf("tty: DPX auto direction ok, %u swaps in 60 s of silence\n", n);
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
	test_dpx();

	printf(failures ? "test_tty: %d FAILED\n" : "test_tty: ok\n", failures);
	return failures ? 1 : 0;
}
