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
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>

#ifndef TASKS_MAX_COUNT
#define TASKS_MAX_COUNT 16u /**< Size of the task table: a project can set its own. */
#endif
#define TASK_NAME_SIZE	16u /**< Name length, including the terminating 0. */

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

/** One slot of the task table. */
typedef struct {
	/** The task function: gets its argument and the current time in us. */
	int (*entry)(void *arg, uint64_t system_time);
	void *arg;		/**< Argument passed to entry(), owned by the task. */
	task_streams_t streams; /**< stdin, stdout, working directory. */
	uint32_t wake_tick;	/**< While sleeping: the time to wake up, in us, wraps. */
	uint8_t state;		/**< A task_state_t. */
	task_info_t info;	/**< Statistics. */
} task_t;

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

/** @brief RCON at start-up: why the chip last reset (0 on a PC). */
uint16_t task_reset_cause(void);

/** @brief Runs the scheduler loop. Never returns. */
void tasks_start(void);

/** @brief Returns the task table (TASKS_MAX_COUNT entries), for `top`. */
task_t *task_get_data(void);

/** @brief Returns the index of the running task, or -1 outside a task. */
int task_get_current(void);

/** @brief Returns the streams of the running task, or of main() outside a task. */
task_streams_t *task_streams(void);

#endif /* TASK_H */
