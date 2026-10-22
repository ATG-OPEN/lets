/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>

/**
 * @brief Registers the built-in commands and starts the shell.
 *
 * The shell is a task that reads command lines from stdin and runs each with
 * system(). Commands: cat, echo, kill, ls, mv, top, touch, rm (plus std and
 * reboot, built into the kernel).
 *
 * @param args A startup script (lines of commands, 0 terminated) run once by
 *             a "startup" task, or NULL for none.
 * @return 0.
 */
int shell_initialize(void *args);

#endif /* SHELL_H */
