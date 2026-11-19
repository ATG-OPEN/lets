/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file task.h
 * @brief Cooperative task scheduler.
 *
 * A task is a plain C function that does a small piece of work and returns.
 * The scheduler calls every task in turn, forever, in one loop. There is no
 * preemption and no per-task stack, so tasks never need locks, but a task must
 * never block: it returns instead and tells the scheduler when to call it
 * again.
 *
 * The value a task returns decides what happens next:
 *
 *   > 0   sleep that many milliseconds, then run again
 *   = 0   run again on the next loop
 *   < 0   stop: the task is removed and the memory it owns is freed
 *
 * Example:
 * @code
 * static int blink(void *arg, uint64_t now)
 * {
 *         LED_Toggle();
 *         return 500;                    // again in 500 ms
 * }
 *
 * task_create("blink", blink, NULL);
 * @endcode
 */

#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "../include/lets/task.h"
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h> /* pid_t: kill() */

#ifndef TASKS_MAX_COUNT
#define TASKS_MAX_COUNT 16u /**< Size of the task table: a project can set its own. */
#endif
#define TASK_NAME_SIZE	16u /**< Name length, including the terminating 0. */

#ifndef LETS_WDT
#define LETS_WDT 0 /**< 1: the scheduler runs the hardware watchdog (see task_watch()). */
#endif
#ifndef LETS_WDT_MS
#define LETS_WDT_MS 2000u /**< Watchdog period asked of the port (a PIC24 takes its config bits). */
#endif
#ifndef LETS_WDT_DEFAULT_MS
#define LETS_WDT_DEFAULT_MS 1000u /**< How long a ready task may wait for its turn (task_watch()). */
#endif
#ifndef LETS_WDT_BUSY_MS
#define LETS_WDT_BUSY_MS 30000u /**< The longest call task_busy() may keep the watchdog quiet for. */
#endif
#define TASK_WATCH_ALIVE 0x8000u /**< In task_t.deadline: the task checks in with task_alive(). */

/** State of a slot in the task table. */
typedef enum {
	TASK_UNUSED = 0,   /**< Free slot. */
	TASK_RUNNING = 1,  /**< Called on every loop. */
	TASK_SLEEPING = 3, /**< Waits until its wake time. */
	TASK_STOPPING = 4, /**< Stopped itself: released when it returns. */
} task_state_t;

/** Statistics of one task, shown by the `top` command, in us. */
typedef struct {
	char name[TASK_NAME_SIZE]; /**< Name, 0 terminated. */
	uint32_t duration;	   /**< Time the last call took. */
	uint32_t worst;		   /**< Longest call so far: what the other tasks had to wait. */
	uint32_t delay;		   /**< Time between the last two calls. */
	uint32_t last_call_time;   /**< Start of the last call. */
} task_info_t;

/**
 * What a task inherits from whoever created it. A negative in or out follows
 * the console, set by `std`: in and out hold a handle, which the task holds
 * open. terminal is 0 or 1, negative to follow the console. cwd is the working
 * directory, a vfs directory id.
 */
typedef struct {
	int8_t in;
	int8_t out;
	int8_t terminal;
	uint8_t cwd;
} task_streams_t;

/** One slot of the task table (task_t: lets/task.h). */
struct task {
	/** The task function: gets its argument and the current time in us. */
	int (*entry)(void *arg, uint64_t system_time);
	void *arg;		/**< Argument passed to entry(), owned by the task. */
	task_streams_t streams; /**< stdin, stdout, working directory. */
	uint32_t wake_tick;	/**< While sleeping: the time to wake up, in us, wraps. */
	uint8_t state;		/**< A task_state_t. */
	task_info_t info;	/**< Statistics. */
#if LETS_WDT
	/** ms it may wait while ready (or, with TASK_WATCH_ALIVE, go without task_alive()); 0: not watched. */
	uint16_t deadline;
	uint32_t alive; /**< End of its last call (or its last task_alive()), in us, wraps. */
#endif
};

/** Why the chip last reset: task_reset_reason(). */
typedef enum {
	TASK_RESET_OTHER = 0,
	TASK_RESET_POWER_ON,
	TASK_RESET_BROWN_OUT,
	TASK_RESET_PIN,
	TASK_RESET_SOFTWARE,
	TASK_RESET_WATCHDOG,
	TASK_RESET_TRAP,
} task_reset_t;

/** What the kernel saw just before the chip reset (LETS_WDT), kept across the reset. */
typedef struct {
	uint16_t kind;		   /**< TASK_FAULT_LATE, TASK_FAULT_TRAP or TASK_FAULT_HANG. */
	uint16_t code;		   /**< The trap's number (as given to task_trap()). */
	char task[TASK_NAME_SIZE]; /**< The task late, trapped or hung, "" outside a task. */
	char by[TASK_NAME_SIZE];   /**< LATE while ready: the task with the longest call then. */
} task_fault_t;

#define TASK_FAULT_LATE 1u /**< A watched task missed its deadline: the kicks stopped. */
#define TASK_FAULT_TRAP 2u /**< task_trap(): a CPU trap, then a reset. */
#define TASK_FAULT_HANG 3u /**< The watchdog fired while this task ran: it never returned. */

/**
 * @brief Prepares the scheduler. Call once, before any other task function.
 * @param handle Clock: returns the time since start-up in microseconds.
 */
void task_initialize(uint64_t (*handle)(void));

/** @brief Returns the time since start-up in microseconds (0 before init). */
uint64_t task_get_system_time(void);

/** @brief Returns the number of tasks in the table. */
int task_count(void);

/**
 * @brief Stops the first task with this name and frees its memory.
 *
 * A task that stops itself keeps running until it returns, and its memory stays
 * until then: it looks at task_stopping() and returns.
 *
 * @return 0, or -1 when there is no such task.
 */
int task_stop_byname(char *name);

/**
 * @brief Stops the task called @p name whose argument is @p arg, as task_stop_byname():
 * one of several tasks of a name, e.g. a shell of its memory.
 * @return 0, or -1 when there is no such task.
 */
int task_stop_arg(const char *name, const void *arg);

/**
 * @brief kill(2): stops the task whose ID (its slot, as `top` and /proc show it) is @p pid,
 * as task_stop_byname(); @p sig 0 only asks whether it is there. Any other signal stops it
 * (SIGTERM, SIGKILL: a cooperative task has nothing to catch it with). Note that pid 0 is a
 * task here, not a process group.
 * @return 0, or -1 when there is no such task.
 */
int kill(pid_t pid, int sig);

/** @brief Ends the sleep of the task in @p slot: it runs on this loop (poll(), vfs.c). */
void task_wake(int slot);

/** @brief true when the running task has stopped itself and must return. */
bool task_stopping(void);

/**
 * @brief malloc() for memory of the running task: freed when the task stops.
 *
 * Plain malloc() has no owner, because what a command allocates usually outlives
 * the task that ran it: a RAM file made by rc, a mounted file system. So besides
 * the argument given to task_create(), only this is freed with the task.
 */
void *task_alloc(size_t size);

/**
 * @brief Adds a task. It is first called on the next loop.
 *
 * If @p arg points to memory from malloc(), the task becomes its owner: the
 * memory is freed automatically when the task stops.
 *
 * @param name  Name shown by `top`, also used by task_stop_byname().
 * @param entry The task function.
 * @param arg   Passed to every call of @p entry.
 * @return The new task, or NULL when the table is full or an argument is NULL.
 */
task_t *task_create(const char *name, int (*entry)(void *arg, uint64_t system_time), void *arg);

/** @brief Returns how long the last full loop over all tasks took, in ms. */
float task_get_loop_time(void);

/**
 * @brief The most stack used since start-up, seen by the pattern left in it.
 * @param peak Bytes used at the worst moment.
 * @param size Bytes there are.
 * @return 0, or -1 on a PC.
 */
int task_stack(size_t *peak, size_t *size);

/** @brief The port's reset register at start-up (RCON, EMU RSTCAUSE; 0 on a PC). */
uint16_t task_reset_cause(void);

/** @brief Why the chip last reset, the same on every port. */
task_reset_t task_reset_reason(void);

/**
 * @brief The fault before the last reset, or NULL when the reset had none (or LETS_WDT is 0).
 */
const task_fault_t *task_fault(void);

/**
 * @brief Changes how the watchdog (LETS_WDT) watches a task.
 *
 * Every task is watched from task_create(): once it is ready (it returned 0, or its sleep is
 * over) it must get its turn within LETS_WDT_DEFAULT_MS, or the scheduler stops kicking the
 * watchdog and the chip resets (`top`: `watchdog (late: TASK, by: the task with the longest call)`).
 * A sleeping task is not late, however long it sleeps. A task that never returns stops every
 * kick: `watchdog (hang: TASK)`.
 *
 * task_watch() asks for more: the task must call task_alive() at least every @p deadline_ms,
 * sleeping included (make it longer than its longest sleep), so a task that runs but makes no
 * progress resets the chip too. With LETS_WDT 0 this does nothing.
 *
 * @param t           The task, NULL for the running one.
 * @param deadline_ms 1 to 32767 ms between task_alive() calls, or 0: not watched at all (not
 *                    even the default; for a task allowed to wait for its turn).
 * @return 0, or -1 when there is no such task.
 */
int task_watch(task_t *t, uint16_t deadline_ms);

/** @brief The running task checks in: it is alive and making progress (see task_watch()). */
void task_alive(void);

/**
 * @brief The running task is in one long call that is making progress: kick the watchdog now.
 *
 * For work that cannot return to the scheduler in between, such as a flash driver copying
 * page after page (a /boot rewrite): call it between steps that each take well under the
 * hardware period. It kicks the watchdog (port_wdt_kick(), with LETS_WDT 0 too) and checks
 * the task in (task_alive()). The other tasks, kept waiting by such a call, wait from its end:
 * none of them is late because of it. A call longer than LETS_WDT_BUSY_MS is taken for a
 * hang: task_busy() stops kicking, and the reset says `watchdog (hang: TASK)`.
 */
void task_busy(void);

/**
 * @brief Records a CPU trap with the running task and resets the chip (port_wdt_reset_now()).
 * With LETS_WDT on a PIC24, port/pic24/wdt.c also defines MCC's TRAPS_halt_on_error() to call
 * it (unless LETS_WDT_NO_TRAPS), so a trap restarts the board.
 */
void task_trap(uint16_t code) __attribute__((noreturn));

/*
 * The watchdog's port hooks, as the clock is the port's (docs/porting.md "Watchdog"): every
 * decision is the kernel's, the port only drives the hardware. port/pic24/wdt.c (PIC24 /
 * dsPIC33), port/efr32; a port without them (a PC) gets the kernel's empty ones (weak).
 */
/** Starts the hardware watchdog with a period of about @p ms (LETS_WDT only, once). */
void port_wdt_start(uint16_t ms);
/** Clears it: the scheduler calls it once a loop (with LETS_WDT 0 too); a no-op before the start. */
void port_wdt_kick(void);
/** Resets the chip now (task_trap()); does not return. */
void port_wdt_reset_now(void) __attribute__((noreturn));
/** Why the chip reset, and the raw register in @p raw (0 if none): once, at task_initialize(). */
task_reset_t port_wdt_reset_cause(uint16_t *raw);

/*
 * The stack's port hooks: a port that knows its stack (PIC24: port/pic24/wdt.c) measures it;
 * the kernel's weak ones (a PC, Cortex-M) know nothing.
 */
/** Marks the stack not used yet, for port_stack(); once, at task_initialize(). */
void port_stack_init(void);
/** The most stack used since port_stack_init() and the stack's size, in bytes: 0, or -1. */
int port_stack(size_t *peak, size_t *size);
/** Bytes of stack left now, below its limit: system() refuses a command with too few. */
size_t port_stack_left(void);

/**
 * @brief Resets the board now, as Linux's reboot(2): nothing is synced, the caller syncs
 * first (the `reboot` command: sync(), the ttys drain, sync(), reboot()). @p cmd is ignored
 * (RB_AUTOBOOT). Through port_wdt_reset_now(); does not return.
 */
int reboot(int cmd) __attribute__((noreturn));

/** @brief Runs the scheduler loop. Never returns. */
void tasks_start(void);

/** @brief Returns the task table (TASKS_MAX_COUNT entries), for `top`. */
task_t *task_get_data(void);

/** @brief Returns the index of the running task, or -1 outside a task. */
int task_get_current(void);

/** @brief Returns the streams of the running task, or of main() outside a task. */
task_streams_t *task_streams(void);

#endif /* TASK_H */
