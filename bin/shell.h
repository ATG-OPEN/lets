/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef SHELL_H
#define SHELL_H

#include <stdbool.h>
#include <stdint.h>

#include "../kernel/task.h"

/**
 * @brief Registers the built-in commands and starts the shell.
 *
 * The shell is a task that reads command lines from stdin and runs each with
 * system(). The commands (ls, cat, cd, kill, top, std, passwd, reboot, ...) are
 * the built-in ones of vfs/bin.c, in flash; vfs_bin_initialize() adds the prompt
 * with the working directory and Tab on paths.
 *
 * This shell is on the console, where `std` points. The command `shell` starts
 * more, each on its own streams: `shell -t < ttyS1 > ttyS1`.
 *
 * After `std <node>`, or for a `shell`, one read() is one command line, with no
 * echo: the mode for a program on the other end. After each command, also one
 * that wrote nothing, it sends the prompt (as a terminal shows it, "/tmp$ "), so
 * the program knows the command is done. After `std -t <node>`, or for a
 * `shell -t`, it is a terminal for a person (screen, PuTTY): a prompt, echo, Backspace, Ctrl-U, Ctrl-C,
 * Up / Down for the last SHELL_HISTORY lines (default 2, 128 bytes each, taken
 * from the heap when the terminal is first used; -DSHELL_HISTORY=0 for none)
 * and Tab to complete a name.
 *
 * -DSHELL_NO_TERMINAL leaves the terminal mode out, for a full flash: a program
 * or the node `sys` (sys.h) gives the commands.
 *
 * The board's startup is not the shell's: main() runs it after its inits,
 * e.g. system("/boot/rc") with argscript as the interpreter.
 *
 * @return 0.
 */
int shell_initialize(void);

/**
 * @brief Makes the shell a command was typed into wait for the task @p t.
 *
 * For a command that leaves its work to a task, such as `top`: until @p t ends
 * the shell shows no prompt and reads no input (on a terminal Ctrl-C stops
 * @p t), so the task's output comes before the next prompt and the next
 * command. Outside a command typed into a shell (rc, argscript) it does nothing.
 */
void shell_wait(task_t *t);

/**
 * @brief The `passwd` command: passwd WORD, in the startup script, locks every shell
 *        until WORD is given on its input; `passwd -` switches the password off.
 *
 * **A default is built in: SHELL_PASSWORD_DEFAULT, "123456".** It is known to everyone:
 * set your own with `passwd WORD` in the startup script (or build with
 * -DSHELL_PASSWORD_DEFAULT=\"...\"). Without a `passwd` line every shell asks for the
 * default. -DSHELL_PASSWORD_DEFAULT=\"\" builds without a default (no password until
 * `passwd WORD`).
 *
 * A shell asks once per input stream (a tty: once after power-up), and again when its
 * input moves (`std`) or the input's driver signals a new peer (vfs_peer_new(), e.g. a
 * packet port whose packets now come from another address). A terminal shows
 * "Password: " and echoes nothing; a program link gets "Password:" back for each line
 * it sent while locked, and the prompt when the word is taken. A wrong word runs nothing, drops the rest of what was read and
 * makes the shell read nothing for SHELL_PASSWORD_DELAY ms (2000). Only a 32-bit hash
 * of the word is kept in RAM. -DSHELL_NO_PASSWORD leaves it out.
 *
 * It guards what a shell reads, nothing else: the word goes in clear over the link, and
 * whatever else runs commands or writes files is outside it (the node `sys` of bin/sys.c runs
 * a line written to it; a service that can write /boot/rc or a script in /home runs it at the
 * next boot or call). Keep those off a link a stranger can reach. A new `passwd` does not log
 * out a shell that was given the old word.
 */
int shell_passwd(void *args, int argc, char *argv[]);

/** @brief The `shell` command: shell [-t] < in > out, a shell on these streams. */
int shell_start(void *args, int argc, char *argv[]);

/**
 * @brief Replaces the prompt (default "> "), of a terminal and of a link.
 *
 * @p prompt prints it with write(2, ...); vfs_bin_initialize() installs one
 * that shows the working directory, as a Linux shell does ("/tmp$ ").
 * NULL: the default.
 */
void shell_prompt_set(void (*prompt)(void));

/**
 * @brief Writes to the terminal with the shell's echo: a prompt uses it, so
 *        the prompt and the line go out together, in one write.
 */
void shell_write(const void *data, unsigned int length);

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
