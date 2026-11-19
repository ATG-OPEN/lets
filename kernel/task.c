/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * Cooperative scheduler: one table of tasks and one endless loop that calls
 * each task in turn. See task.h for the rules a task follows.
 */

#include "task.h"
#include "alloc.h"
#include "../lib/posix.h" /* VFS_FD_TASK */

#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>

#if LETS_WDT
#define TASK_FAULT_MAGIC 0x4C57u
#define TASK_NONE	 0xFFu /* no slot */

/* kept across a reset: not cleared at start-up */
#if defined(__XC16__)
#define TASK_NOINIT __attribute__((persistent))
#elif defined(__arm__)
#define TASK_NOINIT __attribute__((section(".noinit")))
#else
#define TASK_NOINIT
#endif

/*
 * Kept across a reset: the slots of a fault (their names are in the task table, which is
 * kept too), and the slot running now, which names the task after a hang.
 */
static TASK_NOINIT struct {
	uint16_t magic; /* TASK_FAULT_MAGIC: kind, code, task and by are set */
	uint16_t code;
	uint8_t kind;
	uint8_t task;
	uint8_t by;
	uint8_t run; /* the slot being called, TASK_NONE between calls */
} task_wdt;

static TASK_NOINIT task_t tasks[TASKS_MAX_COUNT];
static task_fault_t task_fault_last; /* what it held at start-up */
static bool task_wdt_late;	     /* a watched task was late: no more kicks */
static uint8_t task_hog = TASK_NONE; /* the longest call since the last check ... */
static uint32_t task_hog_us;	     /* ... and how long it took */
static uint32_t task_call_start;     /* when the running task's call began */
static bool task_call_busy;	     /* it called task_busy() */
#else
static task_t tasks[TASKS_MAX_COUNT];
#endif

static uint32_t task_loop_time = 0; /* duration of the last loop, in us */
static int current = -1; /* index of the running task, -1 outside a task */
static task_streams_t task_main = {-1, -1, -1, 0}; /* the streams outside a task */
static uint16_t task_reset; /* the port's reset register at start-up */
static uint8_t task_reason; /* a task_reset_t */

/* vfs: a new task holds its streams open, one that stopped lets them go */
extern void vfs_streams_hold(int in, int out) __attribute__((weak));
extern void vfs_streams_close(int in, int out) __attribute__((weak));
#if VFS_FD_TASK
extern void vfs_task_files_close(int slot) __attribute__((weak)); /* what it left open */
#endif

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

#if VFS_FD_TASK
	if (vfs_task_files_close != NULL) {
		vfs_task_files_close(i);
	}
#endif
}

/* -------------------------------------------------------------------------- */

void task_initialize(uint64_t (*handle)(void))
{
	if (handle == NULL) {
		return;
	}

	task_get_us_from_system = handle;

	task_reset = 0;
	task_reason = (uint8_t)port_wdt_reset_cause(&task_reset);

#if LETS_WDT
	task_fault_last.kind = 0;

	/* after a power-up the RAM holds anything; the task table is still the one before the reset */
	if (task_reason != TASK_RESET_POWER_ON && task_reason != TASK_RESET_BROWN_OUT) {
		uint8_t slot = TASK_NONE;
		uint8_t by = TASK_NONE;

		if (task_wdt.magic == TASK_FAULT_MAGIC) {
			task_fault_last.kind = task_wdt.kind;
			task_fault_last.code = task_wdt.code;
			slot = task_wdt.task;
			by = task_wdt.by;
		} else if (task_reason == TASK_RESET_WATCHDOG && task_wdt.run < TASKS_MAX_COUNT) {
			task_fault_last.kind = TASK_FAULT_HANG; /* no record: the loop never got back */
			task_fault_last.code = 0;
			slot = task_wdt.run;
		}

		task_fault_last.task[0] = '\0';
		task_fault_last.by[0] = '\0';

		if (slot < TASKS_MAX_COUNT) {
			memcpy(task_fault_last.task, tasks[slot].info.name, TASK_NAME_SIZE);
			task_fault_last.task[TASK_NAME_SIZE - 1] = '\0';
		}

		if (by < TASKS_MAX_COUNT && by != slot) {
			memcpy(task_fault_last.by, tasks[by].info.name, TASK_NAME_SIZE);
			task_fault_last.by[TASK_NAME_SIZE - 1] = '\0';
		}

		if (task_fault_last.kind == TASK_FAULT_TRAP) {
			task_reason = TASK_RESET_TRAP;
		}
	}

	task_wdt.magic = 0;
	task_wdt.run = TASK_NONE;
	task_wdt_late = false;
#endif

	/* what is above the stack pointer now: task_stack() reads how far it was used */
	port_stack_init();

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

/* stops the task in slot @p i: 0, or -1 when there is none (or it is stopping already) */
static inline __attribute__((always_inline)) int task_stop_slot(int i)
{
	task_t *t = &tasks[i];

	if (t->state == TASK_UNUSED || t->state == TASK_STOPPING) {
		return -1;
	}

	/* it is running: its memory and streams are in use until it returns */
	if (i == current) {
		t->state = TASK_STOPPING;
	} else {
		task_release(i);
	}

	return 0;
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

		return task_stop_slot(i);
	}

	return -1;
}

/* kill(2): any signal but 0 stops the task, as task_stop_byname() */
int kill(pid_t pid, int sig)
{
	if (pid < 0 || pid >= (pid_t)TASKS_MAX_COUNT) {
		return -1;
	}

	if (sig == 0) {
		return (tasks[pid].state == TASK_UNUSED || tasks[pid].state == TASK_STOPPING) ? -1 : 0;
	}

	return task_stop_slot((int)pid);
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
#if LETS_WDT
		memset(&t->info, 0, sizeof(t->info)); /* the table is not cleared at start-up */
		t->deadline = LETS_WDT_DEFAULT_MS;
		t->alive = (uint32_t)task_get_system_time();
#else
		t->info.worst = 0;
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
	return port_stack(peak, size);
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
static void task_fault_save(uint8_t kind, uint16_t code, int slot, uint8_t by)
{
	task_wdt.kind = kind;
	task_wdt.code = code;
	task_wdt.task = (slot >= 0 && slot < (int)TASKS_MAX_COUNT) ? (uint8_t)slot : TASK_NONE;
	task_wdt.by = by;
	task_wdt.magic = TASK_FAULT_MAGIC;
}

/*
 * true while no watched task is late: a ready task waits for its turn no longer than its
 * deadline, one with TASK_WATCH_ALIVE checks in within it, asleep or not
 */
static bool task_wdt_check(uint32_t now)
{
	uint8_t hog = task_hog;

	task_hog = TASK_NONE;
	task_hog_us = 0;

	for (int i = 0; i < TASKS_MAX_COUNT; i++) {
		task_t *t = &tasks[i];
		uint32_t since = t->alive;

		if (t->state == TASK_UNUSED || t->state == TASK_STOPPING || t->deadline == 0) {
			continue;
		}

		if (!(t->deadline & TASK_WATCH_ALIVE) && t->state == TASK_SLEEPING) {
			if ((int32_t)(now - t->wake_tick) < 0) {
				continue; /* asleep: not waiting for anything */
			}

			/* ready since its wake, or since a busy call kept it waiting */
			since = ((int32_t)(t->alive - t->wake_tick) > 0) ? t->alive : t->wake_tick;
		}

		if (now - since > (uint32_t)(t->deadline & ~TASK_WATCH_ALIVE) * 1000u) {
			task_fault_save(TASK_FAULT_LATE, 0, i,
					(t->deadline & TASK_WATCH_ALIVE) ? TASK_NONE : hog);
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
	t->deadline = (deadline_ms == 0)			 ? 0
		      : (deadline_ms < TASK_WATCH_ALIVE) ? (uint16_t)(deadline_ms | TASK_WATCH_ALIVE)
							 : (uint16_t)0xFFFFu;
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

void task_busy(void)
{
#if LETS_WDT
	if (current >= 0 && current < (int)TASKS_MAX_COUNT) {
		uint32_t now = (uint32_t)task_get_system_time();

		if (task_wdt_late || now - task_call_start > LETS_WDT_BUSY_MS * 1000ul) {
			return; /* late already, or far too long: a hang, let the watchdog fire */
		}

		tasks[current].alive = now;
		task_call_busy = true;
	}
#endif
	port_wdt_kick();
}

void task_trap(uint16_t code)
{
#if LETS_WDT
	task_fault_save(TASK_FAULT_TRAP, code, current, TASK_NONE);
#else
	(void)code;
#endif

	port_wdt_reset_now();
}

#if !defined(__XC16__)
/*
 * A port without a watchdog (a PC): nothing to drive, no reason. A PIC24 / dsPIC33 project
 * links port/pic24/wdt.c instead (no weak ones here, so a missing file does not link).
 */
void __attribute__((weak)) port_wdt_start(uint16_t ms)
{
	(void)ms;
}

void __attribute__((weak)) port_wdt_kick(void)
{
}

/* exit(): the port's _exit() resets the chip (Cortex-M); on a PC the process ends */
void __attribute__((weak)) port_wdt_reset_now(void)
{
	exit(0);
}

task_reset_t __attribute__((weak)) port_wdt_reset_cause(uint16_t *raw)
{
	(void)raw;

	return TASK_RESET_OTHER;
}

/* nothing known of the stack: top shows none, system() does not check */
void __attribute__((weak)) port_stack_init(void)
{
}

int __attribute__((weak)) port_stack(size_t *peak, size_t *size)
{
	(void)peak;
	(void)size;

	return -1;
}

size_t __attribute__((weak)) port_stack_left(void)
{
	return (size_t)-1;
}
#endif

/* reboot(2): the reset, at once; whoever asks syncs first (the `reboot` command does) */
int reboot(int cmd)
{
	(void)cmd;
	port_wdt_reset_now();
}

void tasks_start(void)
{
	uint64_t now = task_get_system_time();
	uint32_t loop_start = (uint32_t)now;

#if LETS_WDT
	/* the boot (rc) may have taken long: a ready task waits from now */
	for (int i = 0; i < TASKS_MAX_COUNT; i++) {
		tasks[i].alive = (uint32_t)now;
	}

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
#else
		port_wdt_kick();
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

#if LETS_WDT
			task_wdt.run = (uint8_t)current; /* names it if it never returns */
			task_call_start = start;
			task_call_busy = false;
#endif
			int r = t->entry(t->arg, now);

			now = task_get_system_time();

			uint32_t end = (uint32_t)now;

#if LETS_WDT
			task_wdt.run = TASK_NONE;

			if (end - start > task_hog_us) {
				task_hog_us = end - start;
				task_hog = (uint8_t)current;
			}

			if (!(t->deadline & TASK_WATCH_ALIVE)) {
				t->alive = end; /* ready again from now, if it does not sleep */
			}

			if (task_call_busy) {
				/* a long call that kept the watchdog quiet: the others wait from now */
				for (int i = 0; i < TASKS_MAX_COUNT; i++) {
					tasks[i].alive = end;
				}
			}
#endif

			/* negative result: the task is done; it may have stopped itself */
			if (r < 0 || t->state == TASK_STOPPING) {
				task_release(current);

				continue;
			}

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
