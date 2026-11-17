/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * Cooperative scheduler: one table of tasks and one endless loop that calls
 * each task in turn. See task.h for the rules a task follows.
 */

#include "task.h"
#include "alloc.h"

#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>

#if defined(__XC16__)
#include <xc.h>

#define TASK_STACK_PATTERN 0x5AA5u
#define TASK_RESET_FLAGS   0xC2DFu /* in RCON: the flags, not the settings */

extern char _SP_init; /* from the linker: the bottom of the stack */
#endif

static uint32_t task_loop_time = 0; /* duration of the last loop, in us */
static task_t tasks[TASKS_MAX_COUNT];
static int current = -1; /* index of the running task, -1 outside a task */
static task_streams_t task_main = {-1, -1, -1, 0}; /* the streams outside a task */
static uint16_t task_reset; /* RCON at start-up */
static uint8_t task_reason; /* a task_reset_t */

/* the board's reset, in bin/reboot.c or its own */
extern void reboot_reset(void) __attribute__((weak));

#if LETS_WDT
#define TASK_FAULT_MAGIC 0x4C57u

/* kept across a reset: not cleared at start-up */
#if defined(__XC16__)
#define TASK_NOINIT __attribute__((persistent))
#elif defined(__arm__)
#define TASK_NOINIT __attribute__((section(".noinit")))
#else
#define TASK_NOINIT
#endif

static TASK_NOINIT struct {
	uint16_t magic;
	task_fault_t fault;
} task_fault_next; /* written just before the reset */

static task_fault_t task_fault_last; /* what it held at start-up */
static bool task_wdt_late;	     /* a watched task was late: no more kicks */
#endif

/* vfs: a new task holds its streams open, one that stopped lets them go */
extern void vfs_streams_hold(int in, int out) __attribute__((weak));
extern void vfs_streams_close(int in, int out) __attribute__((weak));

/* the clock given to task_initialize() */
static uint64_t (*task_get_us_from_system)(void) = NULL;

/* -------------------------------------------------------------------------- */

static void task_set_name(task_t *t, const char *name)
{
	strncpy(t->info.name, name, TASK_NAME_SIZE - 1);
	t->info.name[TASK_NAME_SIZE - 1] = '\0';
}

/* frees the slot and every heap block the task owns */
static void task_release(int i)
{
	task_streams_t s = tasks[i].streams;

	tasks[i].state = TASK_UNUSED;

	alloc_free_owner(i);

	if (vfs_streams_close != NULL) {
		vfs_streams_close(s.in, s.out);
	}
}

/* -------------------------------------------------------------------------- */

void task_initialize(uint64_t (*handle)(void))
{
	if (handle == NULL) {
		return;
	}

	task_get_us_from_system = handle;

#if defined(__XC16__)
	task_reset = RCON;
#endif
	task_reason = (uint8_t)port_wdt_reset_cause();

#if LETS_WDT
	task_fault_last.kind = 0;

	/* after a power-up the RAM holds anything */
	if (task_fault_next.magic == TASK_FAULT_MAGIC && task_reason != TASK_RESET_POWER_ON &&
	    task_reason != TASK_RESET_BROWN_OUT) {
		task_fault_last = task_fault_next.fault;

		if (task_fault_last.kind == TASK_FAULT_TRAP) {
			task_reason = TASK_RESET_TRAP;
		}
	}

	task_fault_next.magic = 0;
	task_wdt_late = false;
#endif

#if defined(__XC16__)
	RCON &= (uint16_t)~TASK_RESET_FLAGS;

	/* what is above the stack pointer now: task_stack() reads how far it was used */
	for (uint16_t *p = (uint16_t *)WREG15 + 1; p < (uint16_t *)SPLIM; p++) {
		*p = TASK_STACK_PATTERN;
	}
#endif

	for (int i = 0; i < TASKS_MAX_COUNT; i++) {
		task_t *t = &tasks[i];

		t->state = TASK_UNUSED;
	}
}

uint64_t task_get_system_time(void)
{
	if (task_get_us_from_system == NULL) {
		return 0;
	}

	return task_get_us_from_system();
}

task_t *task_get_data(void)
{
	return tasks;
}

int task_get_current(void)
{
	return current;
}

task_streams_t *task_streams(void)
{
	return (current >= 0 && current < (int)TASKS_MAX_COUNT) ? &tasks[current].streams : &task_main;
}

bool task_stopping(void)
{
	return current >= 0 && current < (int)TASKS_MAX_COUNT && tasks[current].state == TASK_STOPPING;
}

void *task_alloc(size_t size)
{
	void *p = malloc(size);

	if (p != NULL && current >= 0 && current < (int)TASKS_MAX_COUNT) {
		alloc_change_owner(p, current);
	}

	return p;
}

int task_count(void)
{
	int count = 0;

	for (int i = 0; i < TASKS_MAX_COUNT; i++) {
		task_t *t = &tasks[i];

		if (t->state == TASK_UNUSED) {
			continue;
		}

		count++;
	}

	return count;
}

/* the first task called @name, and with @arg unless @any */
static int task_stop_match(const char *name, const void *arg, bool any)
{
	if (name == NULL) {
		return -1;
	}

	for (int i = 0; i < TASKS_MAX_COUNT; i++) {
		task_t *t = &tasks[i];

		if (t->state == TASK_UNUSED || t->state == TASK_STOPPING) {
			continue;
		}

		if (strcmp(t->info.name, name) != 0 || (!any && t->arg != arg)) {
			continue;
		}

		/* it is running: its memory and streams are in use until it returns */
		if (i == current) {
			t->state = TASK_STOPPING;
		} else {
			task_release(i);
		}

		return 0;
	}

	return -1;
}

int task_stop_byname(char *name)
{
	return task_stop_match(name, NULL, true);
}

int task_stop_arg(const char *name, const void *arg)
{
	return task_stop_match(name, arg, false);
}

task_t *task_create(const char *name, int (*entry)(void *arg, uint64_t system_time), void *arg)
{
	if (name == NULL || entry == NULL) {
		return NULL;
	}

	for (int i = 0; i < TASKS_MAX_COUNT; i++) {
		task_t *t = &tasks[i];

		if (t->state != TASK_UNUSED) {
			continue;
		}

		t->entry = entry;
		t->arg = arg;
		t->state = TASK_RUNNING;
		t->wake_tick = 0;
		t->streams = *task_streams();
		t->info.worst = 0;
#if LETS_WDT
		t->deadline = 0;
#endif

		task_set_name(t, name);

		if (vfs_streams_hold != NULL) {
			vfs_streams_hold(t->streams.in, t->streams.out);
		}

		/* the task owns its argument: freed when the task stops */
		alloc_change_owner(arg, i);

		return t;
	}

	return NULL;
}

float task_get_loop_time(void)
{
	return (float)task_loop_time / 1000;
}

int task_stack(size_t *peak, size_t *size)
{
#if defined(__XC16__)
	uint16_t *start = (uint16_t *)&_SP_init;
	uint16_t *end = (uint16_t *)SPLIM;
	uint16_t *p = end;

	while (p > start && p[-1] == TASK_STACK_PATTERN) {
		p--;
	}

	*peak = (size_t)((char *)p - (char *)start);
	*size = (size_t)((char *)end - (char *)start);

	return 0;
#else
	(void)peak;
	(void)size;

	return -1;
#endif
}

uint16_t task_reset_cause(void)
{
	return task_reset;
}

task_reset_t task_reset_reason(void)
{
	return (task_reset_t)task_reason;
}

const task_fault_t *task_fault(void)
{
#if LETS_WDT
	return (task_fault_last.kind != 0) ? &task_fault_last : NULL;
#else
	return NULL;
#endif
}

#if LETS_WDT
/* kept for after the reset: what happened, and in which task */
static void task_fault_save(uint16_t kind, uint16_t code, int slot)
{
	task_fault_t *f = &task_fault_next.fault;

	f->kind = kind;
	f->code = code;
	f->task[0] = '\0';

	if (slot >= 0 && slot < (int)TASKS_MAX_COUNT) {
		memcpy(f->task, tasks[slot].info.name, TASK_NAME_SIZE);
	}

	task_fault_next.magic = TASK_FAULT_MAGIC;
}

/* true while every watched task has checked in within its deadline */
static bool task_wdt_check(uint32_t now)
{
	for (int i = 0; i < TASKS_MAX_COUNT; i++) {
		task_t *t = &tasks[i];

		if (t->state == TASK_UNUSED || t->deadline == 0) {
			continue;
		}

		if (now - t->alive > (uint32_t)t->deadline * 1000u) {
			task_fault_save(TASK_FAULT_LATE, 0, i);
			task_wdt_late = true; /* for good: the reset is coming */

			return false;
		}
	}

	return true;
}
#endif

int task_watch(task_t *t, uint16_t deadline_ms)
{
#if LETS_WDT
	if (t == NULL && current >= 0 && current < (int)TASKS_MAX_COUNT) {
		t = &tasks[current];
	}

	if (t == NULL || t->state == TASK_UNUSED) {
		return -1;
	}

	t->alive = (uint32_t)task_get_system_time();
	t->deadline = deadline_ms;
#else
	(void)t;
	(void)deadline_ms;
#endif

	return 0;
}

void task_alive(void)
{
#if LETS_WDT
	if (current >= 0 && current < (int)TASKS_MAX_COUNT) {
		tasks[current].alive = (uint32_t)task_get_system_time();
	}
#endif
}

void task_trap(uint16_t code)
{
#if LETS_WDT
	task_fault_save(TASK_FAULT_TRAP, code, current);
#else
	(void)code;
#endif

#if defined(__XC16__)
	__asm__ volatile("reset");
#else
	if (reboot_reset != NULL) {
		reboot_reset();
	}
#endif

	for (;;) {
	}
}

#if defined(__XC16__)
/*
 * PIC24 / dsPIC33: the watchdog's period is in the config bits (FWPSA, WDTPS), and with
 * FWDTEN = ON_SWDTEN the kernel turns it on. A port may define its own.
 */
task_reset_t __attribute__((weak)) port_wdt_reset_cause(void)
{
	uint16_t r = task_reset;

	if (r & 0xC000u) { /* TRAPR, IOPUWR (illegal opcode) */
		return TASK_RESET_TRAP;
	}

	return (r & 0x0010u)   ? TASK_RESET_WATCHDOG
	       : (r & 0x0040u) ? TASK_RESET_SOFTWARE
	       : (r & 0x0080u) ? TASK_RESET_PIN
	       : (r & 0x0001u) ? TASK_RESET_POWER_ON
	       : (r & 0x0002u) ? TASK_RESET_BROWN_OUT
			       : TASK_RESET_OTHER;
}

#if LETS_WDT
void __attribute__((weak)) port_wdt_start(uint16_t ms)
{
	(void)ms;
	RCONbits.SWDTEN = 1;
}

void __attribute__((weak)) port_wdt_kick(void)
{
	__asm__ volatile("clrwdt");
}

/* MCC's traps.c ends every trap here (its version is weak): record it and start again */
#if !defined(LETS_WDT_NO_TRAPS)
void TRAPS_halt_on_error(uint16_t code)
{
	task_trap(code);
}
#endif
#endif
#else
/* a port without them (a PC): no watchdog, no reason */
task_reset_t __attribute__((weak)) port_wdt_reset_cause(void)
{
	return TASK_RESET_OTHER;
}

#if LETS_WDT
void __attribute__((weak)) port_wdt_start(uint16_t ms)
{
	(void)ms;
}

void __attribute__((weak)) port_wdt_kick(void)
{
}
#endif
#endif

void tasks_start(void)
{
	uint64_t now = task_get_system_time();
	uint32_t loop_start = (uint32_t)now;

#if LETS_WDT
	port_wdt_start(LETS_WDT_MS);
#endif

	for (;;) {
		/*
		 * Read the clock on every pass: a pass where every task sleeps runs
		 * none of them, and a stale time would never wake one again.
		 */
		now = task_get_system_time();

		/* time of the previous loop, for task_get_loop_time() */
		task_loop_time = (uint32_t)now - loop_start;
		loop_start = (uint32_t)now;

		/*
		 * A task that never returns stops the kicks; with LETS_WDT a watched task
		 * that is late does too. Either way the watchdog resets the chip.
		 */
#if LETS_WDT
		if (!task_wdt_late && task_wdt_check((uint32_t)now)) {
			port_wdt_kick();
		}
#elif defined(__XC16__)
		__asm__ volatile("clrwdt");
#endif

		for (current = 0; current < TASKS_MAX_COUNT; current++) {
			task_t *t = &tasks[current];

			if (t->state == TASK_UNUSED) {
				continue;
			}

			uint32_t start = (uint32_t)now;

			/* a sleeping task wakes once its time has come; the time wraps */
			if (t->state == TASK_SLEEPING) {
				if ((int32_t)(start - t->wake_tick) < 0) {
					continue;
				}

				t->state = TASK_RUNNING;
			}

			t->info.delay = start - t->info.last_call_time;

			int r = t->entry(t->arg, now);

			now = task_get_system_time();

			/* negative result: the task is done; it may have stopped itself */
			if (r < 0 || t->state == TASK_STOPPING) {
				task_release(current);

				continue;
			}

			uint32_t end = (uint32_t)now;

			t->info.duration = end - start;
			t->info.last_call_time = start;

			if (t->info.duration > t->info.worst) {
				t->info.worst = t->info.duration;
			}

			/*
			 * Positive result: sleep that many ms, counted from the last
			 * wake, so a periodic task keeps its period whatever it took.
			 * One that fell behind starts over from now.
			 */
			if (r > 0) {
				uint32_t wait = (uint32_t)r * 1000u;
				uint32_t wake = t->wake_tick + wait;

				if ((int32_t)(end - wake) >= 0 || (int32_t)(wake - end) > (int32_t)wait) {
					wake = end + wait;
				}

				t->state = TASK_SLEEPING;
				t->wake_tick = wake;
			}
		}
	}
}
