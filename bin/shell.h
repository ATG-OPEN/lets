/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef SHELL_H
#define SHELL_H

#include <stdbool.h>
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

/**
 * @brief Lists the names Tab may complete a word with.
 * @param word      The word being completed, as typed (it may be a path).
 * @param command   true for the first word of the line (a command to run).
 * @param candidate Call it with each name (the last part of a path only) and
 *                  whether it is a directory; it returns true when the name
 *                  starts with what is typed.
 */
typedef void (*shell_names_t)(const char *word, bool command,
			      bool (*candidate)(const char *name, bool dir));

/**
 * @brief The default: every name in the node table (for a command, the ones
 *        that execute). Directories are not known here.
 */
void shell_node_names(const char *word, bool command, bool (*candidate)(const char *, bool));

/**
 * @brief Replaces the names Tab uses, e.g. with ones that follow the
 *        directories of vfs (vfs_bin_initialize() does it). NULL: the default.
 */
void shell_names_set(shell_names_t names);

#endif /* SHELL_H */
