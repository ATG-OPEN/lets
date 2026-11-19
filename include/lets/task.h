/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file lets/task.h
 * @brief LETS's process calls for programs that start tasks of their own (argscript, lines, reboot):
 * the part of kernel/task.h a program may use, with standard headers only. A plain program uses
 * <lets-app.h> (lets_spawn()) instead.
 */
#ifndef LETS_INCLUDE_LETS_TASK_H
#define LETS_INCLUDE_LETS_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** A task (the kernel's table slot; programs keep only the pointer). */
typedef struct task task_t;

/**
 * @brief Starts @p entry(@p arg, now_us) as the task @p name: it returns the ms to sleep, 0 to run
 * again at once, -1 to end. @return the task, NULL when no slot is free.
 */
task_t *task_create(const char *name, int (*entry)(void *arg, uint64_t system_time), void *arg);

/** @brief malloc() whose block belongs to the running task: freed when the task ends. */
void *task_alloc(size_t size);

/** @brief true while the running task is being stopped (kill, Ctrl-C): it should end. */
bool task_stopping(void);

/** @brief The shell (of the running command) waits until @p t ends before its next prompt. */
void shell_wait(task_t *t);

#endif
