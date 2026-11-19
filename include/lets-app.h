/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file lets-app.h
 * @brief The one LETS piece of a program: running its step as a task.
 *
 * A program on LETS is a POSIX program (standard headers only: <fcntl.h>, <unistd.h>, <poll.h>,
 * <time.h>, ... from lets/include) whose command is `int NAME_main(int argc, char *argv[])`. LETS
 * tasks are cooperative, so a program that runs on does not loop in main(): main() opens its files
 * and hands a step to lets_spawn(); the step does what can be done now and returns the ms to sleep
 * (poll() wakes it sooner), or -1 when it ends:
 * @code
 * static int step(void *ctx)
 * {
 *     struct pollfd p = {fd, POLLIN, 0};
 *     ...
 *     return poll(&p, 1, 1000) ? 0 : 1000;
 * }
 * int echo_main(int argc, char *argv[]) { ...; return lets_spawn("echo", step, ctx); }
 * @endcode
 * The board makes NAME_main a command with LETS_COMMAND() (vfs.h is the board's, not the program's).
 */
#ifndef LETS_APP_H
#define LETS_APP_H

/** A program's step: the ms until it must run again, 0 at once, -1 it ends. */
typedef int (*lets_step_t)(void *ctx);

/** @brief Runs @p step(@p ctx) as the task @p name (`top`, `kill NAME`). @return 0, -1 none free. */
int lets_spawn(const char *name, lets_step_t step, void *ctx);

/**
 * Board code: a command for the program's `int FN(int argc, char *argv[])`, as vfs_register_app()
 * takes it: `vfs_register_app("/opt/echo", LETS_COMMAND_OF(echo_main));` after `LETS_COMMAND(echo_main)`.
 */
#define LETS_COMMAND(fn)                                                        \
	int fn(int argc, char *argv[]);                                         \
	static int fn##_command(void *args, int argc, char *argv[])             \
	{                                                                       \
		(void)args;                                                     \
		return fn(argc, argv);                                          \
	}
#define LETS_COMMAND_OF(fn) fn##_command

#endif
