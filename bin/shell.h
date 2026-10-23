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
 * After `std <node>` one read() is one command line, with no echo: the mode
 * for a program on the other end. After `std -t <node>` it is a terminal for
 * a person (screen, PuTTY): a prompt, echo, Backspace, Ctrl-U, Ctrl-C,
 * Up / Down for the last SHELL_HISTORY lines (default 2, 128 bytes each, taken
 * from the heap when the terminal is first used; -DSHELL_HISTORY=0 for none)
 * and Tab to complete a name.
 *
 * @param args A startup script (lines of commands, 0 terminated) run once by
 *             a "startup" task, or NULL for none.
 * @return 0.
 */
int shell_initialize(void *args);

#endif /* SHELL_H */
