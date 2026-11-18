/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * Watchdog host test (LETS_WDT=1), on a fake clock and a fake watchdog: the port hooks are
 * defined here. A reset is a longjmp() back to main(), which starts the kernel again, as a
 * chip does; the fault record survives it as it survives a reset in no-init RAM.
 *
 *   boot 0  a slow boot (3 s before tasks_start()); every task is watched by default: a busy
 *           one, ones asleep longer than the default deadline (not late: asleep), ones that
 *           check in (task_watch()), one that stops being watched; then "bad" stops checking
 *           in: the kicks stop and the fake watchdog fires
 *   boot 1  the reset says watchdog, late: bad; a task traps (task_trap())
 *   boot 2  the reset says trap 2, in: crasher; "hog" takes 1.5 s in one call: the others,
 *           ready, wait longer than LETS_WDT_DEFAULT_MS
 *   boot 3  watchdog, late: victim, by: hog; "spinner" never returns
 *   boot 4  watchdog, hang: spinner; the next reset has no fault
 *   boot 5  "writer" takes 5 s in one call but calls task_busy() every 50 ms (a flash
 *           rewrite): no reset, and the others it kept waiting are not late; then it calls
 *           task_busy() for ever
 *   boot 6  watchdog, hang: writer (after LETS_WDT_BUSY_MS)
 *   boot 7  overhead: one pass with 15 tasks that check in against 15 not watched
 */
#include "../kernel/task.h"
#include "../kernel/alloc.h"
#include "../vfs/vfs.h"

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

ALLOC_INIT_HEAP(4096);

static int failures;

#define CHECK(c)                                                                                   \
	do {                                                                                       \
		if (!(c)) {                                                                        \
			fprintf(stderr, "wdt: FAILED line %d: %s\n", __LINE__, #c);                \
			failures++;                                                                \
		}                                                                                  \
	} while (0)

/* ---- the fake chip -------------------------------------------------------------------- */

static jmp_buf chip;
static uint64_t fake_us;	/* the clock: 10 us more on every read */
static uint64_t wdt_ms;		/* 0: not started */
static uint64_t last_kick;
static unsigned long kicks;
static task_reset_t cause = TASK_RESET_POWER_ON;

static __attribute__((noreturn)) void reset(task_reset_t why, int boot)
{
	cause = why;
	wdt_ms = 0;
	longjmp(chip, boot);
}

static int boot; /* the boot this is, from main() */

static uint64_t now_us(void)
{
	fake_us += 10;

	if (wdt_ms != 0 && fake_us - last_kick > wdt_ms * 1000u) {
		reset(TASK_RESET_WATCHDOG, boot + 1);
	}

	return fake_us;
}

void port_wdt_start(uint16_t ms)
{
	wdt_ms = ms;
	last_kick = fake_us;
}

void port_wdt_kick(void)
{
	last_kick = fake_us;
	kicks++;
}

task_reset_t port_wdt_reset_cause(uint16_t *raw)
{
	*raw = (uint16_t)cause;
	return cause;
}

/* task_trap() resets through here */
void port_wdt_reset_now(void)
{
	reset(TASK_RESET_SOFTWARE, boot + 1);
}

/* ---- boot 0 --------------------------------------------------------------------------- */

static int idle(void *arg, uint64_t now)
{
	return 30000; /* asleep for longer than any deadline */
}

static int nap(void *arg, uint64_t now)
{
	return 1500; /* asleep longer than LETS_WDT_DEFAULT_MS, again and again */
}

static int busy(void *arg, uint64_t now)
{
	return 0; /* never checks in: the default watch, it just runs */
}

static int good(void *arg, uint64_t now)
{
	task_alive();
	return 50;
}

static int bad(void *arg, uint64_t now)
{
	if (now < 5000000u) {
		task_alive(); /* until 5 s: then it is stuck, though it still returns */
	}

	return 20;
}

/* watched, then not: stops checking in once it is not */
static int quits(void *arg, uint64_t now)
{
	if (now < 1000000u) {
		task_alive();
	} else {
		task_watch(NULL, 0);
	}

	return 10;
}

static int check0(void *arg, uint64_t now)
{
	if (now >= 4900000u) {
		/* every watched task on time so far: a kick on every pass */
		CHECK(kicks > 1000);
		CHECK(fake_us - last_kick < 1000u);
		return -1;
	}

	return 100;
}

/* ---- boot 1 --------------------------------------------------------------------------- */

static int crasher(void *arg, uint64_t now)
{
	task_trap(2);
}

/* ---- boot 2, 3 ------------------------------------------------------------------------ */

static int victim(void *arg, uint64_t now)
{
	return 10;
}

static int hog(void *arg, uint64_t now)
{
	if (now > 1000000u) {
		while (task_get_system_time() < now + 1500000u) {
			/* 1.5 s in one call: less than the watchdog, more than the others may wait */
		}
	}

	return 10;
}

static int spinner(void *arg, uint64_t now)
{
	if (now > 1000000u) {
		for (;;) {
			(void)task_get_system_time(); /* never returns */
		}
	}

	return 0;
}

/* ---- boot 5 --------------------------------------------------------------------------- */

static int wrote; /* 1: the 5 s call is over, 2: the others ran after it, no reset */

static void busy_for(uint64_t us)
{
	uint64_t from = task_get_system_time();
	uint64_t step = from;

	while (task_get_system_time() - from < us) {
		if (task_get_system_time() - step >= 50000u) {
			step = task_get_system_time();
			task_busy(); /* a page done */
		}
	}
}

static int writer(void *arg, uint64_t now)
{
	if (now > 1000000u && wrote == 0) {
		busy_for(5000000u); /* longer than the watchdog, and than any deadline */
		wrote = 1;
	} else if (wrote == 2) {
		busy_for(UINT64_MAX); /* never done: a hang, though it keeps calling task_busy() */
	}

	return 10;
}

static int after(void *arg, uint64_t now)
{
	if (wrote == 1 && now > 7000000u) {
		wrote = 2;
	}

	task_alive();
	return 10;
}

/* ---- boot 7 --------------------------------------------------------------------------- */

static int spin(void *arg, uint64_t now)
{
	task_alive();
	return 0;
}

static double real_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

#define PASSES 20000ul

static task_t *spinners[TASKS_MAX_COUNT];
static double pass_s[2];

static int measure(void *arg, uint64_t now)
{
	static int round;
	static unsigned long from;
	static double t0;

	if (kicks - from < PASSES && round > 0) {
		return 0;
	}

	if (round > 0) {
		pass_s[round - 1] = (real_s() - t0) / (double)(kicks - from);
	}

	if (round == 2) {
		fprintf(stderr, "wdt: a pass with 15 tasks: %.2f us watched, %.2f us not\n",
			pass_s[0] * 1e6, pass_s[1] * 1e6);
		CHECK(pass_s[0] < 3 * pass_s[1]);
		fprintf(stderr, failures ? "wdt: FAILED\n" : "wdt: ok\n");
		exit(failures ? 1 : 0);
	}

	/* round 0: watch them all; round 1: none */
	for (int i = 0; spinners[i] != NULL; i++) {
		task_watch(spinners[i], (round == 0) ? 100 : 0);
	}

	round++;
	from = kicks;
	t0 = real_s();

	return 0;
}

/* ---- the boots ------------------------------------------------------------------------ */

static void stuck(int sig)
{
	fprintf(stderr, "wdt: FAILED, stuck in boot %d\n", boot);
	_exit(1);
}

int main(void)
{
	signal(SIGALRM, stuck);
	alarm(20);

	vfs_initialize();

	boot = setjmp(chip);
	task_initialize(now_us);

	const task_fault_t *f = task_fault();

	switch (boot) {
	case 0:
		CHECK(task_reset_reason() == TASK_RESET_POWER_ON && f == NULL);
		task_create("idle", idle, NULL);
		task_create("nap", nap, NULL);
		task_create("busy", busy, NULL);
		task_watch(task_create("good", good, NULL), 100);
		task_watch(task_create("bad", bad, NULL), 200);
		task_watch(task_create("quits", quits, NULL), 100);
		task_create("check0", check0, NULL);
		CHECK(task_watch(NULL, 100) == -1); /* outside a task */
		fake_us += 3000000u; /* a slow rc: not late, they wait from tasks_start() */
		break;

	case 1:
		/* bad's last check-in was just before 5 s: the kicks stopped 200 ms later */
		fprintf(stderr, "wdt: last kick at %.3f s, reset at %.3f s\n", last_kick / 1e6,
			fake_us / 1e6);
		CHECK(last_kick >= 5000000u && last_kick <= 5300000u);
		CHECK(fake_us - last_kick > LETS_WDT_MS * 1000u);
		CHECK(task_reset_reason() == TASK_RESET_WATCHDOG);
		CHECK(f != NULL && f->kind == TASK_FAULT_LATE && strcmp(f->task, "bad") == 0 &&
		      f->by[0] == '\0');
		task_create("crasher", crasher, NULL);
		break;

	case 2:
		CHECK(task_reset_reason() == TASK_RESET_TRAP);
		CHECK(f != NULL && f->kind == TASK_FAULT_TRAP && f->code == 2 &&
		      strcmp(f->task, "crasher") == 0);
		fake_us = 0;
		task_create("victim", victim, NULL);
		task_create("idle", idle, NULL);
		task_create("hog", hog, NULL);
		break;

	case 3:
		fprintf(stderr, "wdt: late at %.3f s, reset at %.3f s\n", last_kick / 1e6, fake_us / 1e6);
		CHECK(task_reset_reason() == TASK_RESET_WATCHDOG);
		CHECK(f != NULL && f->kind == TASK_FAULT_LATE && strcmp(f->task, "victim") == 0 &&
		      strcmp(f->by, "hog") == 0);
		fake_us = 0;
		task_create("nap", nap, NULL);
		task_create("spinner", spinner, NULL);
		task_create("busy", busy, NULL);
		break;

	case 4:
		CHECK(task_reset_reason() == TASK_RESET_WATCHDOG);
		CHECK(f != NULL && f->kind == TASK_FAULT_HANG && strcmp(f->task, "spinner") == 0 &&
		      f->by[0] == '\0');
		reset(TASK_RESET_PIN, 5);
		break;

	case 5:
		CHECK(task_reset_reason() == TASK_RESET_PIN && f == NULL);
		fake_us = 0;
		wrote = 0;
		task_create("victim", victim, NULL);
		task_create("nap", nap, NULL);
		task_watch(task_create("after", after, NULL), 100);
		task_create("writer", writer, NULL);
		break;

	case 6:
		fprintf(stderr, "wdt: busy writer: hang at %.3f s\n", fake_us / 1e6);
		CHECK(wrote == 2);
		CHECK(task_reset_reason() == TASK_RESET_WATCHDOG);
		CHECK(f != NULL && f->kind == TASK_FAULT_HANG && strcmp(f->task, "writer") == 0);
		CHECK(fake_us > 7000000u + LETS_WDT_BUSY_MS * 1000ull);
		reset(TASK_RESET_PIN, 7);
		break;

	default:
		CHECK(task_reset_reason() == TASK_RESET_PIN && f == NULL);

		for (int i = 0; i < TASKS_MAX_COUNT - 1; i++) {
			spinners[i] = task_create("spin", spin, NULL);
		}

		task_create("measure", measure, NULL);
		break;
	}

	tasks_start();
	return 1;
}
